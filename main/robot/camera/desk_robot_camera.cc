#include "desk_robot_camera.h"

#include "camera_diagnostics.h"

#include <chrono>

#include <esp_log.h>
#include <esp_timer.h>

#define TAG "DeskRobotCamera"

DeskRobotCamera::DeskRobotCamera(const camera_config_t& config) : Esp32Camera(config) {}

bool DeskRobotCamera::Capture() {
    std::unique_lock<std::timed_mutex> lock(capture_mutex_, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::seconds(7))) {
        ESP_LOGE(TAG, "MCP camera capture timed out waiting for live preview");
        return false;
    }
    bool expected_inactive = false;
    if (!mcp_operation_active_.compare_exchange_strong(expected_inactive, true)) {
        ESP_LOGW(TAG, "MCP camera capture rejected: another operation is active");
        return false;
    }
    if (mcp_capture_pending_) {
        ESP_LOGW(TAG, "MCP camera capture rejected: previous snapshot is still pending");
        mcp_operation_active_ = false;
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
        mcp_operation_active_ = false;
    }
    return captured;
}

bool DeskRobotCamera::CapturePreview() {
    std::unique_lock<std::timed_mutex> lock(capture_mutex_, std::try_to_lock);
    if (!lock.owns_lock() || mcp_capture_pending_) {
        return false;
    }
    return Esp32Camera::Capture();
}

bool DeskRobotCamera::SendSnapshot(const JpegSender& sender) {
    std::unique_lock<std::timed_mutex> lock(capture_mutex_, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::seconds(7)) || mcp_capture_pending_) {
        return false;
    }
    if (!Esp32Camera::CaptureForWeb()) {
        return false;
    }
    const uint8_t* data = nullptr;
    size_t length = 0;
    return Esp32Camera::GetCurrentJpeg(data, length) && sender(data, length);
}

bool DeskRobotCamera::IsAvailable() const { return Esp32Camera::IsAvailable(); }

std::expected<std::string, std::string> DeskRobotCamera::Explain(const std::string& question) {
    if (!mcp_capture_pending_.exchange(false)) {
        CameraDiagnostics::CompleteOperation();
        mcp_diagnostic_operation_id_ = 0;
        mcp_operation_active_ = false;
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
    mcp_operation_active_ = false;
}
