#include "desk_robot_camera.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "board.h"
#include "display.h"
#include "jpg/jpeg_to_image.h"
#include "lvgl_display/lvgl_image.h"

#define TAG "DeskRobotCamera"

namespace {

constexpr int kMcpFreshWarmupFrames = 2;
constexpr int kMcpLowLightWarmupFrames = 4;
constexpr int kMcpHighResolutionWarmupFrames = 8;
constexpr int64_t kDiagnosticsRefreshIntervalMs = 2000;

bool IsHighResolution(CameraResolution resolution) {
    return resolution == CameraResolution::kXga ||
           resolution == CameraResolution::kSxga ||
           resolution == CameraResolution::kUxga ||
           resolution == CameraResolution::kQsxga;
}

bool SensorSettingsEqual(const CameraSensorSettings& lhs,
                         const CameraSensorSettings& rhs) {
    return lhs.profile == rhs.profile &&
           lhs.brightness == rhs.brightness &&
           lhs.contrast == rhs.contrast &&
           lhs.saturation == rhs.saturation &&
           lhs.auto_exposure == rhs.auto_exposure &&
           lhs.aec2 == rhs.aec2 &&
           lhs.ae_level == rhs.ae_level &&
           lhs.manual_exposure == rhs.manual_exposure &&
           lhs.auto_gain == rhs.auto_gain &&
           lhs.manual_gain == rhs.manual_gain &&
           lhs.gain_ceiling == rhs.gain_ceiling &&
           lhs.auto_white_balance == rhs.auto_white_balance &&
           lhs.awb_gain == rhs.awb_gain &&
           lhs.advanced_awb == rhs.advanced_awb &&
           lhs.white_balance_mode == rhs.white_balance_mode &&
           lhs.black_pixel_correction == rhs.black_pixel_correction &&
           lhs.white_pixel_correction == rhs.white_pixel_correction &&
           lhs.gamma == rhs.gamma &&
           lhs.lens_correction == rhs.lens_correction &&
           lhs.mirror == rhs.mirror &&
           lhs.flip == rhs.flip;
}

bool OperationalSettingsEqual(const CameraSettingsConfig& lhs,
                              const CameraSettingsConfig& rhs) {
    return SensorSettingsEqual(lhs.sensor, rhs.sensor) &&
           lhs.web.resolution == rhs.web.resolution &&
           lhs.web.jpeg_quality == rhs.web.jpeg_quality && lhs.web.fps == rhs.web.fps &&
           lhs.mochan.source_resolution == rhs.mochan.source_resolution &&
           lhs.mochan.aspect == rhs.mochan.aspect &&
           lhs.mochan.render == rhs.mochan.render &&
           lhs.mcp.resolution == rhs.mcp.resolution &&
           lhs.mcp.jpeg_quality == rhs.mcp.jpeg_quality &&
           lhs.mcp.freshness == rhs.mcp.freshness;
}

}  // namespace

DeskRobotCamera::DeskRobotCamera(const camera_config_t& config, std::mutex& shared_i2c_mutex,
                                 const CameraSettingsConfig& settings,
                                 CameraImagePolicy& image_policy,
                                 McpStateCallback mcp_state_callback,
                                 BackgroundWakeCallback background_wake_callback)
    : Esp32Camera(config, &shared_i2c_mutex),
      settings_(CameraSettingsStore::Normalize(settings)),
      image_policy_(image_policy),
      mcp_state_callback_(std::move(mcp_state_callback)),
      background_wake_callback_(std::move(background_wake_callback)) {
    observer_.Configure(settings_.vision);
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
    observer_mode_configured_.store(false, std::memory_order_release);
    WakeBackgroundWorker();
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
    // Change the sensor mode first, then apply the requested image controls to
    // the final capture mode.
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
        observer_mode_configured_.store(false, std::memory_order_release);
        if (!ApplyModeCaptureSettings(settings.web.resolution, settings.web.jpeg_quality) ||
            !ApplyModeSensorSettings(settings.sensor)) {
            return false;
        }
        // WHEN_EMPTY may still hold the frame captured before this mode switch.
        // Drain it before the first Web frame so an old QSXGA/profile frame is
        // never sent as the start of the new stream.
        if (!Esp32Camera::CaptureForWeb()) {
            return false;
        }
        ReturnCurrentFrame();
        hide_mochan = preview_mode_.load() == PreviewMode::kMochanPreview;
        preview_mode_.store(PreviewMode::kWebLive);
    }
    if (hide_mochan) {
        HidePreviewImage();
    }
    WakeBackgroundWorker();
    return true;
}

