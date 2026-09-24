#pragma once

#include "camera_image_policy.h"
#include "camera_settings.h"
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
    enum class PreviewMode : uint8_t {
        kOff,
        kWebLive,
        kMochanPreview,
    };

    enum class McpRequestState : uint8_t {
        kNever,
        kCapturing,
        kAnalyzing,
        kSucceeded,
        kFailed,
    };

    struct McpRequestHealth {
        McpRequestState state = McpRequestState::kNever;
        int64_t started_us = 0;
        uint32_t capture_ms = 0;
        uint32_t vision_ms = 0;
        size_t image_bytes = 0;
        size_t response_bytes = 0;
    };

    DeskRobotCamera(const camera_config_t& config, std::mutex& shared_i2c_mutex,
                    const CameraSettingsConfig& settings, CameraImagePolicy& image_policy);

    bool Capture() override;
    bool StartWebLive();
    void StopWebLive();
    bool StartMochanPreview();
    void StopMochanPreview();
    void ForceOff() override;
    PreviewMode preview_mode() const { return preview_mode_.load(); }
    bool IsWebLiveActive() const {
        return preview_mode_.load() == PreviewMode::kWebLive;
    }
    bool IsMochanPreviewActive() const {
        return preview_mode_.load() == PreviewMode::kMochanPreview;
    }
    bool IsMcpOperationActive() const { return mcp_operation_active_.load(); }
    bool CapturePreview();
    bool SendWebLiveFrame(const JpegSender& sender);
    bool SendSnapshot(const JpegSender& sender);
    bool IsAvailable() const;
    bool ApplySettings(const CameraSettingsConfig& settings);
    CameraSettingsConfig GetSettings() const;
    int WebFrameIntervalMs() const;
    const char* SensorName() const;
    CameraImagePolicy::Status GetImagePolicyStatus() const;
    McpRequestHealth GetMcpRequestHealth() const;
    static const char* McpRequestStateName(McpRequestState state);
    std::expected<std::string, std::string> Explain(const std::string& question) override;
    void OnMcpResponseSent() override;

private:
    bool BeginMcpOperation(bool& preview_preempted);
    void EndMcpOperation();
    void HidePreviewImage();
    bool ApplyModeCaptureSettings(CameraResolution resolution, int jpeg_quality);
    bool ApplyModeSensorSettings(const CameraSensorSettings& settings);
    CameraSensorSettings ResolveSensorSettings(const CameraSensorSettings& settings) const;
    static framesize_t ToFrameSize(CameraResolution resolution);
    static gainceiling_t ToGainCeiling(CameraGainCeiling ceiling);
    static CameraSensorControls ToSensorControls(const CameraSensorSettings& settings);

    std::mutex ownership_mutex_;
    std::timed_mutex capture_mutex_;
    std::atomic<PreviewMode> preview_mode_{PreviewMode::kOff};
    std::atomic_bool mcp_operation_active_{false};
    std::atomic_bool mcp_capture_pending_{false};
    std::atomic<McpRequestState> mcp_request_state_{McpRequestState::kNever};
    std::atomic<int64_t> mcp_request_started_us_{0};
    std::atomic<uint32_t> mcp_capture_ms_{0};
    std::atomic<uint32_t> mcp_vision_ms_{0};
    std::atomic_size_t mcp_image_bytes_{0};
    std::atomic_size_t mcp_response_bytes_{0};
    mutable std::mutex settings_mutex_;
    CameraSettingsConfig settings_;
    CameraImagePolicy& image_policy_;
};
