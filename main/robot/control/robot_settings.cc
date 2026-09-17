#include "robot_settings.h"

#include "config/tuning.h"
#include "motion/motor_controller.h"
#include "settings.h"

#include <algorithm>

namespace {
constexpr int kDefaultMotorSpeedPercent = 70;
constexpr int kDefaultDriveDurationMs = 250;
}  // namespace

int RobotSettings::GetSpeakerVolume() const {
    Settings settings("audio", false);
    return std::clamp(static_cast<int>(settings.GetInt("output_volume", 70)), 0, 100);
}

int RobotSettings::GetMicrophoneGain() const {
    Settings settings("audio", false);
    return std::clamp(static_cast<int>(settings.GetInt("input_gain", 1)), 1, 3);
}

void RobotSettings::SetMicrophoneGain(int gain) const {
    Settings settings("audio", true);
    settings.SetInt("input_gain", std::clamp(gain, 1, 3));
}

bool RobotSettings::GetCameraFlipped() const {
    Settings settings("desk_robot", false);
    return settings.GetBool("camera_flip", false);
}

void RobotSettings::SetCameraFlipped(bool flipped) const {
    Settings settings("desk_robot", true);
    settings.SetBool("camera_flip", flipped);
}

bool RobotSettings::GetDisplayFlipped() const {
    Settings settings("desk_robot", false);
    return settings.GetBool("display_flip", false);
}

void RobotSettings::SetDisplayFlipped(bool flipped) const {
    Settings settings("desk_robot", true);
    settings.SetBool("display_flip", flipped);
}

int RobotSettings::GetCliffEdgeMm() const {
    Settings settings("desk_robot", false);
    return std::clamp(static_cast<int>(settings.GetInt("cliff_edge_mm", CLIFF_EDGE_DISTANCE_MM)),
                      50, 500);
}

void RobotSettings::SetCliffEdgeMm(int edge_mm) const {
    Settings settings("desk_robot", true);
    settings.SetInt("cliff_edge_mm", std::clamp(edge_mm, 50, 500));
}

bool RobotSettings::GetMotionEmotionsEnabled() const {
    Settings settings("desk_robot", false);
    return settings.GetBool("motion_emotions", true);
}

void RobotSettings::SetMotionEmotionsEnabled(bool enabled) const {
    Settings settings("desk_robot", true);
    settings.SetBool("motion_emotions", enabled);
}

int RobotSettings::GetStatusLightBrightness() const {
    Settings settings("desk_robot", false);
    return std::clamp(
        static_cast<int>(settings.GetInt("led_brightness", STATUS_LIGHT_DEFAULT_BRIGHTNESS)), 0,
        100);
}

void RobotSettings::SetStatusLightBrightness(int brightness) const {
    Settings settings("desk_robot", true);
    settings.SetInt("led_brightness", std::clamp(brightness, 0, 100));
}

int RobotSettings::GetMotorSpeed() const {
    Settings settings("desk_robot", false);
    return std::clamp(static_cast<int>(settings.GetInt("motor_speed", kDefaultMotorSpeedPercent)),
                      MotorController::kMinSpeedPercent, MotorController::kMaxSpeedPercent);
}

void RobotSettings::SetMotorSpeed(int speed) const {
    Settings settings("desk_robot", true);
    settings.SetInt("motor_speed", std::clamp(speed, MotorController::kMinSpeedPercent,
                                               MotorController::kMaxSpeedPercent));
}

int RobotSettings::GetDriveDurationMs() const {
    Settings settings("desk_robot", false);
    return std::clamp(static_cast<int>(settings.GetInt("drive_time", kDefaultDriveDurationMs)), 50,
                      2000);
}

void RobotSettings::SetDriveDurationMs(int duration_ms) const {
    Settings settings("desk_robot", true);
    settings.SetInt("drive_time", std::clamp(duration_ms, 50, 2000));
}

bool RobotSettings::GetEmotionMovementEnabled() const {
    Settings settings("desk_robot", false);
    return settings.GetBool("emotion_move", false);
}

void RobotSettings::SetEmotionMovementEnabled(bool enabled) const {
    Settings settings("desk_robot", true);
    settings.SetBool("emotion_move", enabled);
}

int RobotSettings::GetManualScreenBrightness() const {
    Settings settings("display", false);
    return std::clamp(static_cast<int>(settings.GetInt("brightness", 75)), 10, 100);
}

bool RobotSettings::GetAutoBrightnessEnabled() const {
    Settings settings("desk_robot", false);
    return settings.GetBool("auto_bright", false);
}

void RobotSettings::SetAutoBrightnessEnabled(bool enabled) const {
    Settings settings("desk_robot", true);
    settings.SetBool("auto_bright", enabled);
}

int RobotSettings::GetAutoBrightnessMinimum() const {
    Settings settings("desk_robot", false);
    return std::clamp(static_cast<int>(settings.GetInt("auto_bmin",
                                                       AUTO_BRIGHTNESS_DEFAULT_MIN_PERCENT)),
                      10, 100);
}

void RobotSettings::SetAutoBrightnessMinimum(int brightness) const {
    Settings settings("desk_robot", true);
    settings.SetInt("auto_bmin", std::clamp(brightness, 10, 100));
}

int RobotSettings::GetAutoBrightnessMaximum() const {
    Settings settings("desk_robot", false);
    return std::clamp(static_cast<int>(settings.GetInt("auto_bmax",
                                                       AUTO_BRIGHTNESS_DEFAULT_MAX_PERCENT)),
                      10, 100);
}

void RobotSettings::SetAutoBrightnessMaximum(int brightness) const {
    Settings settings("desk_robot", true);
    settings.SetInt("auto_bmax", std::clamp(brightness, 10, 100));
}
