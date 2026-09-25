#include "robot_web_adapter.h"

#include "config/hardware_config.h"
#include "control/robot_controller.h"
#ifdef SECONDARY_OLED_I2C_ADDRESS
#include "display/secondary_display_controller.h"
#endif

#include <algorithm>
#include <string>
#include <utility>

namespace {

bool ParseDirection(const std::string& direction, MotorController::Direction& command) {
    if (direction == "forward") {
        command = MotorController::Direction::kForward;
    } else if (direction == "backward") {
        command = MotorController::Direction::kBackward;
    } else if (direction == "left") {
        command = MotorController::Direction::kLeft;
    } else if (direction == "right") {
        command = MotorController::Direction::kRight;
    } else {
        return false;
    }
    return true;
}

}  // namespace

bool RobotWebAdapter::ExecuteLiveDrive(int left_percent, int right_percent,
                                       std::string& message) {
    const int safe_left = std::clamp(left_percent, -100, 100);
    const int safe_right = std::clamp(right_percent, -100, 100);
    const bool accepted = controller_.SetLiveDrive(safe_left, safe_right);
    message = accepted ? (safe_left == 0 && safe_right == 0 ? "Motors stopped" : "Live drive")
                       : "Movement blocked: table edge detected; reverse remains available";
    return accepted;
}

