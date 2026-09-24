#include "desk_robot_camera.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "board.h"
#include "display.h"
#include "lvgl_display/lvgl_image.h"

#define TAG "DeskRobotCamera"

namespace {

constexpr int kMcpFreshWarmupFrames = 2;
constexpr int kMcpLowLightWarmupFrames = 4;
constexpr int kMcpHighResolutionWarmupFrames = 8;

bool IsHighResolution(CameraResolution resolution) {
    return resolution == CameraResolution::kXga ||
           resolution == CameraResolution::kSxga ||
           resolution == CameraResolution::kUxga;
}

}  // namespace

DeskRobotCamera::DeskRobotCamera(const camera_config_t& config, std::mutex& shared_i2c_mutex,
                                 const CameraSettingsConfig& settings,
                                 CameraImagePolicy& image_policy,
                                 McpStateCallback mcp_state_callback)
    : Esp32Camera(config, &shared_i2c_mutex),
      settings_(CameraSettingsStore::Normalize(settings)),
      image_policy_(image_policy),
      mcp_state_callback_(std::move(mcp_state_callback)) {
    if (Esp32Camera::IsAvailable() &&
        !ApplyModeSensorSettings(settings_.sensor)) {
        ESP_LOGW(TAG, "Some persisted camera sensor settings were rejected");
    }
}

bool DeskRobotCamera::Capture() {
    bool preview_preempted = false;
    if (!BeginMcpOperation(preview_preempted)) {
        ESP_LOGW(TAG, "MCP camera capture rejected: another operation is active");
        return false;
    }
    const int64_t capture_start_us = esp_timer_get_time();
    mcp_request_started_us_.store(capture_start_us, std::memory_order_relaxed);
    mcp_capture_ms_.store(0, std::memory_order_relaxed);
    mcp_vision_ms_.store(0, std::memory_order_relaxed);
    mcp_image_bytes_.store(0, std::memory_order_relaxed);
    mcp_response_bytes_.store(0, std::memory_order_relaxed);
    UpdateMcpRequestState(McpRequestState::kCapturing);
    if (preview_preempted) {
        HidePreviewImage();
    }

    std::unique_lock<std::timed_mutex> lock(capture_mutex_, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::seconds(7))) {
        ESP_LOGE(TAG, "MCP camera capture timed out waiting for live preview");
        mcp_capture_ms_.store(
            static_cast<uint32_t>((esp_timer_get_time() - capture_start_us) / 1000),
            std::memory_order_relaxed);
        UpdateMcpRequestState(McpRequestState::kFailed);
        EndMcpOperation();
        return false;
    }
    // A preview capture that was already inside the hardware critical section
    // may have posted its display image after the first hide.
    if (preview_preempted) {
        HidePreviewImage();
    }
    if (mcp_capture_pending_) {
        ESP_LOGW(TAG, "MCP camera capture rejected: previous snapshot is still pending");
        UpdateMcpRequestState(McpRequestState::kFailed);
        EndMcpOperation();
        return false;
    }
    const CameraSettingsConfig settings = GetSettings();
    const CameraSensorSettings resolved_sensor = ResolveSensorSettings(settings.sensor);
    // Change the sensor mode first because OV2640 set_framesize() rewrites its
    // DVP/pixformat path. Apply the requested image controls to the final mode.
    const bool configured =
        ApplyModeCaptureSettings(settings.mcp.resolution, settings.mcp.jpeg_quality) &&
        Esp32Camera::ApplySensorControls(ToSensorControls(resolved_sensor));
    int warmup_frames = 0;
    if (settings.mcp.freshness == McpFreshFramePolicy::kFresh) {
        warmup_frames = resolved_sensor.profile == CameraImageProfile::kLowLight
                            ? kMcpLowLightWarmupFrames
                            : kMcpFreshWarmupFrames;
        if (IsHighResolution(settings.mcp.resolution)) {
            warmup_frames = std::max(warmup_frames, kMcpHighResolutionWarmupFrames);
        }
    }
    size_t image_bytes = 0;
    const bool captured =
        configured && Esp32Camera::CaptureOwnedJpeg(warmup_frames, &image_bytes);
    const int64_t capture_ms = (esp_timer_get_time() - capture_start_us) / 1000;
    mcp_capture_ms_.store(static_cast<uint32_t>(std::max<int64_t>(0, capture_ms)),
                          std::memory_order_relaxed);
    mcp_image_bytes_.store(image_bytes, std::memory_order_relaxed);
    if (!captured) {
        UpdateMcpRequestState(McpRequestState::kFailed);
    }
    ESP_LOGI(TAG,
             "camera_mcp stage=camera_capture_completed success=%d elapsed_ms=%lld "
             "free_internal=%zu free_psram=%zu",
             captured ? 1 : 0, static_cast<long long>(capture_ms),
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    mcp_capture_pending_ = captured;
    if (!captured) {
        EndMcpOperation();
    }
    return captured;
}

