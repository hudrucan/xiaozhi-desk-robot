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

private:
    std::timed_mutex capture_mutex_;
    std::atomic_bool mcp_frame_reserved_{false};
};
