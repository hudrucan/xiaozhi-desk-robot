#include "desk_robot_camera.h"

#include <chrono>

#include <esp_log.h>

#define TAG "DeskRobotCamera"

DeskRobotCamera::DeskRobotCamera(const camera_config_t& config) : Esp32Camera(config) {}

bool DeskRobotCamera::Capture() {
    std::unique_lock<std::timed_mutex> lock(capture_mutex_, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::seconds(7))) {
        ESP_LOGE(TAG, "MCP camera capture timed out waiting for live preview");
        return false;
    }
    if (mcp_frame_reserved_) {
        ESP_LOGW(TAG, "MCP camera capture rejected: previous frame is still reserved");
        return false;
    }
    ESP_LOGI(TAG, "MCP camera capture begin");
    const bool captured = Esp32Camera::Capture();
    ESP_LOGI(TAG, "MCP camera capture %s", captured ? "done" : "failed");
    mcp_frame_reserved_ = captured;
    return captured;
}

bool DeskRobotCamera::CapturePreview() {
    std::unique_lock<std::timed_mutex> lock(capture_mutex_, std::try_to_lock);
    if (!lock.owns_lock() || mcp_frame_reserved_) {
        return false;
    }
    return Esp32Camera::Capture();
}

bool DeskRobotCamera::SendSnapshot(const JpegSender& sender) {
    std::unique_lock<std::timed_mutex> lock(capture_mutex_, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::seconds(7)) || mcp_frame_reserved_) {
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
    std::unique_lock<std::timed_mutex> lock(capture_mutex_, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::seconds(7))) {
        mcp_frame_reserved_ = false;
        return std::unexpected("Timed out waiting for camera frame");
    }
    ESP_LOGI(TAG, "MCP camera explain begin");
    auto result = Esp32Camera::Explain(question);
    mcp_frame_reserved_ = false;
    if (result) {
        ESP_LOGI(TAG, "MCP camera explain done");
    } else {
        ESP_LOGE(TAG, "MCP camera explain failed");
    }
    return result;
}