bool DeskRobotCamera::StartWebLive() {
    bool hide_mochan = false;
    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        if (mcp_operation_active_.load()) {
            return false;
        }
        if (preview_mode_.load() == PreviewMode::kWebLive) {
            return true;
        }
        std::unique_lock<std::timed_mutex> capture_lock(capture_mutex_, std::defer_lock);
        if (!capture_lock.try_lock_for(std::chrono::seconds(7))) {
            return false;
        }
        const CameraSettingsConfig settings = GetSettings();
        if (!ApplyModeCaptureSettings(settings.web.resolution, settings.web.jpeg_quality) ||
            !ApplyModeSensorSettings(settings.sensor)) {
            return false;
        }
        hide_mochan = preview_mode_.load() == PreviewMode::kMochanPreview;
        preview_mode_.store(PreviewMode::kWebLive);
    }
    if (hide_mochan) {
        HidePreviewImage();
    }
    return true;
}

void DeskRobotCamera::StopWebLive() {
    std::lock_guard<std::mutex> lock(ownership_mutex_);
    if (preview_mode_.load() == PreviewMode::kWebLive) {
        preview_mode_.store(PreviewMode::kOff);
    }
}

bool DeskRobotCamera::StartMochanPreview() {
    std::lock_guard<std::mutex> lock(ownership_mutex_);
    if (mcp_operation_active_.load()) {
        return false;
    }
    if (preview_mode_.load() == PreviewMode::kMochanPreview) {
        return true;
    }
    std::unique_lock<std::timed_mutex> capture_lock(capture_mutex_, std::defer_lock);
    if (!capture_lock.try_lock_for(std::chrono::seconds(7))) {
        return false;
    }
    const CameraSettingsConfig settings = GetSettings();
    if (!ApplyModeCaptureSettings(settings.mochan.source_resolution, 12) ||
        !ApplyModeSensorSettings(settings.sensor)) {
        return false;
    }
    preview_mode_.store(PreviewMode::kMochanPreview);
    return true;
}

void DeskRobotCamera::StopMochanPreview() {
    bool hide_preview = false;
    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        if (preview_mode_.load() == PreviewMode::kMochanPreview) {
            preview_mode_.store(PreviewMode::kOff);
            hide_preview = true;
        }
    }
    if (hide_preview) {
        HidePreviewImage();
    }
}

void DeskRobotCamera::ForceOff() {
    bool hide_preview = false;
    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        hide_preview = preview_mode_.load() == PreviewMode::kMochanPreview;
        preview_mode_.store(PreviewMode::kOff);
    }
    if (hide_preview) {
        HidePreviewImage();
    }
}

bool DeskRobotCamera::CapturePreview() {
    std::unique_lock<std::timed_mutex> lock(capture_mutex_, std::try_to_lock);
    if (!lock.owns_lock() || mcp_operation_active_.load() || !IsMochanPreviewActive()) {
        return false;
    }
    const bool captured = Esp32Camera::CaptureForPreview();
    ReturnCurrentFrame();
    if (!IsMochanPreviewActive()) {
        HidePreviewImage();
        return false;
    }
    return captured;
}

bool DeskRobotCamera::SendWebLiveFrame(const JpegSender& sender) {
    std::unique_lock<std::timed_mutex> lock(capture_mutex_, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::seconds(7)) || mcp_operation_active_.load() ||
        !IsWebLiveActive()) {
        return false;
    }
    if (!Esp32Camera::CaptureForWeb()) {
        return false;
    }
    const uint8_t* data = nullptr;
    size_t length = 0;
    const bool sent = IsWebLiveActive() &&
                      Esp32Camera::GetCurrentJpeg(data, length) && sender(data, length);
    ReturnCurrentFrame();
    return sent;
}