void DeskRobotCamera::StopWebLive() {
    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        if (preview_mode_.load() == PreviewMode::kWebLive) {
            preview_mode_.store(PreviewMode::kOff);
        }
    }
    WakeBackgroundWorker();
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
    observer_mode_configured_.store(false, std::memory_order_release);
    if (!ApplyModeCaptureSettings(settings.mochan.source_resolution, 12) ||
        !ApplyModeSensorSettings(settings.sensor)) {
        return false;
    }
    // Drop the queued pre-switch frame before decoding the first Mochan
    // preview. In WHEN_EMPTY mode it may use the previous resolution/profile.
    if (!Esp32Camera::CaptureForWeb()) {
        return false;
    }
    ReturnCurrentFrame();
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
    WakeBackgroundWorker();
}

void DeskRobotCamera::ForceOff() {
    observer_runtime_allowed_.store(false, std::memory_order_release);
    bool hide_preview = false;
    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        hide_preview = preview_mode_.load() == PreviewMode::kMochanPreview;
        preview_mode_.store(PreviewMode::kOff);
    }
    if (hide_preview) {
        HidePreviewImage();
    }
    {
        std::lock_guard<std::mutex> observer_gate(observer_sample_gate_);
        observer_.ResetBaseline();
        observer_baseline_reset_pending_ = false;
        const CameraSettingsConfig settings = GetSettings();
        observer_.SetState(settings.vision.enabled
                               ? CameraObserverState::kSuspendedNonIdle
                               : CameraObserverState::kDisabled,
                           settings.vision.enabled
                               ? CameraObserverSuspendReason::kNonIdle
                               : CameraObserverSuspendReason::kDisabled);
    }
    WakeBackgroundWorker();
}

