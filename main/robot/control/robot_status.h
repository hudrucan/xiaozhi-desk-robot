#pragma once

#include "display/secondary_oled.h"
#include "motion/gyro_turn_controller.h"
#include "motion/motor_controller.h"
#include "power/battery_controller.h"
#include "sensors/cliff_sensor.h"
#include "sensors/environment_types.h"

#include <cstddef>
#include <cstdint>
#include <string>

struct RobotStatus {
    std::string state = "unknown";
    bool asr_ready = false;
    bool asr_preparing = false;

    bool camera_available = false;
    bool camera_flipped = false;
    bool display_flipped = false;
    std::string emotion;
    int speaker_volume = 0;
    int microphone_gain = 1;
    int microphone_level = 0;
    bool microphone_clipping = false;
    int screen_brightness = 0;
    bool auto_brightness_enabled = false;
    int auto_brightness_minimum = 15;
    int auto_brightness_maximum = 85;
    int status_light_brightness = 0;
    bool live_camera_available = false;
    bool live_camera = false;

    int motor_speed = 0;
    int drive_duration_ms = 0;
    bool emotion_movement_enabled = false;
    bool emotion_movement_active = false;
    MotorController::Status motors;

    CliffSensor::Status cliff;
    BatteryController::Status battery;
    EnvironmentStatus environment;

    bool motion_sensor_available = false;
    bool motion_sensor_valid = false;
    bool motion_emotions_enabled = false;
    float motion_roll_deg = 0.0f;
    float motion_pitch_deg = 0.0f;
    float motion_acceleration_g = 0.0f;
    float motion_rotation_dps = 0.0f;
    std::string motion_gesture = "calibrating";
    GyroTurnController::Status gyro;

    bool oled_available = false;
    SecondaryOled::Config oled_config;
    uint8_t oled_page_count = 0;

    std::string version = "unknown";
    std::string ip;
    std::string ssid = "—";
    int rssi = 0;
    int64_t uptime_sec = 0;
    size_t free_internal_bytes = 0;
    size_t free_psram_bytes = 0;
    std::string last_reset_reason = "unknown";
    bool camera_operation_interrupted = false;
    std::string last_camera_stage = "none";
    uint32_t last_camera_operation_id = 0;
};