bool DeskRobotCamera::SendSnapshot(const JpegSender& sender) {
    // A manual browser snapshot takes ownership from Mochan. It remains a
    // one-shot operation and does not enter or resume a persistent preview mode.
    if (IsMochanPreviewActive()) {
        StopMochanPreview();
    }
    if (IsWebLiveActive()) {
        return false;
    }
    std::unique_lock<std::timed_mutex> lock(capture_mutex_, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::seconds(7)) || mcp_operation_active_.load()) {
        return false;
    }
    const CameraSettingsConfig settings = GetSettings();
    if (!ApplyModeCaptureSettings(settings.web.resolution, settings.web.jpeg_quality) ||
        !ApplyModeSensorSettings(settings.sensor)) {
        return false;
    }
    if (!Esp32Camera::CaptureForWeb()) {
        return false;
    }
    const uint8_t* data = nullptr;
    size_t length = 0;
    const bool sent = Esp32Camera::GetCurrentJpeg(data, length) && sender(data, length);
    ReturnCurrentFrame();
    return sent;
}

bool DeskRobotCamera::IsAvailable() const { return Esp32Camera::IsAvailable(); }

bool DeskRobotCamera::ApplySettings(const CameraSettingsConfig& requested) {
    ForceOff();
    std::unique_lock<std::timed_mutex> capture_lock(capture_mutex_, std::defer_lock);
    if (!capture_lock.try_lock_for(std::chrono::seconds(7)) || mcp_operation_active_.load()) {
        return false;
    }
    const CameraSettingsConfig normalized = CameraSettingsStore::Normalize(requested);
    if (!ApplyModeSensorSettings(normalized.sensor)) {
        return false;
    }
    std::lock_guard<std::mutex> settings_lock(settings_mutex_);
    settings_ = normalized;
    return true;
}

CameraSettingsConfig DeskRobotCamera::GetSettings() const {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    return settings_;
}

int DeskRobotCamera::WebFrameIntervalMs() const {
    const int fps = std::max(1, GetSettings().web.fps);
    return 1000 / fps;
}

const char* DeskRobotCamera::SensorName() const {
    return SensorPid() == OV2640_PID ? "OV2640" : "Unknown";
}

CameraImagePolicy::Status DeskRobotCamera::GetImagePolicyStatus() const {
    return image_policy_.GetStatus(esp_timer_get_time());
}

DeskRobotCamera::McpRequestHealth DeskRobotCamera::GetMcpRequestHealth() const {
    McpRequestHealth health;
    health.state = mcp_request_state_.load(std::memory_order_acquire);
    health.started_us = mcp_request_started_us_.load(std::memory_order_relaxed);
    health.capture_ms = mcp_capture_ms_.load(std::memory_order_relaxed);
    health.vision_ms = mcp_vision_ms_.load(std::memory_order_relaxed);
    health.image_bytes = mcp_image_bytes_.load(std::memory_order_relaxed);
    health.response_bytes = mcp_response_bytes_.load(std::memory_order_relaxed);
    return health;
}

const char* DeskRobotCamera::McpRequestStateName(McpRequestState state) {
    switch (state) {
        case McpRequestState::kCapturing:
            return "capturing";
        case McpRequestState::kAnalyzing:
            return "analyzing";
        case McpRequestState::kSucceeded:
            return "succeeded";
        case McpRequestState::kFailed:
            return "failed";
        case McpRequestState::kNever:
        default:
            return "never";
    }
}

void DeskRobotCamera::UpdateMcpRequestState(McpRequestState state) {
    mcp_request_state_.store(state, std::memory_order_release);
    if (mcp_state_callback_) {
        mcp_state_callback_(state);
    }
}

std::expected<std::string, std::string> DeskRobotCamera::Explain(const std::string& question) {
    if (!mcp_capture_pending_.exchange(false)) {
        UpdateMcpRequestState(McpRequestState::kFailed);
        EndMcpOperation();
        return std::unexpected("No MCP camera snapshot is pending");
    }
    ESP_LOGD(TAG, "MCP camera explain begin");
    const int64_t vision_start_us = esp_timer_get_time();
    UpdateMcpRequestState(McpRequestState::kAnalyzing);
    auto result = Esp32Camera::Explain(question);
    const int64_t vision_ms = (esp_timer_get_time() - vision_start_us) / 1000;
    mcp_vision_ms_.store(static_cast<uint32_t>(std::max<int64_t>(0, vision_ms)),
                         std::memory_order_relaxed);
    if (result) {
        mcp_response_bytes_.store(result->size(), std::memory_order_relaxed);
        UpdateMcpRequestState(McpRequestState::kSucceeded);
        ESP_LOGI(TAG, "MCP camera explain done");
    } else {
        mcp_response_bytes_.store(0, std::memory_order_relaxed);
        UpdateMcpRequestState(McpRequestState::kFailed);
        ESP_LOGE(TAG, "MCP camera explain failed");
    }
    return result;
}