bool RobotWebAdapter::ExecuteAction(const std::string& action, int value,
                                    const std::string& text, std::string& message) {
    MotorController::Direction direction;
    if (ParseDirection(action, direction)) {
        const int safe_duration = std::clamp(value, 50, 5000);
        if (!controller_.Move(direction, safe_duration,
                              RobotController::MovePolicy::kReplaceCurrent)) {
            message = "Movement blocked: table edge detected; reverse remains available";
            return false;
        }
        message = "Moving " + action;
        return true;
    }
    if (action == "stop") {
        controller_.Stop();
        message = "Motors stopped";
        return true;
    }
#ifdef MPU6050_I2C_ADDRESS
    if (action == "turn_relative") {
        return controller_.TurnRelative(value, message);
    }
#endif
    if (action == "dance") {
        const bool started = controller_.Dance();
        message = started ? "Random dance started" : "Dance blocked: table edge detected";
        return started;
    }
    if (action == "wake") {
        controller_.ToggleWake();
        message = "Wake toggled";
        return true;
    }
    if (action == "camera_flip") {
        message = controller_.ToggleCameraFlip() ? "Camera flipped" : "Camera restored";
        return true;
    }
    if (action == "display_flip") {
        message = controller_.ToggleDisplayFlip() ? "Main display flipped"
                                                   : "Main display restored";
        return true;
    }
#ifdef SECONDARY_OLED_I2C_ADDRESS
    if (action == "oled_flip" || action == "oled_contrast" ||
        action == "oled_auto_contrast" || action == "oled_auto_contrast_minimum" ||
        action == "oled_auto_contrast_maximum" || action == "oled_brand" ||
        action == "oled_prefix") {
        SecondaryOled::Config config = controller_.GetSecondaryDisplayConfig();
        if (action == "oled_flip") {
            config.flip_180 = !config.flip_180;
        } else if (action == "oled_contrast") {
            config.contrast = static_cast<uint8_t>(std::clamp(value, 0, 255));
        } else if (action == "oled_auto_contrast") {
            config.auto_contrast_enabled = value != 0;
        } else if (action == "oled_auto_contrast_minimum") {
            config.auto_contrast_minimum =
                static_cast<uint8_t>(std::clamp(value, 0, 255));
            if (config.auto_contrast_maximum < config.auto_contrast_minimum) {
                config.auto_contrast_maximum = config.auto_contrast_minimum;
            }
        } else if (action == "oled_auto_contrast_maximum") {
            config.auto_contrast_maximum =
                static_cast<uint8_t>(std::clamp(value, 0, 255));
            if (config.auto_contrast_minimum > config.auto_contrast_maximum) {
                config.auto_contrast_minimum = config.auto_contrast_maximum;
            }
        } else if (action == "oled_brand") {
            config.brand = SecondaryDisplayController::NormalizeConfigText(text, "Desk Robot");
        } else {
            config.distance_prefix = SecondaryDisplayController::NormalizeConfigText(text, "Dist");
        }
        controller_.SetSecondaryDisplayConfig(config);
        message = "OLED settings updated";
        return true;
    }
    const int enabled_index =
        SecondaryDisplayController::WidgetActionIndex(action, "oled_widget_on_");
    const int size_index =
        SecondaryDisplayController::WidgetActionIndex(action, "oled_widget_size_");
    const int mode_index =
        SecondaryDisplayController::WidgetActionIndex(action, "oled_widget_mode_");
    const int up_index =
        SecondaryDisplayController::WidgetActionIndex(action, "oled_widget_up_");
    const int down_index =
        SecondaryDisplayController::WidgetActionIndex(action, "oled_widget_down_");
    if (enabled_index >= 0 || size_index >= 0 || mode_index >= 0 || up_index >= 0 ||
        down_index >= 0) {
        SecondaryOled::Config config = controller_.GetSecondaryDisplayConfig();
        if (enabled_index >= 0) {
            config.widgets[enabled_index].enabled = value != 0;
        } else if (size_index >= 0) {
            config.widgets[size_index].size =
                static_cast<SecondaryOled::WidgetSize>(std::clamp(value, 0, 2));
        } else if (mode_index >= 0) {
            config.widgets[mode_index].mode = static_cast<uint8_t>(std::clamp(value, 0, 2));
        } else if (up_index > 0) {
            std::swap(config.widgets[up_index], config.widgets[up_index - 1]);
        } else if (down_index >= 0 &&
                   down_index + 1 < static_cast<int>(config.widgets.size())) {
            std::swap(config.widgets[down_index], config.widgets[down_index + 1]);
        } else {
            message = "Widget is already at that edge";
            return false;
        }
        controller_.SetSecondaryDisplayConfig(config);
        message = "OLED widget layout updated";
        return true;
    }
#endif
    if (action == "lights_toggle") {
        message = controller_.ToggleStatusLight() ? "Status lights enabled"
                                                   : "Status lights disabled";
        return true;
    }
    if (action == "reaction_cancel") {
        const bool canceled = controller_.CancelReaction();
        message = canceled ? "Reaction canceled" : "No active reaction";
        return true;
    }
    const std::string reaction_prefix = "react_";
    if (action.rfind(reaction_prefix, 0) == 0) {
        const std::string reaction = action.substr(reaction_prefix.size());
        const int safe_duration = std::clamp(value, 250, 30000);
        const bool accepted = controller_.React(reaction, safe_duration, text);
        message = accepted ? "Reaction: " + reaction
                           : "Unsupported or blocked by a higher-priority reaction";
        return accepted;
    }
    if (action == "emotion") {
        if (controller_.GetStatus().state != "idle") {
            message = "Manual emotions are available only while Idle";
            return false;
        }
        const bool accepted = controller_.ShowEmotion(text, value > 0 ? value : 5000);
        message = accepted ? "Emotion: " + text : "Unsupported emotion";
        return accepted;
    }
    if (action == "audio_test") {
        if (controller_.GetStatus().state != "idle") {
            message = "Audio test is available only while Idle";
            return false;
        }
        controller_.PlayAudioTest();
        message = "Playing speaker test";
        return true;
    }
    if (action == "return_idle") {
        controller_.ReturnToIdleState();
        message = "Returning robot to idle";
        return true;
    }
    if (action == "reboot") {
        const bool queued = controller_.Reboot();
        message = queued ? "Robot is rebooting" : "Could not schedule reboot";
        return queued;
    }
    if (action == "speaker_volume") {
        const int safe_volume = std::clamp(value, 0, 100);
        controller_.SetSpeakerVolume(safe_volume);
        message = "Speaker volume " + std::to_string(safe_volume) + "%";
        return true;
    }
    if (action == "microphone_gain") {
        const int safe_gain = std::clamp(value, 1, 3);
        controller_.SetMicrophoneGain(safe_gain);
        message = "Microphone gain " + std::to_string(safe_gain) + "x";
        return true;
    }
    if (action == "microphone_mute") {
        const bool muted = value != 0;
        controller_.SetMicrophoneMuted(muted);
        message = muted ? "Microphone muted" : "Microphone enabled";
        return true;
    }
    if (action == "screen_brightness") {
        const int safe_brightness = std::clamp(value, 10, 100);
        controller_.SetScreenBrightness(safe_brightness);
        message = "Screen brightness " + std::to_string(safe_brightness) + "%";
        return true;
    }
    if (action == "auto_brightness") {
        const bool enabled = value != 0;
        controller_.SetAutoBrightnessEnabled(enabled);
        message = enabled ? "Automatic brightness enabled" : "Automatic brightness disabled";
        return true;
    }
    if (action == "auto_brightness_minimum") {
        const int safe_brightness = std::clamp(value, 10, 100);
        controller_.SetAutoBrightnessMinimum(safe_brightness);
        message = "Automatic brightness minimum " + std::to_string(safe_brightness) + "%";
        return true;
    }
    if (action == "auto_brightness_maximum") {
        const int safe_brightness = std::clamp(value, 10, 100);
        controller_.SetAutoBrightnessMaximum(safe_brightness);
        message = "Automatic brightness maximum " + std::to_string(safe_brightness) + "%";
        return true;
    }
    if (action == "motor_speed") {
        const int safe_speed = std::clamp(value, MotorController::kMinSpeedPercent,
                                          MotorController::kMaxSpeedPercent);
        controller_.SetMotorSpeed(safe_speed);
        message = "Motor speed " + std::to_string(safe_speed) + "%";
        return true;
    }
    if (action == "drive_duration") {
        const int safe_duration = std::clamp(value, 50, 5000);
        controller_.SetDriveDuration(safe_duration);
        message = "Drive time " + std::to_string(safe_duration) + " ms";
        return true;
    }
    if (action == "emotion_movement") {
        const bool enabled = value != 0;
        controller_.SetEmotionMovementEnabled(enabled);
        message = enabled ? "Emotion movement enabled" : "Emotion movement disabled";
        return true;
    }
    if (action == "status_light_brightness") {
        const int safe_brightness = std::clamp(value, 0, 100);
        controller_.SetStatusLightBrightness(safe_brightness);
        message = "Status light brightness " + std::to_string(safe_brightness) + "%";
        return true;
    }
#ifdef INA219_I2C_ADDRESS
    if (action == "battery_capacity_start") {
        if (!controller_.StartBatteryCapacityTest()) {
            message = "INA219 is unavailable";
            return false;
        }
        message = "Battery capacity measurement started";
        return true;
    }
    if (action == "battery_capacity_stop") {
        controller_.StopBatteryCapacityTest();
        message = "Battery capacity measurement stopped";
        return true;
    }
    if (action == "battery_capacity_reset") {
        controller_.ResetBatteryCapacityTest();
        message = "Battery capacity measurement reset";
        return true;
    }
#endif
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
    if (action == "cliff_threshold") {
        const int safe_edge_mm = std::clamp(value, 50, 500);
        controller_.SetCliffThreshold(safe_edge_mm);
        message = "Cliff threshold " + std::to_string(safe_edge_mm) + " mm";
        return true;
    }
#endif
#ifdef MPU6050_I2C_ADDRESS
    if (action == "motion_emotions") {
        const bool enabled = value != 0;
        controller_.SetMotionEmotionsEnabled(enabled);
        message = enabled ? "Motion emotions enabled" : "Motion emotions disabled";
        return true;
    }
#endif
    if (action == "live_camera") {
        const bool enabled = controller_.ToggleLiveCamera();
        message = enabled ? "Live preview enabled" : "Live preview disabled";
        return controller_.GetStatus().live_camera_available;
    }
    if (action == "wifi_config") {
        controller_.EnterWifiSetup();
        message = "Entering Wi-Fi setup";
        return true;
    }
    message = "Unknown action";
    return false;
}
