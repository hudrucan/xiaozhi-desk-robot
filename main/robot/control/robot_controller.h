#pragma once

#include "camera/camera_settings.h"
#include "robot_status.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

class RobotController {
public:
    using SnapshotSender = std::function<bool(const uint8_t*, size_t)>;
    enum class MovePolicy { kReplaceCurrent, kPreserveQueued };

    virtual ~RobotController();

    virtual bool Move(MotorController::Direction direction, int duration_ms, MovePolicy policy) = 0;
    virtual bool SetLiveDrive(int left_percent, int right_percent) = 0;
    virtual void Stop() = 0;
    virtual bool Dance() = 0;
    virtual bool TurnRelative(int degrees, std::string& message) = 0;

    virtual bool ShowEmotion(const std::string& emotion, int duration_ms) = 0;
    virtual bool ShowSecondaryText(const std::string& text, int duration_ms) = 0;
    virtual bool SetStatusLightEffect(const std::string& effect, int duration_ms) = 0;
    virtual bool ToggleCameraFlip() = 0;
    virtual bool ToggleDisplayFlip() = 0;
    virtual bool ToggleStatusLight() = 0;
    virtual bool ToggleLiveCamera() = 0;
    virtual bool StartWebCameraStream() = 0;
    virtual bool IsWebCameraStreamEnabled() const = 0;
    virtual bool SendWebCameraFrame(const SnapshotSender& sender) = 0;
    virtual int GetWebCameraFrameIntervalMs() const = 0;
    virtual void StopWebCameraStream() = 0;
    virtual bool SendSnapshot(const SnapshotSender& sender) = 0;
    virtual CameraSettingsConfig GetCameraSettings() const = 0;
    virtual bool ApplyCameraSettings(const CameraSettingsConfig& settings) = 0;
    virtual bool ResetCameraSettings() = 0;
    virtual std::string GetCameraSensorName() const = 0;

    virtual SecondaryOled::Config GetSecondaryDisplayConfig() const = 0;
    virtual void SetSecondaryDisplayConfig(const SecondaryOled::Config& config) = 0;

    virtual void SetSpeakerVolume(int volume) = 0;
    virtual void SetMicrophoneGain(int gain) = 0;
    virtual void SetMicrophoneMuted(bool muted) = 0;
    virtual void SetScreenBrightness(int brightness) = 0;
    virtual void SetAutoBrightnessEnabled(bool enabled) = 0;
    virtual void SetAutoBrightnessMinimum(int brightness) = 0;
    virtual void SetAutoBrightnessMaximum(int brightness) = 0;
    virtual void SetMotorSpeed(int speed) = 0;
    virtual void SetDriveDuration(int duration_ms) = 0;
    virtual void SetEmotionMovementEnabled(bool enabled) = 0;
    virtual void SetStatusLightBrightness(int brightness) = 0;
    virtual void SetCliffThreshold(int edge_mm) = 0;
    virtual void SetMotionEmotionsEnabled(bool enabled) = 0;

    virtual bool StartBatteryCapacityTest() = 0;
    virtual void StopBatteryCapacityTest() = 0;
    virtual void ResetBatteryCapacityTest() = 0;

    virtual void ToggleWake() = 0;
    virtual bool PlayAudioTest() = 0;
    virtual void ReturnToIdleState() = 0;
    virtual bool Reboot() = 0;
    virtual void EnterWifiSetup() = 0;

    virtual RobotStatus GetStatus() = 0;
};