void DeskRobotCamera::OnMcpResponseSent() {
    if (!mcp_operation_active_.load()) {
        return;
    }
    ESP_LOGD(TAG, "MCP camera response sent");
    EndMcpOperation();
}

bool DeskRobotCamera::BeginMcpOperation(bool& preview_preempted) {
    std::lock_guard<std::mutex> lock(ownership_mutex_);
    if (mcp_operation_active_.load()) {
        preview_preempted = false;
        return false;
    }
    preview_preempted = preview_mode_.load() != PreviewMode::kOff;
    preview_mode_.store(PreviewMode::kOff);
    mcp_operation_active_.store(true);
    return true;
}

void DeskRobotCamera::EndMcpOperation() {
    std::lock_guard<std::mutex> lock(ownership_mutex_);
    mcp_operation_active_.store(false);
}

void DeskRobotCamera::HidePreviewImage() {
    auto* display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->SetPreviewImage(nullptr);
    }
}

bool DeskRobotCamera::ApplyModeCaptureSettings(CameraResolution resolution, int jpeg_quality) {
    return Esp32Camera::ApplyCaptureSettings(ToFrameSize(resolution), jpeg_quality);
}

bool DeskRobotCamera::ApplyModeSensorSettings(const CameraSensorSettings& settings) {
    return Esp32Camera::ApplySensorControls(ToSensorControls(ResolveSensorSettings(settings)));
}

CameraSensorSettings DeskRobotCamera::ResolveSensorSettings(
    const CameraSensorSettings& settings) const {
    if (settings.profile != CameraImageProfile::kAuto) {
        return settings;
    }
    CameraSettingsConfig resolved;
    resolved.sensor = settings;
    resolved.sensor.profile = image_policy_.EffectiveProfile(esp_timer_get_time());
    return CameraSettingsStore::Normalize(resolved).sensor;
}

framesize_t DeskRobotCamera::ToFrameSize(CameraResolution resolution) {
    switch (resolution) {
        case CameraResolution::kQvga:
            return FRAMESIZE_QVGA;
        case CameraResolution::kHvga:
            return FRAMESIZE_HVGA;
        case CameraResolution::kSvga:
            return FRAMESIZE_SVGA;
        case CameraResolution::kXga:
            return FRAMESIZE_XGA;
        case CameraResolution::kSxga:
            return FRAMESIZE_SXGA;
        case CameraResolution::kUxga:
            return FRAMESIZE_UXGA;
        case CameraResolution::kAuto:
        case CameraResolution::kVga:
        default:
            return FRAMESIZE_VGA;
    }
}

gainceiling_t DeskRobotCamera::ToGainCeiling(CameraGainCeiling ceiling) {
    switch (ceiling) {
        case CameraGainCeiling::k4x:
            return GAINCEILING_4X;
        case CameraGainCeiling::k8x:
            return GAINCEILING_8X;
        case CameraGainCeiling::k16x:
            return GAINCEILING_16X;
        case CameraGainCeiling::k32x:
            return GAINCEILING_32X;
        case CameraGainCeiling::k64x:
            return GAINCEILING_64X;
        case CameraGainCeiling::k128x:
            return GAINCEILING_128X;
        case CameraGainCeiling::k2x:
        default:
            return GAINCEILING_2X;
    }
}

CameraSensorControls DeskRobotCamera::ToSensorControls(const CameraSensorSettings& settings) {
    return {
        .brightness = settings.brightness,
        .contrast = settings.contrast,
        .saturation = settings.saturation,
        .auto_exposure = settings.auto_exposure,
        .aec2 = settings.aec2,
        .ae_level = settings.ae_level,
        .manual_exposure = settings.manual_exposure,
        .auto_gain = settings.auto_gain,
        .manual_gain = settings.manual_gain,
        .gain_ceiling = ToGainCeiling(settings.gain_ceiling),
        .auto_white_balance = settings.auto_white_balance,
        .awb_gain = settings.awb_gain,
        .white_balance_mode = settings.white_balance_mode,
        .black_pixel_correction = settings.black_pixel_correction,
        .white_pixel_correction = settings.white_pixel_correction,
        .gamma = settings.gamma,
        .lens_correction = settings.lens_correction,
        .mirror = settings.mirror,
        .flip = settings.flip,
    };
}
