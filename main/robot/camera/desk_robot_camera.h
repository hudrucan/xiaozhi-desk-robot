#pragma once

#include "esp32_camera.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <string>

class DeskRobotCamera : public Esp32Camera {
public:
    using JpegSender = std::function<bool(const uint8_t*, size_t)>;

    explicit DeskRobotCamera(const camera_config_t& config);

    bool Capture() override;
    bool CapturePreview();
    bool SendSnapshot(const JpegSender& sender);
    bool IsAvailable() const;
    std::expected<std::string, std::string> Explain(const std::string& question) override;
    void OnMcpResultSerialized() override;
    void OnMcpResponseSent() override;

private:
    std::timed_mutex capture_mutex_;
    std::atomic_bool mcp_operation_active_{false};
    std::atomic_bool mcp_capture_pending_{false};
    std::atomic<uint32_t> mcp_diagnostic_operation_id_{0};
};
