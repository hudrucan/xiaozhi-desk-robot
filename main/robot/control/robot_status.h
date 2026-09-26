#pragma once

#include "audio/acoustic_environment_status.h"
#include "audio/voice_input_config.h"
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
    std::string server_status_phase;
    bool asr_ready = false;
    bool asr_preparing = false;

    bool camera_available = false;
    bool camera_flipped = false;
    bool display_flipped = false;
    std::string emotion;
    int speaker_volume = 0;
    bool microphone_muted = false;
    int microphone_level = 0;
    bool microphone_clipping = false;
    VoiceInputStatus voice_input;
    int screen_brightness = 0;
    bool auto_brightness_enabled = false;
    int auto_brightness_minimum = 15;
    int auto_brightness_maximum = 85;
    int status_light_brightness = 0;
    bool live_camera_available = false;
    bool live_camera = false;
    bool web_camera_live = false;
    std::string camera_sensor = "unknown";
    std::string camera_mode = "off";
    std::string camera_profile = "normal";
    std::string camera_effective_profile = "normal";
    bool camera_auto_profile_available = false;
    std::string camera_request_state = "never";
    int64_t camera_request_age_sec = 0;
    uint32_t camera_capture_ms = 0;
    uint32_t camera_vision_ms = 0;
    size_t camera_image_bytes = 0;
    size_t camera_response_bytes = 0;

    int motor_speed = 0;
    int drive_duration_ms = 0;
    bool emotion_movement_enabled = false;
    bool emotion_movement_active = false;
    bool reaction_active = false;
    std::string reaction;
    int reaction_priority = 0;
    uint32_t reaction_generation = 0;
    int reaction_remaining_ms = 0;
    std::string reaction_motion_state = "not_requested";
    bool reaction_oled_active = false;
    MotorController::Status motors;

    CliffSensor::Status cliff;
    BatteryController::Status battery;
    EnvironmentStatus environment;
    AcousticEnvironmentStatus acoustic_environment;

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
    uint8_t oled_effective_contrast = 128;
    bool oled_auto_contrast_available = false;
    uint8_t oled_page_count = 0;

    std::string version = "unknown";
    std::string ip;
    std::string ssid = "—";
    int rssi = 0;
    int64_t uptime_sec = 0;
    std::string reset_reason = "unknown";
    std::string server_transport = "none";
    bool server_connected = false;
    size_t free_internal_bytes = 0;
    size_t total_internal_bytes = 0;
    size_t minimum_free_internal_bytes = 0;
    size_t free_psram_bytes = 0;
    size_t total_psram_bytes = 0;
};
