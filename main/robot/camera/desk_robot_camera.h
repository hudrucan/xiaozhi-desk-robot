#pragma once

#include "camera_settings.h"
#include "esp32_camera.h"

#include <atomic>
#include <cstddef>
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

    DeskRobotCamera(const camera_config_t& config, std::mutex& shared_i2c_mutex,
                    const CameraSettingsConfig& settings);

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
    std::expected<std::string, std::string> Explain(const std::string& question) override;
    void OnMcpResponseSent() override;

private:
    bool BeginMcpOperation(bool& preview_preempted);
    void EndMcpOperation();
    void HidePreviewImage();
    bool ApplyModeCaptureSettings(CameraResolution resolution, int jpeg_quality);
    static framesize_t ToFrameSize(CameraResolution resolution);
    static gainceiling_t ToGainCeiling(CameraGainCeiling ceiling);
    static CameraSensorControls ToSensorControls(const CameraSensorSettings& settings);

    std::mutex ownership_mutex_;
    std::timed_mutex capture_mutex_;
    std::atomic<PreviewMode> preview_mode_{PreviewMode::kOff};
    std::atomic_bool mcp_operation_active_{false};
    std::atomic_bool mcp_capture_pending_{false};
    mutable std::mutex settings_mutex_;
    CameraSettingsConfig settings_;
};
