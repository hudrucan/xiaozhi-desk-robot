#include "desk_robot_camera.h"

#include "camera_diagnostics.h"

#include <chrono>

#include <esp_log.h>
#include <esp_timer.h>

#include "board.h"
#include "display.h"

#define TAG "DeskRobotCamera"

DeskRobotCamera::DeskRobotCamera(const camera_config_t& config) : Esp32Camera(config) {}

bool DeskRobotCamera::Capture() {
    bool preview_preempted = false;
    if (!BeginMcpOperation(preview_preempted)) {
        ESP_LOGW(TAG, "MCP camera capture rejected: another operation is active");
        return false;
    }
    if (preview_preempted) {
        HidePreviewImage();
    }

    std::unique_lock<std::timed_mutex> lock(capture_mutex_, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::seconds(7))) {
        ESP_LOGE(TAG, "MCP camera capture timed out waiting for live preview");
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
        EndMcpOperation();
        return false;
    }
    mcp_diagnostic_operation_id_ =
        CameraDiagnostics::BeginOperation(CameraDiagnosticStage::kCapture);
    const int64_t capture_start_us = esp_timer_get_time();
    ESP_LOGI(TAG, "MCP camera operation=%lu capture begin",
             static_cast<unsigned long>(mcp_diagnostic_operation_id_.load()));
    const bool captured = Esp32Camera::CaptureOwnedJpeg();
    const int64_t capture_ms = (esp_timer_get_time() - capture_start_us) / 1000;
    ESP_LOGI(TAG, "MCP camera operation=%lu capture %s elapsed=%lldms",
             static_cast<unsigned long>(mcp_diagnostic_operation_id_.load()),
             captured ? "done" : "failed", static_cast<long long>(capture_ms));
    mcp_capture_pending_ = captured;
    if (!captured) {
        CameraDiagnostics::CompleteOperation();
        mcp_diagnostic_operation_id_ = 0;
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
    const bool captured = Esp32Camera::Capture();
    ReturnCurrentFrame();
    if (!IsMochanPreviewActive()) {
        HidePreviewImage();
        return false;
    }
    return captured;
}

bool DeskRobotCamera::SendSnapshot(const JpegSender& sender) {
    // A browser capture takes ownership from Mochan. The browser live loop is
    // still snapshot-based until the MJPEG phase, so its first frame performs
    // the same one-way preemption and preview is not auto-resumed.
    if (IsMochanPreviewActive()) {
        StopMochanPreview();
    }
    std::unique_lock<std::timed_mutex> lock(capture_mutex_, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::seconds(7)) || mcp_operation_active_.load()) {
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

std::expected<std::string, std::string> DeskRobotCamera::Explain(const std::string& question) {
    if (!mcp_capture_pending_.exchange(false)) {
        CameraDiagnostics::CompleteOperation();
        mcp_diagnostic_operation_id_ = 0;
        EndMcpOperation();
        return std::unexpected("No MCP camera snapshot is pending");
    }
    ESP_LOGI(TAG, "MCP camera operation=%lu explain begin",
             static_cast<unsigned long>(mcp_diagnostic_operation_id_.load()));
    auto result = Esp32Camera::Explain(question);
    CameraDiagnostics::SetStage(CameraDiagnosticStage::kResultReady);
    if (result) {
        ESP_LOGI(TAG, "MCP camera operation=%lu explain done",
                 static_cast<unsigned long>(mcp_diagnostic_operation_id_.load()));
    } else {
        ESP_LOGE(TAG, "MCP camera operation=%lu explain failed",
                 static_cast<unsigned long>(mcp_diagnostic_operation_id_.load()));
    }
    return result;
}

void DeskRobotCamera::OnMcpResultSerialized() {
    if (!mcp_operation_active_.load()) {
        return;
    }
    CameraDiagnostics::SetStage(CameraDiagnosticStage::kResultSerialized);
}

void DeskRobotCamera::OnMcpResponseSent() {
    if (!mcp_operation_active_.load()) {
        return;
    }
    CameraDiagnostics::SetStage(CameraDiagnosticStage::kResponseSent);
    ESP_LOGI(TAG, "MCP camera operation=%lu response sent",
             static_cast<unsigned long>(mcp_diagnostic_operation_id_.load()));
    CameraDiagnostics::CompleteOperation();
    mcp_diagnostic_operation_id_ = 0;
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
