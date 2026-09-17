#include "robot_web_adapter.h"

#include "config/hardware_config.h"
#include "config/tuning.h"
#include "control/robot_controller.h"
#ifdef SECONDARY_OLED_I2C_ADDRESS
#include "display/secondary_display_controller.h"
#endif

#include <esp_timer.h>
#include <cJSON.h>

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

bool RobotWebAdapter::ExecuteAction(const std::string& action, int value,
                                          const std::string& text, std::string& message) {
    MotorController::Direction direction;
    if (ParseDirection(action, direction)) {
        const int safe_duration = std::clamp(value, 50, 2000);
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
    if (action == "oled_flip" || action == "oled_contrast" || action == "oled_brand" ||
        action == "oled_prefix") {
        SecondaryOled::Config config = controller_.GetSecondaryDisplayConfig();
        if (action == "oled_flip") {
            config.flip_180 = !config.flip_180;
        } else if (action == "oled_contrast") {
            config.contrast = static_cast<uint8_t>(std::clamp(value, 0, 255));
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
    if (action == "screen_brightness") {
        const int safe_brightness = std::clamp(value, 10, 100);
        controller_.SetScreenBrightness(safe_brightness);
        message = "Screen brightness " + std::to_string(safe_brightness) + "%";
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
        const int safe_duration = std::clamp(value, 50, 2000);
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


cJSON* RobotWebAdapter::CreateStatus() {
    const RobotStatus status = controller_.GetStatus();
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return nullptr;
    }
    cJSON_AddStringToObject(root, "state", status.state.c_str());
    cJSON_AddBoolToObject(root, "asr_ready", status.asr_ready);
    cJSON_AddBoolToObject(root, "asr_preparing", status.asr_preparing);
    cJSON_AddBoolToObject(root, "camera_available", status.camera_available);
    cJSON_AddBoolToObject(root, "camera_flipped", status.camera_flipped);
    cJSON_AddBoolToObject(root, "display_flipped", status.display_flipped);
    cJSON_AddStringToObject(root, "emotion", status.emotion.c_str());
    cJSON_AddNumberToObject(root, "speaker_volume", status.speaker_volume);
    cJSON_AddNumberToObject(root, "microphone_gain", status.microphone_gain);
    cJSON_AddNumberToObject(root, "microphone_level", status.microphone_level);
    cJSON_AddBoolToObject(root, "microphone_clipping", status.microphone_clipping);
    cJSON_AddNumberToObject(root, "screen_brightness", status.screen_brightness);
    cJSON_AddNumberToObject(root, "status_light_brightness", status.status_light_brightness);
    cJSON_AddBoolToObject(root, "live_camera", status.live_camera);
    cJSON_AddNumberToObject(root, "motor_speed", status.motor_speed);
    cJSON_AddNumberToObject(root, "drive_duration_ms", status.drive_duration_ms);
    cJSON_AddBoolToObject(root, "emotion_movement_enabled",
                          status.emotion_movement_enabled);
    cJSON_AddBoolToObject(root, "emotion_movement_active", status.emotion_movement_active);
    cJSON* motor_status = cJSON_CreateObject();
    if (motor_status != nullptr) {
        cJSON_AddBoolToObject(motor_status, "available", status.motors.available);
        cJSON_AddBoolToObject(motor_status, "faulted", status.motors.faulted);
        cJSON_AddBoolToObject(motor_status, "moving", status.motors.moving);
        cJSON_AddStringToObject(motor_status, "direction",
                                MotorController::DirectionName(status.motors.direction));
        cJSON_AddNumberToObject(motor_status, "intensity_percent",
                                status.motors.intensity_percent);
        cJSON_AddNumberToObject(motor_status, "queued", status.motors.queued);
        cJSON_AddNumberToObject(motor_status, "remaining_ms", status.motors.remaining_ms);
        cJSON_AddBoolToObject(motor_status, "sequence_active", status.motors.sequence_active);
        cJSON_AddNumberToObject(motor_status, "sequence_total", status.motors.sequence_total);
        cJSON_AddNumberToObject(motor_status, "sequence_completed",
                                status.motors.sequence_completed);
        cJSON_AddItemToObject(root, "motors", motor_status);
    }
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
    const auto& cliff = status.cliff;
    cJSON_AddNumberToObject(root, "distance_mm", cliff.distance_mm);
    cJSON_AddBoolToObject(root, "distance_valid", cliff.valid);
    cJSON_AddBoolToObject(root, "cliff_detected", cliff.cliff_detected);
    cJSON_AddNumberToObject(root, "cliff_edge_mm", cliff.edge_mm);
#endif
#ifdef INA219_I2C_ADDRESS
    const auto& battery = status.battery;
    cJSON_AddBoolToObject(root, "battery_available", battery.available);
    cJSON_AddBoolToObject(root, "battery_valid", battery.valid);
    cJSON_AddNumberToObject(root, "battery_percent", battery.percent);
    cJSON_AddNumberToObject(root, "battery_voltage_v", battery.voltage_v);
    cJSON_AddNumberToObject(root, "battery_current_ma", battery.current_ma);
    cJSON_AddNumberToObject(root, "battery_power_mw", battery.power_mw);
    cJSON_AddNumberToObject(root, "battery_signed_current_ma", battery.signed_current_ma);
    cJSON_AddNumberToObject(root, "battery_shunt_voltage_mv", battery.shunt_voltage_mv);
    cJSON_AddNumberToObject(root, "battery_bus_voltage_v", battery.bus_voltage_v);
    cJSON_AddNumberToObject(root, "battery_remaining_mah", battery.remaining_mah);
    cJSON_AddNumberToObject(root, "battery_capacity_mah", BATTERY_SOC_USABLE_CAPACITY_MAH);
    cJSON_AddStringToObject(root, "battery_soc_method", "coulomb_quasi_rest_anchors");
    cJSON_AddBoolToObject(root, "battery_soc_tracking_degraded",
                          battery.soc_tracking_degraded);
    cJSON_AddBoolToObject(root, "battery_soc_quasi_resting", battery.soc_quasi_resting);
    cJSON_AddNumberToObject(root, "battery_soc_voltage_reference_percent",
                            battery.soc_voltage_reference_percent);
    cJSON_AddNumberToObject(root, "battery_soc_voltage_correction_mah",
                            battery.soc_voltage_correction_mah);
    cJSON_AddBoolToObject(root, "battery_soc_full_anchored", battery.soc_full_anchored);
    cJSON_AddBoolToObject(root, "battery_soc_bootstrap_voltage_rebased",
                          battery.soc_bootstrap_voltage_rebased);
    cJSON_AddBoolToObject(root, "battery_soc_empty_anchored", battery.soc_empty_anchored);
    cJSON_AddBoolToObject(root, "battery_conversion_ready", battery.conversion_ready);
    cJSON_AddBoolToObject(root, "battery_math_overflow", battery.math_overflow);
    const char* flow_state = !battery.valid                         ? "unknown"
                             : battery.signed_current_ma < -20.0f   ? "charging"
                             : battery.signed_current_ma > 20.0f    ? "discharging"
                                                                    : "near_zero";
    cJSON_AddStringToObject(root, "battery_flow_state", flow_state);
    cJSON_AddStringToObject(root, "external_power", "unknown");
    cJSON_AddBoolToObject(root, "battery_charging", battery.charging);
    cJSON_AddBoolToObject(root, "battery_discharging", battery.discharging);
    cJSON_AddBoolToObject(root, "battery_capacity_test_active", battery.capacity_test_active);
    cJSON_AddBoolToObject(root, "battery_capacity_test_measuring",
                          battery.capacity_test_measuring);
    cJSON_AddNumberToObject(root, "battery_capacity_test_mah",
                            battery.capacity_test_uah / 1000.0);
    cJSON_AddNumberToObject(root, "battery_capacity_test_seconds",
                            battery.capacity_test_seconds);
#endif
#ifdef MPU6050_I2C_ADDRESS
    const auto& gyro = status.gyro;
    cJSON_AddBoolToObject(root, "motion_sensor_available", status.motion_sensor_available);
    cJSON_AddBoolToObject(root, "motion_sensor_valid", status.motion_sensor_valid);
    cJSON_AddBoolToObject(root, "motion_emotions_enabled", status.motion_emotions_enabled);
    cJSON_AddNumberToObject(root, "motion_roll_deg", status.motion_roll_deg);
    cJSON_AddNumberToObject(root, "motion_pitch_deg", status.motion_pitch_deg);
    cJSON_AddNumberToObject(root, "motion_acceleration_g", status.motion_acceleration_g);
    cJSON_AddNumberToObject(root, "motion_rotation_dps", status.motion_rotation_dps);
    cJSON_AddNumberToObject(root, "motion_yaw_rate_dps", gyro.yaw_rate_dps);
    cJSON_AddNumberToObject(root, "motion_yaw_bias_dps", gyro.yaw_bias_dps);
    cJSON_AddBoolToObject(root, "gyro_bias_valid", gyro.bias_valid);
    const int64_t gyro_sample_us = gyro.sample_timestamp_us;
    cJSON_AddNumberToObject(
        root, "gyro_sample_age_ms",
        gyro_sample_us > 0 ? (esp_timer_get_time() - gyro_sample_us) / 1000.0 : -1.0);
    cJSON_AddBoolToObject(root, "gyro_turn_available", gyro.available);
    cJSON_AddBoolToObject(root, "gyro_turn_pending", gyro.pending);
    cJSON_AddBoolToObject(root, "gyro_turn_active", gyro.active);
    cJSON_AddNumberToObject(root, "gyro_turn_target_deg", gyro.target_deg);
    cJSON_AddNumberToObject(root, "gyro_turn_progress_deg", gyro.progress_deg);
    cJSON_AddStringToObject(root, "gyro_turn_stop_reason",
                            GyroTurnController::StopReasonName(gyro.stop_reason));
    cJSON_AddStringToObject(root, "motion_gesture", status.motion_gesture.c_str());
#endif
#ifdef SECONDARY_OLED_I2C_ADDRESS
    cJSON_AddBoolToObject(root, "oled_available", status.oled_available);
    const auto& oled_config = status.oled_config;
    cJSON_AddBoolToObject(root, "oled_flipped", oled_config.flip_180);
    cJSON_AddNumberToObject(root, "oled_contrast", oled_config.contrast);
    cJSON_AddNumberToObject(root, "oled_page_count", status.oled_page_count);
    cJSON_AddStringToObject(root, "oled_brand", oled_config.brand.c_str());
    cJSON_AddStringToObject(root, "oled_distance_prefix", oled_config.distance_prefix.c_str());
    cJSON* oled_widgets = cJSON_AddArrayToObject(root, "oled_widgets");
    if (oled_widgets != nullptr) {
        for (const auto& widget : oled_config.widgets) {
            cJSON* item = cJSON_CreateObject();
            if (item == nullptr) {
                break;
            }
            cJSON_AddStringToObject(item, "type",
                                    SecondaryDisplayController::WidgetTypeName(widget.type));
            cJSON_AddBoolToObject(item, "enabled", widget.enabled);
            cJSON_AddNumberToObject(item, "size", static_cast<int>(widget.size));
            cJSON_AddNumberToObject(item, "mode", widget.mode);
            cJSON_AddItemToArray(oled_widgets, item);
        }
    }
#endif
    cJSON_AddStringToObject(root, "version", status.version.c_str());
    cJSON_AddStringToObject(root, "ip", status.ip.c_str());
    cJSON_AddStringToObject(root, "ssid", status.ssid.c_str());
    cJSON_AddNumberToObject(root, "rssi", status.rssi);
    cJSON_AddNumberToObject(root, "uptime_sec", status.uptime_sec);
    cJSON_AddNumberToObject(root, "free_internal_bytes", status.free_internal_bytes);
    cJSON_AddNumberToObject(root, "free_psram_bytes", status.free_psram_bytes);
    return root;
}