void DeskRobotCamera::OnIdle() {
    if (observer_runtime_allowed_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    const CameraSettingsConfig settings = GetSettings();
    observer_.SetState(settings.vision.enabled ? CameraObserverState::kWaiting
                                               : CameraObserverState::kDisabled,
                       settings.vision.enabled
                           ? CameraObserverSuspendReason::kWaitingFirstSample
                           : CameraObserverSuspendReason::kDisabled);
    WakeBackgroundWorker();
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
    observer_mode_configured_.store(false, std::memory_order_release);
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
    const CameraSettingsConfig normalized = CameraSettingsStore::Normalize(requested);
    const CameraSettingsConfig initial = GetSettings();
    if (OperationalSettingsEqual(initial, normalized)) {
        {
            std::lock_guard<std::mutex> observer_gate(observer_sample_gate_);
            std::lock_guard<std::mutex> settings_lock(settings_mutex_);
            settings_ = normalized;
            observer_.Configure(normalized.vision);
            if (initial.vision.enabled != normalized.vision.enabled) {
                observer_baseline_reset_pending_ = false;
            }
        }
        WakeBackgroundWorker();
        return true;
    }
    {
        std::lock_guard<std::mutex> ownership_lock(ownership_mutex_);
        if (mcp_operation_active_.load()) {
            return false;
        }
        std::unique_lock<std::timed_mutex> capture_lock(capture_mutex_, std::defer_lock);
        if (!capture_lock.try_lock_for(std::chrono::seconds(7))) {
            return false;
        }

        const CameraSettingsConfig previous = GetSettings();
        const PreviewMode mode = preview_mode_.load();
        const bool sensor_changed = !SensorSettingsEqual(previous.sensor, normalized.sensor);
        const bool web_capture_changed =
            mode == PreviewMode::kWebLive &&
            (previous.web.resolution != normalized.web.resolution ||
             previous.web.jpeg_quality != normalized.web.jpeg_quality);
        const bool mochan_capture_changed =
            mode == PreviewMode::kMochanPreview &&
            previous.mochan.source_resolution != normalized.mochan.source_resolution;
        const bool capture_changed = web_capture_changed || mochan_capture_changed;
        observer_mode_configured_.store(false, std::memory_order_release);

        if (web_capture_changed &&
            !ApplyModeCaptureSettings(normalized.web.resolution, normalized.web.jpeg_quality)) {
            return false;
        }
        if (mochan_capture_changed &&
            !ApplyModeCaptureSettings(normalized.mochan.source_resolution, 12)) {
            return false;
        }
        // A frame-size change can rewrite sensor state, so replay controls after a
        // current-mode capture change even when the requested controls are equal.
        if ((sensor_changed || capture_changed) &&
            !ApplyModeSensorSettings(normalized.sensor)) {
            return false;
        }
        if (mode != PreviewMode::kOff && (sensor_changed || capture_changed)) {
            // CAMERA_GRAB_WHEN_EMPTY may have queued one frame using the previous
            // mode or controls. Discard it without changing preview ownership.
            if (!Esp32Camera::CaptureForWeb()) {
                return false;
            }
            ReturnCurrentFrame();
        }

        std::lock_guard<std::mutex> settings_lock(settings_mutex_);
        settings_ = normalized;
    }
    {
        std::lock_guard<std::mutex> observer_gate(observer_sample_gate_);
        observer_.Configure(normalized.vision);
        if (initial.vision.enabled != normalized.vision.enabled) {
            observer_baseline_reset_pending_ = false;
        }
    }
    WakeBackgroundWorker();
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
    return SensorPid() == OV5640_PID ? "OV5640" : "Unknown";
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

CameraDiagnostics DeskRobotCamera::GetDiagnostics() {
    const int64_t now_ms = esp_timer_get_time() / 1000;
    CameraDiagnostics cached;
    {
        std::lock_guard<std::mutex> cache_lock(diagnostics_cache_mutex_);
        cached = diagnostics_cache_;
    }
    if (cached.last_read_ms > 0 &&
        now_ms - cached.last_read_ms < kDiagnosticsRefreshIntervalMs) {
        return cached;
    }

    std::unique_lock<std::mutex> refresh_lock(diagnostics_refresh_mutex_,
                                               std::try_to_lock);
    if (!refresh_lock.owns_lock()) {
        return cached;
    }
    // Keep the normal ownership -> capture -> shared-I2C order. Every lock in
    // this diagnostic path is a try-lock so camera work always wins.
    std::unique_lock<std::mutex> ownership_lock(ownership_mutex_, std::try_to_lock);
    if (!ownership_lock.owns_lock() || mcp_operation_active_.load()) {
        return cached;
    }
    std::unique_lock<std::timed_mutex> capture_lock(capture_mutex_, std::try_to_lock);
    if (!capture_lock.owns_lock()) {
        return cached;
    }

    CameraDiagnostics refreshed;
    if (!Esp32Camera::ReadDiagnostics(refreshed)) {
        return cached;
    }
    {
        std::lock_guard<std::mutex> cache_lock(diagnostics_cache_mutex_);
        diagnostics_cache_ = refreshed;
    }
    return refreshed;
}

CameraObserverStatus DeskRobotCamera::GetObserverStatus() const {
    return observer_.GetStatus();
}

void DeskRobotCamera::ResetObserverContinuity() {
    std::lock_guard<std::mutex> observer_gate(observer_sample_gate_);
    observer_.ResetBaseline();
    observer_baseline_reset_pending_ = false;
    WakeBackgroundWorker();
}

int DeskRobotCamera::GetObserverRecommendedIntervalMs() const {
    return observer_.GetRecommendedIntervalMs();
}

CameraVisionEventFrameStatus DeskRobotCamera::GetVisionEventFrameStatus() const {
    CameraVisionEventFrameStatus status = vision_event_frame_.GetStatus();
    status.capture_pending =
        vision_event_frame_capture_pending_.load(std::memory_order_acquire);
    return status;
}

bool DeskRobotCamera::SetVisionEventFrameEnabled(bool enabled) {
    std::lock_guard<std::mutex> observer_gate(observer_sample_gate_);
    vision_event_frame_.SetEnabled(enabled);
    if (!enabled) {
        vision_event_frame_capture_pending_.store(false,
                                                  std::memory_order_release);
    }
    return true;
}

bool DeskRobotCamera::SendVisionEventFrame(
    const CameraVisionEventFrame::FrameSender& sender) const {
    return vision_event_frame_.Send(sender);
}

bool DeskRobotCamera::QueueVisionEventFrameCapture() {
    const CameraVisionEventFrameStatus status = vision_event_frame_.GetStatus();
    if (!status.enabled || status.psram_bytes == 0) {
        return false;
    }
    bool expected = false;
    if (!vision_event_frame_capture_pending_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return false;
    }
    const CameraVisionEventFrameStatus queued_status =
        vision_event_frame_.GetStatus();
    if (!queued_status.enabled || queued_status.psram_bytes == 0) {
        vision_event_frame_capture_pending_.store(false,
                                                  std::memory_order_release);
        return false;
    }
    WakeBackgroundWorker();
    return true;
}

void DeskRobotCamera::SetObserverWorkerState(CameraObserverState state,
                                             CameraObserverSuspendReason reason,
                                             bool count_skip) {
    observer_.SetState(state, reason, count_skip);
}

bool DeskRobotCamera::CaptureObserverSample(bool manual_event_frame) {
    std::unique_lock<std::mutex> observer_gate(observer_sample_gate_);
    if (manual_event_frame &&
        !vision_event_frame_capture_pending_.load(std::memory_order_acquire)) {
        return false;
    }
    const CameraSettingsConfig settings = GetSettings();
    if (!settings.vision.enabled && !manual_event_frame) {
        observer_.SetState(CameraObserverState::kDisabled,
                           CameraObserverSuspendReason::kDisabled);
        return false;
    }
    if (!observer_runtime_allowed_.load(std::memory_order_acquire)) {
        observer_.SetState(CameraObserverState::kSuspendedNonIdle,
                           CameraObserverSuspendReason::kNonIdle, true);
        return false;
    }
    if (!observer_.EnsureScratch()) {
        if (manual_event_frame) {
            vision_event_frame_capture_pending_.store(false,
                                                      std::memory_order_release);
        }
        observer_.RecordFailure(CameraObserverSuspendReason::kScratchUnavailable);
        return false;
    }

    std::unique_lock<std::mutex> ownership_lock(ownership_mutex_, std::try_to_lock);
    if (!ownership_lock.owns_lock()) {
        observer_.SetState(CameraObserverState::kBusy,
                           CameraObserverSuspendReason::kCameraBusy, true);
        return false;
    }
    if (mcp_operation_active_.load(std::memory_order_acquire)) {
        observer_.SetState(CameraObserverState::kSuspendedMcp,
                           CameraObserverSuspendReason::kMcp, true);
        return false;
    }
    const PreviewMode mode = preview_mode_.load(std::memory_order_acquire);
    if (mode != PreviewMode::kOff) {
        observer_.SetState(CameraObserverState::kSuspendedExplicitCamera,
                           mode == PreviewMode::kWebLive
                               ? CameraObserverSuspendReason::kWebLive
                               : CameraObserverSuspendReason::kMochanPreview,
                           true);
        return false;
    }
    std::unique_lock<std::timed_mutex> capture_lock(capture_mutex_, std::try_to_lock);
    if (!capture_lock.owns_lock()) {
        observer_.SetState(CameraObserverState::kBusy,
                           CameraObserverSuspendReason::kCameraBusy, true);
        return false;
    }

    const int64_t sample_start_us = esp_timer_get_time();
    const CameraSensorSettings resolved = ResolveSensorSettings(settings.sensor);
    const bool observer_sensor_changed =
        !observer_applied_sensor_settings_valid_ ||
        !SensorSettingsEqual(resolved, observer_applied_sensor_settings_);
    if (!observer_mode_configured_.load(std::memory_order_acquire) ||
        observer_sensor_changed) {
        if (manual_event_frame) {
            observer_baseline_reset_pending_ = true;
        } else {
            observer_.ResetBaseline();
            observer_baseline_reset_pending_ = false;
        }
        const bool configured = ApplyModeCaptureSettings(CameraResolution::kQvga, 20) &&
                                Esp32Camera::ApplySensorControls(
                                    ToSensorControls(resolved));
        if (!configured || !Esp32Camera::CaptureForWeb()) {
            ReturnCurrentFrame();
            if (manual_event_frame) {
                vision_event_frame_capture_pending_.store(
                    false, std::memory_order_release);
            }
            observer_.RecordFailure(CameraObserverSuspendReason::kCaptureFailed);
            return false;
        }
        ReturnCurrentFrame();
        observer_applied_sensor_settings_ = resolved;
        observer_applied_sensor_settings_valid_ = true;
        observer_mode_configured_.store(true, std::memory_order_release);
    }
    if (!manual_event_frame && observer_baseline_reset_pending_) {
        observer_.ResetBaseline();
        observer_baseline_reset_pending_ = false;
    }

    if (!Esp32Camera::CaptureForWeb()) {
        ReturnCurrentFrame();
        if (manual_event_frame) {
            vision_event_frame_capture_pending_.store(false,
                                                      std::memory_order_release);
        }
        observer_.RecordFailure(CameraObserverSuspendReason::kCaptureFailed);
        return false;
    }
    const uint8_t* jpeg_data = nullptr;
    size_t jpeg_length = 0;
    if (!Esp32Camera::GetCurrentJpeg(jpeg_data, jpeg_length)) {
        ReturnCurrentFrame();
        if (manual_event_frame) {
            vision_event_frame_capture_pending_.store(false,
                                                      std::memory_order_release);
        }
        observer_.RecordFailure(CameraObserverSuspendReason::kCaptureFailed);
        return false;
    }
    const uint32_t capture_ms = static_cast<uint32_t>(std::max<int64_t>(
        0, (esp_timer_get_time() - sample_start_us + 999) / 1000));
    size_t output_length = 0;
    size_t output_width = 0;
    size_t output_height = 0;
    size_t output_stride = 0;
    const int64_t decode_start_us = esp_timer_get_time();
    const esp_err_t decode_result = jpeg_to_image_scaled_into(
        jpeg_data, jpeg_length, observer_.scratch_data(), observer_.scratch_capacity(),
        &output_length, &output_width, &output_height, &output_stride,
        CameraObserver::kDecodeWidth, CameraObserver::kDecodeHeight);
    const uint32_t decode_ms = static_cast<uint32_t>(std::max<int64_t>(
        0, (esp_timer_get_time() - decode_start_us + 999) / 1000));
    ReturnCurrentFrame();
    capture_lock.unlock();
    ownership_lock.unlock();
    if (decode_result != ESP_OK || output_length > observer_.scratch_capacity()) {
        if (manual_event_frame) {
            vision_event_frame_capture_pending_.store(false,
                                                      std::memory_order_release);
        }
        observer_.RecordFailure(CameraObserverSuspendReason::kDecodeFailed);
        return false;
    }
    if (output_width != CameraObserver::kDecodeWidth ||
        output_height != CameraObserver::kDecodeHeight ||
        output_stride < CameraObserver::kDecodeStride || output_height == 0 ||
        output_stride > output_length / output_height) {
        if (manual_event_frame) {
            vision_event_frame_capture_pending_.store(false,
                                                      std::memory_order_release);
        }
        observer_.RecordFailure(CameraObserverSuspendReason::kInvalidFrame);
        return false;
    }
    const CameraObserverAnalysisResult analysis = observer_.ProcessRgb565(
        output_width, output_height, output_stride, capture_ms, decode_ms,
        sample_start_us, !manual_event_frame);
    if (manual_event_frame && !analysis.valid) {
        vision_event_frame_capture_pending_.store(false,
                                                  std::memory_order_release);
    } else if (analysis.valid && manual_event_frame) {
        if (vision_event_frame_.Capture(observer_.scratch_data(), output_width,
                                        output_height, output_stride,
                                        CameraVisionEventFrameSource::kManual,
                                        analysis.spatial)) {
            vision_event_frame_capture_pending_.store(
                false, std::memory_order_release);
        } else {
            const CameraVisionEventFrameStatus frame_status =
                vision_event_frame_.GetStatus();
            if (!frame_status.enabled || frame_status.psram_bytes == 0) {
                vision_event_frame_capture_pending_.store(
                    false, std::memory_order_release);
            }
        }
    } else if (analysis.valid &&
               analysis.sampling_state != CameraObserverSamplingState::kQuiet) {
        const CameraVisionEventFrameSource source =
            analysis.sampling_state == CameraObserverSamplingState::kMotion
                ? CameraVisionEventFrameSource::kMotion
                : CameraVisionEventFrameSource::kActivity;
        vision_event_frame_.Capture(observer_.scratch_data(), output_width,
                                    output_height, output_stride, source,
                                    analysis.spatial);
    }
    return analysis.valid;
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
    {
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        mcp_operation_active_.store(false);
    }
    WakeBackgroundWorker();
}

void DeskRobotCamera::WakeBackgroundWorker() {
    if (background_wake_callback_) {
        background_wake_callback_();
    }
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
        case CameraResolution::kQsxga:
            return FRAMESIZE_QSXGA;
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
        .advanced_awb = settings.advanced_awb,
        .white_balance_mode = settings.white_balance_mode,
        .black_pixel_correction = settings.black_pixel_correction,
        .white_pixel_correction = settings.white_pixel_correction,
        .gamma = settings.gamma,
        .lens_correction = settings.lens_correction,
        .mirror = settings.mirror,
        .flip = settings.flip,
    };
}
