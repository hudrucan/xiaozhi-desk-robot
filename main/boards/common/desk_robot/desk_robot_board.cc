#include "wifi_board.h"

#include "application.h"
#include "assets/lang_config.h"
#include "button.h"
#include "codecs/no_audio_codec.h"
#include "config.h"
#include "display/lcd_display.h"
#ifdef DESK_ROBOT_USE_ESP32_CAMERA
#include "esp32_camera.h"
#else
#include "esp_video.h"
#endif
#ifdef INA219_I2C_ADDRESS
#include "ina219_power_monitor.h"
#endif
#include "led/circular_strip.h"
#include "led/gpio_led.h"
#include "mcp_server.h"
#include "mochan_display.h"
#include "motor_controller.h"
#ifdef MPU6050_I2C_ADDRESS
#include "mpu6050_motion_sensor.h"
#endif
#include "robot_web_control_server.h"
#include "secondary_oled.h"
#include "settings.h"

#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <wifi_manager.h>

#ifdef DISTANCE_SENSOR_I2C_ADDRESS
extern "C" {
#include <vl53l0x.h>
}
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#define TAG "DeskRobotBoard"

#ifndef STATUS_LIGHT_DEFAULT_BRIGHTNESS
#define STATUS_LIGHT_DEFAULT_BRIGHTNESS 35
#endif

#ifndef CLIFF_EDGE_DISTANCE_MM
#define CLIFF_EDGE_DISTANCE_MM 150
#endif

#ifndef CLIFF_CONFIRM_SAMPLES
#define CLIFF_CONFIRM_SAMPLES 2
#endif

#ifndef CLIFF_AUTO_RETREAT_MS
#define CLIFF_AUTO_RETREAT_MS 180
#endif

#ifndef DISTANCE_SENSOR_PERIOD_MS
#define DISTANCE_SENSOR_PERIOD_MS 80
#endif

#ifndef MPU6050_SAMPLE_PERIOD_MS
#define MPU6050_SAMPLE_PERIOD_MS 40
#endif

#ifndef MPU6050_PRESS_THRESHOLD_G
#define MPU6050_PRESS_THRESHOLD_G 1.35f
#endif

#ifdef DESK_ROBOT_USE_ESP32_CAMERA
using DeskRobotCameraBase = Esp32Camera;
using DeskRobotCameraConfig = camera_config_t;
#else
using DeskRobotCameraBase = EspVideo;
using DeskRobotCameraConfig = esp_video_init_config_t;
#endif

class DeskRobotCamera : public DeskRobotCameraBase {
public:
    explicit DeskRobotCamera(const DeskRobotCameraConfig& config) : DeskRobotCameraBase(config) {}

    bool Capture() override {
        std::unique_lock<std::timed_mutex> lock(capture_mutex_, std::defer_lock);
        if (!lock.try_lock_for(std::chrono::seconds(7))) {
            ESP_LOGE(TAG, "MCP camera capture timed out waiting for live preview");
            return false;
        }
        if (mcp_frame_reserved_) {
            ESP_LOGW(TAG, "MCP camera capture rejected: previous frame is still reserved");
            return false;
        }
        ESP_LOGI(TAG, "MCP camera capture begin");
        const bool captured = DeskRobotCameraBase::Capture();
        ESP_LOGI(TAG, "MCP camera capture %s", captured ? "done" : "failed");
        mcp_frame_reserved_ = captured;
        return captured;
    }

    bool CapturePreview() {
        std::unique_lock<std::timed_mutex> lock(capture_mutex_, std::try_to_lock);
        if (!lock.owns_lock() || mcp_frame_reserved_) {
            return false;
        }
        return DeskRobotCameraBase::Capture();
    }

#ifdef DESK_ROBOT_USE_ESP32_CAMERA
    bool SendWebSnapshot(const RobotWebControlServer::SnapshotSender& sender) {
        std::unique_lock<std::timed_mutex> lock(capture_mutex_, std::defer_lock);
        if (!lock.try_lock_for(std::chrono::seconds(7)) || mcp_frame_reserved_) {
            return false;
        }
        if (!DeskRobotCameraBase::CaptureForWeb()) {
            return false;
        }
        const uint8_t* data = nullptr;
        size_t length = 0;
        return DeskRobotCameraBase::GetCurrentJpeg(data, length) && sender(data, length);
    }
#endif

    bool IsAvailable() const {
#ifdef DESK_ROBOT_USE_ESP32_CAMERA
        return DeskRobotCameraBase::IsAvailable();
#else
        return true;
#endif
    }

    std::expected<std::string, std::string> Explain(const std::string& question) override {
        std::unique_lock<std::timed_mutex> lock(capture_mutex_, std::defer_lock);
        if (!lock.try_lock_for(std::chrono::seconds(7))) {
            mcp_frame_reserved_ = false;
            return std::unexpected("Timed out waiting for camera frame");
        }
        ESP_LOGI(TAG, "MCP camera explain begin");
        auto result = DeskRobotCameraBase::Explain(question);
        mcp_frame_reserved_ = false;
        if (result) {
            ESP_LOGI(TAG, "MCP camera explain done");
        } else {
            ESP_LOGE(TAG, "MCP camera explain failed");
        }
        return result;
    }

private:
    std::timed_mutex capture_mutex_;
    std::atomic_bool mcp_frame_reserved_{false};
};

class DeskRobotBoard : public WifiBoard {
private:
    Button boot_button_;
    MochanDisplay* display_ = nullptr;
    DeskRobotCamera* camera_ = nullptr;
    MotorController motors_{MOTOR_LEFT_IN1, MOTOR_LEFT_IN2, MOTOR_RIGHT_IN1, MOTOR_RIGHT_IN2};
    std::unique_ptr<RobotWebControlServer> web_control_server_;
    std::atomic_bool camera_flipped_{false};
    std::atomic_bool display_flipped_{false};
    std::atomic_int speaker_volume_{70};
    std::atomic_int microphone_gain_{1};
    std::atomic_int status_light_brightness_{STATUS_LIGHT_DEFAULT_BRIGHTNESS};
    std::atomic_int status_light_saved_brightness_{STATUS_LIGHT_DEFAULT_BRIGHTNESS};
    std::atomic_bool live_camera_enabled_{false};
    std::atomic_bool motor_activity_active_{false};
    TaskHandle_t live_camera_task_ = nullptr;
    esp_timer_handle_t face_reset_timer_ = nullptr;
    esp_timer_handle_t oled_text_reset_timer_ = nullptr;
    esp_timer_handle_t light_effect_reset_timer_ = nullptr;
    std::mutex temporary_emotion_mutex_;
    std::string temporary_emotion_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
#ifdef AUXILIARY_I2C_SDA_PIN
    i2c_master_bus_handle_t auxiliary_i2c_bus_ = nullptr;
    std::mutex auxiliary_i2c_mutex_;
#endif
#ifdef INA219_I2C_ADDRESS
    Ina219PowerMonitor power_monitor_;
    std::atomic_bool battery_valid_{false};
    std::atomic_int battery_percent_{-1};
    std::atomic<float> battery_voltage_v_{0.0f};
    std::atomic<float> battery_current_ma_{0.0f};
    std::atomic<float> battery_power_mw_{0.0f};
    std::atomic_bool battery_charging_{false};
    std::atomic_bool battery_discharging_{false};
    std::atomic_bool battery_capacity_test_active_{false};
    std::atomic_bool battery_capacity_test_measuring_{false};
    std::atomic<uint32_t> battery_capacity_test_uah_{0};
    std::atomic<uint32_t> battery_capacity_test_seconds_{0};
#endif
#ifdef MPU6050_I2C_ADDRESS
    enum class MotionGesture : uint8_t {
        kCalibrating,
        kSteady,
        kLeft,
        kRight,
        kUp,
        kDown,
        kUpLeft,
        kUpRight,
        kDownLeft,
        kDownRight,
        kShake,
        kSurprised,
        kSleepy,
    };
    Mpu6050MotionSensor motion_sensor_;
    std::atomic_bool motion_sensor_valid_{false};
    std::atomic_bool motion_emotions_enabled_{true};
    std::atomic<float> motion_roll_deg_{0.0f};
    std::atomic<float> motion_pitch_deg_{0.0f};
    std::atomic<float> motion_acceleration_g_{0.0f};
    std::atomic<float> motion_rotation_dps_{0.0f};
    std::atomic<MotionGesture> motion_gesture_{MotionGesture::kCalibrating};
    std::atomic_bool press_reaction_pending_{false};
#endif
#if defined(INA219_I2C_ADDRESS) || defined(MPU6050_I2C_ADDRESS)
    TaskHandle_t auxiliary_sensor_task_ = nullptr;
#endif
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
    i2c_master_bus_handle_t camera_i2c_bus_ = nullptr;
    vl53l0x_handle_t distance_sensor_ = nullptr;
    TaskHandle_t distance_task_ = nullptr;
    std::atomic_int distance_mm_{-1};
    std::atomic_bool distance_valid_{false};
    std::atomic_bool cliff_detected_{false};
    std::atomic_bool cliff_retreat_pending_{false};
    std::atomic_int cliff_edge_mm_{CLIFF_EDGE_DISTANCE_MM};
#endif
#ifdef SECONDARY_OLED_I2C_ADDRESS
    SecondaryOled secondary_oled_;
    TaskHandle_t secondary_oled_task_ = nullptr;
#endif

#ifdef AUXILIARY_I2C_SDA_PIN
    void InitializeAuxiliaryI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = AUXILIARY_I2C_PORT,
            .sda_io_num = AUXILIARY_I2C_SDA_PIN,
            .scl_io_num = AUXILIARY_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {.enable_internal_pullup = true},
        };
        const esp_err_t error = i2c_new_master_bus(&bus_config, &auxiliary_i2c_bus_);
        if (error != ESP_OK) {
            auxiliary_i2c_bus_ = nullptr;
            ESP_LOGW(TAG, "Cannot create auxiliary I2C bus on SDA GPIO%d/SCL GPIO%d: %s",
                     AUXILIARY_I2C_SDA_PIN, AUXILIARY_I2C_SCL_PIN, esp_err_to_name(error));
            return;
        }
        ESP_LOGI(TAG, "Auxiliary I2C bus ready on SDA GPIO%d/SCL GPIO%d", AUXILIARY_I2C_SDA_PIN,
                 AUXILIARY_I2C_SCL_PIN);
    }
#endif

#ifdef INA219_I2C_ADDRESS
    void InitializePowerMonitor() {
        Settings settings("desk_robot", false);
        battery_capacity_test_active_.store(settings.GetBool("cap_test_on", false));
        battery_capacity_test_uah_.store(
            static_cast<uint32_t>(std::max(settings.GetInt("cap_test_uah", 0), int32_t{0})));
        battery_capacity_test_seconds_.store(
            static_cast<uint32_t>(std::max(settings.GetInt("cap_test_sec", 0), int32_t{0})));
        std::lock_guard<std::mutex> lock(auxiliary_i2c_mutex_);
        if (auxiliary_i2c_bus_ == nullptr ||
            i2c_master_probe(auxiliary_i2c_bus_, INA219_I2C_ADDRESS, 100) != ESP_OK) {
            ESP_LOGW(TAG, "INA219 not detected at 0x%02x", INA219_I2C_ADDRESS);
            return;
        }
        if (!power_monitor_.Initialize(auxiliary_i2c_bus_, INA219_I2C_ADDRESS,
                                       INA219_SHUNT_RESISTANCE_OHMS)) {
            ESP_LOGW(TAG, "INA219 initialization failed");
        }
    }

    void PersistBatteryCapacityTest() {
        Settings settings("desk_robot", true);
        settings.SetBool("cap_test_on", battery_capacity_test_active_.load());
        settings.SetInt("cap_test_uah", static_cast<int32_t>(battery_capacity_test_uah_.load()));
        settings.SetInt("cap_test_sec",
                        static_cast<int32_t>(battery_capacity_test_seconds_.load()));
    }
#endif

#ifdef MPU6050_I2C_ADDRESS
    static float NormalizeMotionAngle(float angle_deg) {
        while (angle_deg > 180.0f) {
            angle_deg -= 360.0f;
        }
        while (angle_deg < -180.0f) {
            angle_deg += 360.0f;
        }
        return angle_deg;
    }

    static const char* MotionGestureName(MotionGesture gesture) {
        switch (gesture) {
            case MotionGesture::kSteady:
                return "steady";
            case MotionGesture::kLeft:
                return "left";
            case MotionGesture::kRight:
                return "right";
            case MotionGesture::kUp:
                return "up";
            case MotionGesture::kDown:
                return "down";
            case MotionGesture::kUpLeft:
                return "up_left";
            case MotionGesture::kUpRight:
                return "up_right";
            case MotionGesture::kDownLeft:
                return "down_left";
            case MotionGesture::kDownRight:
                return "down_right";
            case MotionGesture::kShake:
                return "shake";
            case MotionGesture::kSurprised:
                return "surprised";
            case MotionGesture::kSleepy:
                return "sleepy";
            case MotionGesture::kCalibrating:
                return "calibrating";
        }
        return "calibrating";
    }

    bool AreMotorsMoving() const {
        return motors_.IsMoving(MotorController::Direction::kForward) ||
               motors_.IsMoving(MotorController::Direction::kBackward) ||
               motors_.IsMoving(MotorController::Direction::kLeft) ||
               motors_.IsMoving(MotorController::Direction::kRight);
    }

    bool InitializeMotionSensor() {
        Settings settings("desk_robot", false);
        motion_emotions_enabled_.store(settings.GetBool("motion_emotions", true));
        std::lock_guard<std::mutex> lock(auxiliary_i2c_mutex_);
        if (auxiliary_i2c_bus_ == nullptr ||
            !motion_sensor_.Initialize(auxiliary_i2c_bus_, MPU6050_I2C_ADDRESS)) {
            ESP_LOGW(TAG, "MPU6050 not detected at 0x68 or 0x69");
            return false;
        }
        return true;
    }

    void RegisterMotionTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool(
            "self.motion.get_orientation",
            "Get the MPU6050 orientation, acceleration, rotation, and detected gesture.",
            PropertyList(), [this](const PropertyList&) -> ToolResult {
                cJSON* result = cJSON_CreateObject();
                if (result == nullptr) {
                    return std::unexpected("Out of memory");
                }
                cJSON_AddBoolToObject(result, "available", motion_sensor_.IsAvailable());
                cJSON_AddBoolToObject(result, "valid", motion_sensor_valid_.load());
                cJSON_AddBoolToObject(result, "emotion_control", motion_emotions_enabled_.load());
                if (motion_sensor_valid_.load()) {
                    cJSON_AddNumberToObject(result, "roll_deg", motion_roll_deg_.load());
                    cJSON_AddNumberToObject(result, "pitch_deg", motion_pitch_deg_.load());
                    cJSON_AddNumberToObject(result, "acceleration_g",
                                            motion_acceleration_g_.load());
                    cJSON_AddNumberToObject(result, "rotation_dps", motion_rotation_dps_.load());
                    cJSON_AddStringToObject(result, "gesture",
                                            MotionGestureName(motion_gesture_.load()));
                }
                return result;
            });
        mcp_server.AddTool("self.motion.set_emotion_control",
                           "Enable or disable automatic face reactions from MPU6050 movement.",
                           PropertyList({Property("enabled", kPropertyTypeBoolean, true)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               const bool enabled = properties["enabled"].value<bool>();
                               motion_emotions_enabled_.store(enabled);
                               Application::GetInstance().Schedule([enabled]() {
                                   Settings settings("desk_robot", true);
                                   settings.SetBool("motion_emotions", enabled);
                               });
                               return true;
                           });
        ESP_LOGI(TAG, "MPU6050 MCP tools registered");
    }
#endif

#if defined(INA219_I2C_ADDRESS) || defined(MPU6050_I2C_ADDRESS)
    static void AuxiliarySensorTask(void* arg) {
        static_cast<DeskRobotBoard*>(arg)->RunAuxiliarySensorTask();
    }

    void RunAuxiliarySensorTask() {
        TickType_t last_wake_time = xTaskGetTickCount();
#ifdef INA219_I2C_ADDRESS
        int64_t next_power_sample_us = 0;
        bool power_filter_initialized = false;
        float filtered_voltage_v = 0.0f;
        float filtered_current_ma = 0.0f;
        float filtered_power_mw = 0.0f;
        float filtered_percent = 0.0f;
        unsigned power_failures = 0;
        bool capacity_test_was_active = false;
        int64_t capacity_previous_sample_us = 0;
        int64_t capacity_last_save_us = 0;
        double capacity_fractional_uah = 0.0;
        int64_t capacity_fractional_time_us = 0;
        unsigned capacity_low_voltage_samples = 0;
#endif
#ifdef MPU6050_I2C_ADDRESS
        constexpr int kCalibrationSamples = 50;
        int calibration_samples = 0;
        float calibration_roll_reference = 0.0f;
        float calibration_pitch_reference = 0.0f;
        float calibration_roll_sum = 0.0f;
        float calibration_pitch_sum = 0.0f;
        float roll_offset_deg = 0.0f;
        float pitch_offset_deg = 0.0f;
        MotionGesture candidate_gesture = MotionGesture::kCalibrating;
        int candidate_samples = 0;
        int64_t last_gesture_us = 0;
        unsigned motion_failures = 0;
#endif

        while (true) {
            const int64_t now_us = esp_timer_get_time();
#ifdef INA219_I2C_ADDRESS
            if (power_monitor_.IsAvailable() && now_us >= next_power_sample_us) {
                next_power_sample_us = now_us + INA219_SAMPLE_PERIOD_MS * 1000LL;
                Ina219PowerMonitor::Reading reading;
                bool read_ok = false;
                {
                    std::lock_guard<std::mutex> lock(auxiliary_i2c_mutex_);
                    read_ok = power_monitor_.Read(reading);
                }
                if (read_ok) {
                    constexpr float kFilterAlpha = 0.25f;
                    if (!power_filter_initialized) {
                        filtered_voltage_v = reading.bus_voltage_v;
                        filtered_current_ma = reading.current_ma;
                        filtered_power_mw = reading.power_mw;
                        filtered_percent = static_cast<float>(reading.battery_percent);
                        power_filter_initialized = true;
                    } else {
                        filtered_voltage_v +=
                            kFilterAlpha * (reading.bus_voltage_v - filtered_voltage_v);
                        filtered_current_ma +=
                            kFilterAlpha * (reading.current_ma - filtered_current_ma);
                        filtered_power_mw += kFilterAlpha * (reading.power_mw - filtered_power_mw);
                        filtered_percent +=
                            kFilterAlpha * (reading.battery_percent - filtered_percent);
                    }
                    battery_voltage_v_.store(filtered_voltage_v);
                    battery_current_ma_.store(std::fabs(filtered_current_ma));
                    battery_power_mw_.store(std::fabs(filtered_power_mw));
                    battery_percent_.store(
                        std::clamp(static_cast<int>(std::lround(filtered_percent)), 0, 100));
                    battery_charging_.store(reading.charging);
                    battery_discharging_.store(reading.discharging);
                    battery_valid_.store(true);
                    const bool capacity_active = battery_capacity_test_active_.load();
                    if (capacity_active && !capacity_test_was_active) {
                        capacity_previous_sample_us = now_us;
                        capacity_last_save_us = now_us;
                        capacity_fractional_uah = 0.0;
                        capacity_fractional_time_us = 0;
                    }
                    const bool capacity_measuring = capacity_active && reading.discharging;
                    battery_capacity_test_measuring_.store(capacity_measuring);
                    if (capacity_measuring && capacity_test_was_active &&
                        capacity_previous_sample_us > 0) {
                        const int64_t elapsed_us =
                            std::clamp(now_us - capacity_previous_sample_us, int64_t{0},
                                       int64_t{INA219_SAMPLE_PERIOD_MS * 3000LL});
                        // mA * us / 3,600,000 = uAh.
                        capacity_fractional_uah +=
                            std::fabs(static_cast<double>(reading.current_ma)) * elapsed_us /
                            3600000.0;
                        const uint32_t whole_uah = static_cast<uint32_t>(capacity_fractional_uah);
                        if (whole_uah > 0) {
                            battery_capacity_test_uah_.fetch_add(whole_uah);
                            capacity_fractional_uah -= whole_uah;
                        }
                        capacity_fractional_time_us += elapsed_us;
                        const uint32_t whole_seconds =
                            static_cast<uint32_t>(capacity_fractional_time_us / 1000000LL);
                        if (whole_seconds > 0) {
                            battery_capacity_test_seconds_.fetch_add(whole_seconds);
                            capacity_fractional_time_us -=
                                static_cast<int64_t>(whole_seconds) * 1000000LL;
                        }
                    }
                    if (capacity_measuring && reading.bus_voltage_v <= 3.20f) {
                        ++capacity_low_voltage_samples;
                    } else {
                        capacity_low_voltage_samples = 0;
                    }
                    if (capacity_low_voltage_samples >= 10) {
                        battery_capacity_test_active_.store(false);
                        battery_capacity_test_measuring_.store(false);
                        PersistBatteryCapacityTest();
                        capacity_low_voltage_samples = 0;
                        ESP_LOGI(TAG, "Battery capacity measurement stopped at low voltage");
                    }
                    if (capacity_measuring && now_us - capacity_last_save_us >= 60000000LL) {
                        PersistBatteryCapacityTest();
                        capacity_last_save_us = now_us;
                    }
                    capacity_previous_sample_us = now_us;
                    capacity_test_was_active = capacity_active;
                    power_failures = 0;
                    const int percent = battery_percent_.load();
                    const float voltage = battery_voltage_v_.load();
                    const bool charging = battery_charging_.load();
                    Application::GetInstance().Schedule([this, percent, voltage, charging]() {
                        display_->SetBatteryStatus(percent, voltage, charging);
                    });
                } else {
                    battery_valid_.store(false);
                    battery_capacity_test_measuring_.store(false);
                    if (++power_failures == 1 || power_failures % 30 == 0) {
                        ESP_LOGW(TAG, "INA219 read failed (%u consecutive)", power_failures);
                    }
                }
            }
#endif

#ifdef MPU6050_I2C_ADDRESS
            if (motion_sensor_.IsAvailable()) {
                Mpu6050MotionSensor::Sample sample;
                bool read_ok = false;
                {
                    std::lock_guard<std::mutex> lock(auxiliary_i2c_mutex_);
                    read_ok = motion_sensor_.Read(sample);
                }
                if (!read_ok) {
                    motion_sensor_valid_.store(false);
                    if (++motion_failures == 1 || motion_failures % 250 == 0) {
                        ESP_LOGW(TAG, "MPU6050 read failed (%u consecutive)", motion_failures);
                    }
                } else {
                    motion_failures = 0;
                    if (calibration_samples < kCalibrationSamples) {
                        if (calibration_samples == 0) {
                            calibration_roll_reference = sample.roll_deg;
                            calibration_pitch_reference = sample.pitch_deg;
                        }
                        calibration_roll_sum +=
                            NormalizeMotionAngle(sample.roll_deg - calibration_roll_reference);
                        calibration_pitch_sum +=
                            NormalizeMotionAngle(sample.pitch_deg - calibration_pitch_reference);
                        ++calibration_samples;
                        if (calibration_samples == kCalibrationSamples) {
                            roll_offset_deg =
                                NormalizeMotionAngle(calibration_roll_reference +
                                                     calibration_roll_sum / kCalibrationSamples);
                            pitch_offset_deg =
                                NormalizeMotionAngle(calibration_pitch_reference +
                                                     calibration_pitch_sum / kCalibrationSamples);
                            motion_sensor_valid_.store(true);
                            motion_gesture_.store(MotionGesture::kSteady);
                            ESP_LOGI(TAG, "MPU6050 orientation calibrated: roll %.1f, pitch %.1f",
                                     roll_offset_deg, pitch_offset_deg);
                        }
                    } else {
                        const float roll = NormalizeMotionAngle(sample.roll_deg - roll_offset_deg);
                        const float pitch =
                            NormalizeMotionAngle(sample.pitch_deg - pitch_offset_deg);
                        motion_roll_deg_.store(roll);
                        motion_pitch_deg_.store(pitch);
                        motion_acceleration_g_.store(sample.acceleration_magnitude_g);
                        motion_rotation_dps_.store(sample.rotation_magnitude_dps);
                        motion_sensor_valid_.store(true);

                        const bool press_impulse =
                            sample.acceleration_magnitude_g > MPU6050_PRESS_THRESHOLD_G;
                        MotionGesture gesture = MotionGesture::kSteady;
                        if (sample.acceleration_magnitude_g < MPU6050_FREEFALL_THRESHOLD_G ||
                            sample.acceleration_magnitude_g > MPU6050_IMPACT_THRESHOLD_G ||
                            press_impulse) {
                            gesture = MotionGesture::kSurprised;
                        } else if (sample.rotation_magnitude_dps > MPU6050_SHAKE_THRESHOLD_DPS) {
                            gesture = MotionGesture::kShake;
                        } else if (std::fabs(pitch) > MPU6050_TILT_THRESHOLD_DEG) {
                            // Pitch is authoritative for nose-up/down. Near those poses Euler roll
                            // can legitimately approach 180 degrees even though the robot is not
                            // upside down. Only use moderate roll for diagonal looks.
                            const bool diagonal = std::fabs(roll) > MPU6050_TILT_THRESHOLD_DEG &&
                                                  std::fabs(roll) < 75.0f;
                            if (pitch > 0.0f) {
                                gesture = !diagonal     ? MotionGesture::kUp
                                          : roll < 0.0f ? MotionGesture::kUpLeft
                                                        : MotionGesture::kUpRight;
                            } else {
                                gesture = !diagonal     ? MotionGesture::kDown
                                          : roll < 0.0f ? MotionGesture::kDownLeft
                                                        : MotionGesture::kDownRight;
                            }
                        } else if (std::fabs(roll) > 150.0f) {
                            gesture = MotionGesture::kSleepy;
                        } else if (std::fabs(roll) > MPU6050_TILT_THRESHOLD_DEG) {
                            gesture = roll < 0.0f ? MotionGesture::kLeft : MotionGesture::kRight;
                        }
                        motion_gesture_.store(gesture);

                        const bool can_animate =
                            motion_emotions_enabled_.load() &&
                            Application::GetInstance().GetDeviceState() == kDeviceStateIdle &&
                            !motor_activity_active_.load(std::memory_order_relaxed);
                        if (!can_animate || gesture == MotionGesture::kSteady) {
                            candidate_gesture = MotionGesture::kCalibrating;
                            candidate_samples = 0;
                        } else {
                            if (gesture == candidate_gesture) {
                                ++candidate_samples;
                            } else {
                                candidate_gesture = gesture;
                                candidate_samples = 1;
                            }
                            bool face_busy = false;
                            {
                                std::lock_guard<std::mutex> lock(temporary_emotion_mutex_);
                                face_busy = !temporary_emotion_.empty();
                            }
                            if (press_impulse && !face_busy &&
                                now_us - last_gesture_us >= MPU6050_GESTURE_COOLDOWN_MS * 1000LL &&
                                QueuePressReaction()) {
                                last_gesture_us = now_us;
                                candidate_gesture = MotionGesture::kCalibrating;
                                candidate_samples = 0;
                            } else if (candidate_samples >= 3 && !face_busy &&
                                       now_us - last_gesture_us >=
                                           MPU6050_GESTURE_COOLDOWN_MS * 1000LL) {
                                const int duration_ms = gesture == MotionGesture::kShake ||
                                                                gesture == MotionGesture::kSurprised
                                                            ? 1400
                                                            : 1800;
                                if (QueueTemporaryEmotion(MotionGestureName(gesture),
                                                          duration_ms)) {
                                    last_gesture_us = now_us;
                                }
                                candidate_samples = 0;
                            }
                        }
                    }
                }
            }
#endif
            vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(MPU6050_SAMPLE_PERIOD_MS));
        }
    }

    void StartAuxiliarySensorTask() {
        const bool has_sensor =
#ifdef INA219_I2C_ADDRESS
            power_monitor_.IsAvailable() ||
#endif
#ifdef MPU6050_I2C_ADDRESS
            motion_sensor_.IsAvailable() ||
#endif
            false;
        if (!has_sensor) {
            return;
        }
        if (xTaskCreate(AuxiliarySensorTask, "aux_sensors", 6144, this, 2,
                        &auxiliary_sensor_task_) != pdPASS) {
            auxiliary_sensor_task_ = nullptr;
            ESP_LOGE(TAG, "Failed to create auxiliary sensor task");
        }
    }
#endif

    void OnNetworkEvent(NetworkEvent event, const std::string& data = "") override {
        WifiBoard::OnNetworkEvent(event, data);

        switch (event) {
            case NetworkEvent::Scanning:
            case NetworkEvent::Connecting:
            case NetworkEvent::Disconnected:
                display_->SetWifiConnected(false);
                display_->ShowBootSplash();
                break;
            case NetworkEvent::Connected:
                display_->SetWifiConnected(true);
                display_->HideBootSplash();
                Application::GetInstance().Schedule([this]() {
                    if (web_control_server_ != nullptr && web_control_server_->Start(8080)) {
                        ESP_LOGI(TAG, "Local control: http://%s:8080",
                                 WifiManager::GetInstance().GetIpAddress().c_str());
                    }
                });
                break;
            case NetworkEvent::WifiConfigModeEnter:
                display_->SetWifiConnected(false);
                display_->HideBootSplash();
                break;
            default:
                break;
        }
    }

    void InitializeSpi() {
        spi_bus_config_t config = {};
        config.mosi_io_num = DISPLAY_MOSI_PIN;
        config.miso_io_num = GPIO_NUM_NC;
        config.sclk_io_num = DISPLAY_SCLK_PIN;
        config.quadwp_io_num = GPIO_NUM_NC;
        config.quadhd_io_num = GPIO_NUM_NC;
        config.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &config, SPI_DMA_CH_AUTO));
    }

    void InitializeDisplay() {
        Settings display_settings("desk_robot", false);
        const bool display_flipped = display_settings.GetBool("display_flip", false);
        display_flipped_.store(display_flipped);
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
#ifdef DISPLAY_SPI_CLOCK_HZ
        io_config.pclk_hz = DISPLAY_SPI_CLOCK_HZ;
#else
        io_config.pclk_hz = 40 * 1000 * 1000;
#endif
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_SPI_HOST, &io_config, &panel_io_));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io_, &panel_config, &panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, DISPLAY_INVERT_COLOR));
        // Rotation is applied by lvgl_port through SpiLcdDisplay. Applying the same transform to
        // the panel here as well rotates the flush coordinates twice and clips the rendered UI.
        display_ =
            new MochanDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X,
                              DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X ^ display_flipped,
                              DISPLAY_MIRROR_Y ^ display_flipped, DISPLAY_SWAP_XY);
#if defined(DISPLAY_PANEL_GAP_X) && defined(DISPLAY_PANEL_GAP_Y)
        // A 240x240 ST7789 panel addresses a 240x320 controller RAM. After swapping X/Y,
        // shift the panel window onto the visible 240-pixel area instead of clipping 80 pixels.
        ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_, display_flipped ? 0 : DISPLAY_PANEL_GAP_X,
                                              DISPLAY_PANEL_GAP_Y));
#endif
        display_->ShowBootSplash();
    }

    void InitializeCamera() {
#ifdef DESK_ROBOT_USE_ESP32_CAMERA
        camera_config_t config = {};
        config.pin_d0 = CAMERA_PIN_D0;
        config.pin_d1 = CAMERA_PIN_D1;
        config.pin_d2 = CAMERA_PIN_D2;
        config.pin_d3 = CAMERA_PIN_D3;
        config.pin_d4 = CAMERA_PIN_D4;
        config.pin_d5 = CAMERA_PIN_D5;
        config.pin_d6 = CAMERA_PIN_D6;
        config.pin_d7 = CAMERA_PIN_D7;
        config.pin_xclk = CAMERA_PIN_XCLK;
        config.pin_pclk = CAMERA_PIN_PCLK;
        config.pin_vsync = CAMERA_PIN_VSYNC;
        config.pin_href = CAMERA_PIN_HREF;
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
        // Reuse the new-driver I2C0 bus already created for the VL53L0X.
        config.pin_sccb_sda = GPIO_NUM_NC;
        config.pin_sccb_scl = GPIO_NUM_NC;
        config.sccb_i2c_port = I2C_NUM_0;
#else
        config.pin_sccb_sda = CAMERA_PIN_SIOD;
        config.pin_sccb_scl = CAMERA_PIN_SIOC;
        config.sccb_i2c_port = I2C_NUM_0;
#endif
        config.pin_pwdn = CAMERA_PIN_PWDN;
        config.pin_reset = CAMERA_PIN_RESET;
        config.xclk_freq_hz = CAMERA_XCLK_FREQ_HZ;
        config.ledc_timer = LEDC_TIMER_0;
        config.ledc_channel = LEDC_CHANNEL_0;
        config.pixel_format = PIXFORMAT_JPEG;
        config.frame_size = FRAMESIZE_VGA;
        config.jpeg_quality = 12;
        config.fb_count = 2;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_LATEST;
        camera_ = new DeskRobotCamera(config);
#else
        static esp_cam_ctlr_dvp_pin_config_t dvp_pin_config = {
            .data_width = CAM_CTLR_DATA_WIDTH_8,
            .data_io =
                {
                    [0] = CAMERA_PIN_D0,
                    [1] = CAMERA_PIN_D1,
                    [2] = CAMERA_PIN_D2,
                    [3] = CAMERA_PIN_D3,
                    [4] = CAMERA_PIN_D4,
                    [5] = CAMERA_PIN_D5,
                    [6] = CAMERA_PIN_D6,
                    [7] = CAMERA_PIN_D7,
                },
            .vsync_io = CAMERA_PIN_VSYNC,
            .de_io = CAMERA_PIN_HREF,
            .pclk_io = CAMERA_PIN_PCLK,
            .xclk_io = CAMERA_PIN_XCLK,
        };

        esp_video_init_sccb_config_t sccb_config = {
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
            .init_sccb = false,
            .i2c_handle = camera_i2c_bus_,
#else
            .init_sccb = true,
            .i2c_config =
                {
                    .port = 0,
                    .scl_pin = CAMERA_PIN_SIOC,
                    .sda_pin = CAMERA_PIN_SIOD,
                },
#endif
            .freq = 100000,
        };

        esp_video_init_dvp_config_t dvp_config = {
            .sccb_config = sccb_config,
            .reset_pin = CAMERA_PIN_RESET,
            .pwdn_pin = CAMERA_PIN_PWDN,
            .dvp_pin = dvp_pin_config,
            .xclk_freq = CAMERA_XCLK_FREQ_HZ,
        };

        esp_video_init_config_t video_config = {
            .dvp = &dvp_config,
        };
        camera_ = new DeskRobotCamera(video_config);
#endif

        Settings settings("desk_robot", false);
        const bool flipped = settings.GetBool("camera_flip", false);
        camera_flipped_.store(flipped);
        camera_->SetHMirror(flipped);
        camera_->SetVFlip(flipped);
    }

#ifdef DISTANCE_SENSOR_I2C_ADDRESS
    void InitializeCameraI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = DISTANCE_SENSOR_SDA_PIN,
            .scl_io_num = DISTANCE_SENSOR_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {.enable_internal_pullup = true},
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &camera_i2c_bus_));
    }

    static void DistanceTask(void* arg) {
        auto* self = static_cast<DeskRobotBoard*>(arg);
        TickType_t last_wake_time = xTaskGetTickCount();
        uint8_t unsafe_samples = 0;
        while (true) {
            vl53l0x_data_t reading = {};
            const esp_err_t error = vl53l0x_single_measure(self->distance_sensor_, &reading);
            if (error == ESP_OK) {
                self->distance_mm_.store(reading.distance_mm);
                self->distance_valid_.store(reading.valid && reading.distance_mm > 0);
            } else {
                self->distance_valid_.store(false);
                ESP_LOGW(TAG, "VL53L0X measurement failed: %s", esp_err_to_name(error));
            }

            // The sensor points down at the table. A close, valid return means floor is still
            // present; a distant or missing return means the robot is approaching an edge.
            const int edge_mm = self->cliff_edge_mm_.load(std::memory_order_relaxed);
            const bool floor_detected = error == ESP_OK && reading.valid &&
                                        reading.distance_mm > 0 && reading.distance_mm <= edge_mm;
            if (floor_detected) {
                unsafe_samples = 0;
                self->cliff_detected_.store(false);
            } else {
                unsafe_samples = std::min<uint8_t>(unsafe_samples + 1, CLIFF_CONFIRM_SAMPLES);
                if (unsafe_samples >= CLIFF_CONFIRM_SAMPLES &&
                    !self->cliff_detected_.exchange(true)) {
                    if (reading.valid && reading.distance_mm > 0) {
                        ESP_LOGW(TAG, "Cliff detected: floor is %u mm away", reading.distance_mm);
                    } else {
                        ESP_LOGW(TAG, "Cliff detected: no valid floor return");
                    }
                    const bool was_moving_forward =
                        self->motors_.IsMoving(MotorController::Direction::kForward);
                    const bool was_moving_unsafe =
                        was_moving_forward ||
                        self->motors_.IsMoving(MotorController::Direction::kLeft) ||
                        self->motors_.IsMoving(MotorController::Direction::kRight);
                    if (was_moving_unsafe) {
                        self->motors_.EmergencyStop();
                        if (was_moving_forward) {
                            self->QueueCliffRetreat();
                        }
                    }
                }
            }
            vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(DISTANCE_SENSOR_PERIOD_MS));
        }
    }

    bool IsCliffDetected() const { return distance_sensor_ != nullptr && cliff_detected_.load(); }

    bool IsDirectionBlockedByCliff(MotorController::Direction direction) const {
        return IsCliffDetected() && direction != MotorController::Direction::kBackward;
    }

    void QueueCliffRetreat() {
        if (cliff_retreat_pending_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        Application::GetInstance().Schedule([this]() {
            // EmergencyStop already removed bridge power in the sensor task. Normalize any queued
            // sequence once more on the application task before issuing the single bounded retreat.
            motors_.Stop();
            if (IsCliffDetected()) {
                ESP_LOGI(TAG, "Backing away from cliff for %d ms", CLIFF_AUTO_RETREAT_MS);
                motors_.Drive(MotorController::Direction::kBackward, CLIFF_AUTO_RETREAT_MS);
            }
            cliff_retreat_pending_.store(false, std::memory_order_release);
        });
    }

    void InitializeCliffSettings() {
        Settings settings("desk_robot", false);
        const int edge_mm = std::clamp(
            static_cast<int>(settings.GetInt("cliff_edge_mm", CLIFF_EDGE_DISTANCE_MM)), 50, 500);
        cliff_edge_mm_.store(edge_mm, std::memory_order_relaxed);
        ESP_LOGI(TAG, "Cliff threshold set to %d mm", edge_mm);
    }

    void QueueCliffThreshold(int edge_mm) {
        const int safe_edge_mm = std::clamp(edge_mm, 50, 500);
        cliff_edge_mm_.store(safe_edge_mm, std::memory_order_relaxed);
        Application::GetInstance().Schedule([safe_edge_mm]() {
            Settings settings("desk_robot", true);
            settings.SetInt("cliff_edge_mm", safe_edge_mm);
        });
    }

    void InitializeDistanceSensor() {
        if (i2c_master_probe(camera_i2c_bus_, DISTANCE_SENSOR_I2C_ADDRESS, 100) != ESP_OK) {
            ESP_LOGW(TAG, "VL53L0X not detected at 0x%02x", DISTANCE_SENSOR_I2C_ADDRESS);
            return;
        }
        esp_err_t error = vl53l0x_create(&distance_sensor_, camera_i2c_bus_);
        if (error == ESP_OK) {
            error = vl53l0x_init(distance_sensor_);
        }
        if (error == ESP_OK) {
            vl53l0x_ref_spad_calibration_t spad_calibration = {};
            error = vl53l0x_perform_ref_spad_management(distance_sensor_, &spad_calibration);
            if (error == ESP_OK) {
                error = vl53l0x_set_reference_spads(distance_sensor_, &spad_calibration);
            }
        }
        if (error == ESP_OK) {
            vl53l0x_ref_calibration_t reference_calibration = {};
            error = vl53l0x_perform_ref_calibration(distance_sensor_, &reference_calibration);
        }
        if (error == ESP_OK) {
            error = vl53l0x_set_profile(distance_sensor_, VL53L0X_PROFILE_DEFAULT);
        }
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "VL53L0X initialization failed: %s", esp_err_to_name(error));
            if (distance_sensor_ != nullptr) {
                vl53l0x_destroy(distance_sensor_);
                distance_sensor_ = nullptr;
            }
            return;
        }
        if (xTaskCreate(DistanceTask, "vl53l0x", 4096, this, 1, &distance_task_) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create VL53L0X task");
            vl53l0x_destroy(distance_sensor_);
            distance_sensor_ = nullptr;
            return;
        }
        ESP_LOGI(TAG, "VL53L0X ready on shared camera I2C bus");
    }
#endif

#ifdef SECONDARY_OLED_I2C_ADDRESS
    static void SecondaryOledTask(void* arg) {
        auto* self = static_cast<DeskRobotBoard*>(arg);
        // Let the board constructor and application singleton finish before reading runtime state.
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_LOGI(TAG, "Secondary OLED marquee task started");
        TickType_t last_wake_time = xTaskGetTickCount();
        std::string previous_state;
        int previous_distance = -2;
        bool previous_valid = false;
        int previous_battery_percent = -2;
        int previous_battery_centi_v = -1;
        int previous_battery_current_ma = 0;
        while (true) {
            const char* state_name =
                DeviceStateMachine::GetStateName(Application::GetInstance().GetDeviceState());
            const std::string state = state_name != nullptr ? state_name : "starting";
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
            const int distance = self->distance_mm_.load();
            const bool valid = self->distance_valid_.load();
#else
            const int distance = -1;
            const bool valid = false;
#endif
#ifdef INA219_I2C_ADDRESS
            const int battery_percent =
                self->battery_valid_.load() ? self->battery_percent_.load() : -1;
            const float battery_voltage = self->battery_voltage_v_.load();
            const int battery_centi_v = static_cast<int>(std::lround(battery_voltage * 100.0f));
            const float battery_current = self->battery_current_ma_.load();
            const int battery_current_ma = static_cast<int>(std::lround(battery_current));
#else
            const int battery_percent = -1;
            const float battery_voltage = 0.0f;
            const int battery_centi_v = 0;
            const float battery_current = 0.0f;
            const int battery_current_ma = 0;
#endif
            if (state != previous_state || distance != previous_distance ||
                valid != previous_valid || battery_percent != previous_battery_percent ||
                battery_centi_v != previous_battery_centi_v ||
                battery_current_ma != previous_battery_current_ma) {
                self->secondary_oled_.ShowStatus(state, distance, valid, battery_percent,
                                                 battery_voltage, battery_current);
                previous_state = state;
                previous_distance = distance;
                previous_valid = valid;
                previous_battery_percent = battery_percent;
                previous_battery_centi_v = battery_centi_v;
                previous_battery_current_ma = battery_current_ma;
            } else {
                self->secondary_oled_.Tick();
            }
            vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(40));
        }
    }

    void InitializeSecondaryOled() {
        Settings settings("desk_robot", false);
        SecondaryOled::Config oled_config;
        oled_config.flip_180 = settings.GetBool("oled_flip", SECONDARY_OLED_FLIP_180);
        oled_config.show_brand = settings.GetBool("oled_brand_on", true);
        oled_config.show_state = settings.GetBool("oled_state_on", true);
        oled_config.show_distance = settings.GetBool("oled_dist_on", true);
        oled_config.show_battery = settings.GetBool("oled_bat_on", true);
        oled_config.show_voltage = settings.GetBool("oled_volt_on", false);
        oled_config.show_current = settings.GetBool("oled_amp_on", false);
        oled_config.text_scale =
            std::clamp(static_cast<int>(settings.GetInt("oled_scale", 2)), 1, 3);
        oled_config.brand = settings.GetString("oled_brand", "Xiaozhi");
        oled_config.distance_prefix = settings.GetString("oled_prefix", "Dist");
        if (!secondary_oled_.Initialize(auxiliary_i2c_bus_, auxiliary_i2c_mutex_,
                                        SECONDARY_OLED_I2C_ADDRESS, SECONDARY_OLED_WIDTH,
                                        SECONDARY_OLED_HEIGHT, oled_config.flip_180)) {
            return;
        }
        secondary_oled_.Configure(oled_config);
        if (xTaskCreate(SecondaryOledTask, "status_oled", 6144, this, 2, &secondary_oled_task_) !=
            pdPASS) {
            secondary_oled_task_ = nullptr;
            ESP_LOGE(TAG, "Failed to create secondary OLED task");
        }
    }
#endif

    static bool ParseDirection(const std::string& direction, MotorController::Direction& command) {
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

    void ApplyCameraFlip(bool flipped) {
        camera_->SetHMirror(flipped);
        camera_->SetVFlip(flipped);
        Settings settings("desk_robot", true);
        settings.SetBool("camera_flip", flipped);
    }

    bool QueueCameraFlip() {
        const bool flipped = !camera_flipped_.load();
        camera_flipped_.store(flipped);
        Application::GetInstance().Schedule([this, flipped]() { ApplyCameraFlip(flipped); });
        return flipped;
    }

    void ApplyDisplayFlip(bool flipped) {
        if (!display_->SetPanelMirror(DISPLAY_MIRROR_X ^ flipped, DISPLAY_MIRROR_Y ^ flipped)) {
            display_flipped_.store(!flipped);
            return;
        }
#if defined(DISPLAY_PANEL_GAP_X) && defined(DISPLAY_PANEL_GAP_Y)
        // The ST7789 controller has 80 hidden rows. Mirroring reverses which side owns that
        // offset; keeping the unflipped gap would leave an 80-pixel black strip on the right.
        const esp_err_t gap_error =
            esp_lcd_panel_set_gap(panel_, flipped ? 0 : DISPLAY_PANEL_GAP_X, DISPLAY_PANEL_GAP_Y);
        if (gap_error != ESP_OK) {
            ESP_LOGW(TAG, "Cannot update display gap: %s", esp_err_to_name(gap_error));
        }
#endif
        Settings settings("desk_robot", true);
        settings.SetBool("display_flip", flipped);
    }

    bool QueueDisplayFlip() {
        const bool flipped = !display_flipped_.load();
        display_flipped_.store(flipped);
        Application::GetInstance().Schedule([this, flipped]() { ApplyDisplayFlip(flipped); });
        return flipped;
    }

#ifdef SECONDARY_OLED_I2C_ADDRESS
    static std::string NormalizeOledText(const std::string& text, const char* fallback) {
        std::string normalized;
        normalized.reserve(std::min<size_t>(text.size(), 20));
        bool previous_space = true;
        for (unsigned char character : text) {
            if (normalized.size() >= 20) {
                break;
            }
            if (std::isalnum(character) || character == '-') {
                normalized.push_back(static_cast<char>(character));
                previous_space = false;
            } else if (std::isspace(character) && !previous_space) {
                normalized.push_back(' ');
                previous_space = true;
            }
        }
        while (!normalized.empty() && normalized.back() == ' ') {
            normalized.pop_back();
        }
        return normalized.empty() ? fallback : normalized;
    }

    void QueueSecondaryOledConfig(SecondaryOled::Config config) {
        config.text_scale = std::clamp(config.text_scale, 1, 3);
        config.brand = NormalizeOledText(config.brand, "Xiaozhi");
        config.distance_prefix = NormalizeOledText(config.distance_prefix, "Dist");
        Application::GetInstance().Schedule([this, config = std::move(config)]() {
            if (!secondary_oled_.Configure(config)) {
                return;
            }
            Settings settings("desk_robot", true);
            settings.SetBool("oled_flip", config.flip_180);
            settings.SetBool("oled_brand_on", config.show_brand);
            settings.SetBool("oled_state_on", config.show_state);
            settings.SetBool("oled_dist_on", config.show_distance);
            settings.SetBool("oled_bat_on", config.show_battery);
            settings.SetBool("oled_volt_on", config.show_voltage);
            settings.SetBool("oled_amp_on", config.show_current);
            settings.SetInt("oled_scale", config.text_scale);
            settings.SetString("oled_brand", config.brand);
            settings.SetString("oled_prefix", config.distance_prefix);
        });
    }
#endif

    bool ToggleStatusLight() {
        const int current = status_light_brightness_.load();
        if (current > 0) {
            status_light_saved_brightness_.store(current);
            QueueStatusLightBrightness(0);
            return false;
        }
        const int restored = std::max(1, status_light_saved_brightness_.load());
        QueueStatusLightBrightness(restored);
        return true;
    }

    static std::string NormalizeTemporaryText(const std::string& text, size_t max_length) {
        std::string normalized;
        normalized.reserve(std::min(text.size(), max_length));
        bool previous_space = true;
        for (unsigned char character : text) {
            if (normalized.size() >= max_length) {
                break;
            }
            if (std::isalnum(character) || character == '-') {
                normalized.push_back(static_cast<char>(character));
                previous_space = false;
            } else if (std::isspace(character) && !previous_space) {
                normalized.push_back(' ');
                previous_space = true;
            }
        }
        while (!normalized.empty() && normalized.back() == ' ') {
            normalized.pop_back();
        }
        return normalized;
    }

    bool QueueTemporaryEmotion(const std::string& emotion, int duration_ms) {
        if (!MochanDisplay::IsSupportedEmotion(emotion)) {
            return false;
        }
        const int safe_duration = std::clamp(duration_ms, 250, 30000);
        {
            std::lock_guard<std::mutex> lock(temporary_emotion_mutex_);
            temporary_emotion_ = emotion;
        }
        Application::GetInstance().Schedule(
            [this, emotion]() { display_->SetEmotion(emotion.c_str()); });
        if (face_reset_timer_ != nullptr) {
            esp_timer_stop(face_reset_timer_);
            ESP_ERROR_CHECK(esp_timer_start_once(face_reset_timer_, safe_duration * 1000ULL));
        }
        return true;
    }

    void ResetTemporaryEmotion() {
        std::string expected;
        {
            std::lock_guard<std::mutex> lock(temporary_emotion_mutex_);
            expected.swap(temporary_emotion_);
        }
        Application::GetInstance().Schedule([this, expected = std::move(expected)]() {
            if (expected.empty() || display_->GetCurrentEmotion() != expected) {
                return;
            }
            const DeviceState state = Application::GetInstance().GetDeviceState();
            if (state == kDeviceStateListening) {
                display_->SetEmotion("listening");
            } else if (state == kDeviceStateSpeaking) {
                display_->SetEmotion("speaking");
            } else if (state == kDeviceStateConnecting || state == kDeviceStateActivating) {
                display_->SetEmotion("thinking");
            } else {
                display_->SetEmotion("neutral");
            }
        });
    }

#ifdef SECONDARY_OLED_I2C_ADDRESS
    bool QueueTemporaryOledText(const std::string& text, int duration_ms) {
        const std::string normalized = NormalizeTemporaryText(text, 48);
        if (normalized.empty() || !secondary_oled_.IsAvailable()) {
            return false;
        }
        const int safe_duration = std::clamp(duration_ms, 500, 60000);
        Application::GetInstance().Schedule(
            [this, normalized]() { secondary_oled_.ShowTemporaryText(normalized); });
        if (oled_text_reset_timer_ != nullptr) {
            esp_timer_stop(oled_text_reset_timer_);
            ESP_ERROR_CHECK(esp_timer_start_once(oled_text_reset_timer_, safe_duration * 1000ULL));
        }
        return true;
    }
#endif

    bool QueueStatusLightEffect(const std::string& effect, int duration_ms) {
#if BUILTIN_LED_COUNT == 1
        GpioLed::EffectOverride override = GpioLed::EffectOverride::kNone;
        if (effect == "steady") {
            override = GpioLed::EffectOverride::kSteady;
        } else if (effect == "breathe") {
            override = GpioLed::EffectOverride::kBreathe;
        } else if (effect == "blink") {
            override = GpioLed::EffectOverride::kBlink;
        } else if (effect == "off") {
            override = GpioLed::EffectOverride::kOff;
        } else {
            return false;
        }
        const int safe_duration = std::clamp(duration_ms, 250, 30000);
        Application::GetInstance().Schedule(
            [this, override]() { static_cast<GpioLed*>(GetLed())->SetEffectOverride(override); });
        if (light_effect_reset_timer_ != nullptr) {
            esp_timer_stop(light_effect_reset_timer_);
            ESP_ERROR_CHECK(
                esp_timer_start_once(light_effect_reset_timer_, safe_duration * 1000ULL));
        }
        return true;
#else
        return false;
#endif
    }

    void InitializeInteractionTimers() {
        esp_timer_create_args_t face_args = {
            .callback =
                [](void* arg) { static_cast<DeskRobotBoard*>(arg)->ResetTemporaryEmotion(); },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "face_reset",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&face_args, &face_reset_timer_));
#ifdef SECONDARY_OLED_I2C_ADDRESS
        esp_timer_create_args_t oled_args = {
            .callback =
                [](void* arg) {
                    auto* self = static_cast<DeskRobotBoard*>(arg);
                    Application::GetInstance().Schedule(
                        [self]() { self->secondary_oled_.ClearTemporaryText(); });
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "oled_text_reset",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&oled_args, &oled_text_reset_timer_));
#endif
#if BUILTIN_LED_COUNT == 1
        esp_timer_create_args_t light_args = {
            .callback =
                [](void* arg) {
                    auto* self = static_cast<DeskRobotBoard*>(arg);
                    Application::GetInstance().Schedule([self]() {
                        static_cast<GpioLed*>(self->GetLed())
                            ->SetEffectOverride(GpioLed::EffectOverride::kNone);
                    });
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "light_fx_reset",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&light_args, &light_effect_reset_timer_));
#endif
    }

    void ReturnToIdle() {
        motors_.EmergencyStop();
        live_camera_enabled_.store(false);
        if (live_camera_task_ != nullptr) {
            xTaskNotifyGive(live_camera_task_);
        }
        Application::GetInstance().Schedule([this]() {
            auto& app = Application::GetInstance();
            const DeviceState state = app.GetDeviceState();
            if (state == kDeviceStateSpeaking) {
                app.AbortSpeaking(kAbortReasonNone);
            } else if (state == kDeviceStateListening) {
                app.StopListening();
            } else if (state == kDeviceStateConnecting || state == kDeviceStateNotifying) {
                app.SetDeviceState(kDeviceStateIdle);
            }
            display_->SetEmotion("neutral");
        });
    }

    static void DelayedRebootTask(void*) {
        vTaskDelay(pdMS_TO_TICKS(350));
        Application::GetInstance().Schedule([]() { Application::GetInstance().Reboot(); });
        vTaskDelete(nullptr);
    }

    bool QueueReboot() {
        motors_.EmergencyStop();
        return xTaskCreate(DelayedRebootTask, "web_reboot", 2048, nullptr, 1, nullptr) == pdPASS;
    }

    bool QueueDance() {
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
        if (IsCliffDetected()) {
            return false;
        }
#endif
        constexpr MotorController::Direction kDirections[] = {
            MotorController::Direction::kForward,
            MotorController::Direction::kBackward,
            MotorController::Direction::kLeft,
            MotorController::Direction::kRight,
        };
        constexpr size_t kDirectionCount = sizeof(kDirections) / sizeof(kDirections[0]);
        const size_t step_count = 30 + esp_random() % 21;
        std::vector<MotorController::Movement> movements;
        movements.reserve(step_count);
        size_t previous_direction = kDirectionCount;
        for (size_t step = 0; step < step_count; ++step) {
            size_t direction_index = esp_random() % kDirectionCount;
            if (direction_index == previous_direction) {
                direction_index =
                    (direction_index + 1 + esp_random() % (kDirectionCount - 1)) % kDirectionCount;
            }
            previous_direction = direction_index;
            const auto direction = kDirections[direction_index];
            const bool turning = direction == MotorController::Direction::kLeft ||
                                 direction == MotorController::Direction::kRight;
            const uint32_t duration_ms =
                turning ? 110 + esp_random() % 341 : 180 + esp_random() % 371;
            movements.push_back({direction, duration_ms});
        }
        Application::GetInstance().Schedule([this, movements = std::move(movements)]() {
            if (!motors_.PlaySequence(movements)) {
                ESP_LOGW(TAG, "Random dance sequence was rejected");
            }
        });
        return true;
    }

#ifdef MPU6050_I2C_ADDRESS
    bool QueuePressReaction() {
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
        if (!distance_valid_.load(std::memory_order_relaxed) || IsCliffDetected()) {
            return false;
        }
#endif
        if (press_reaction_pending_.exchange(true, std::memory_order_acq_rel)) {
            return false;
        }

        const std::vector<MotorController::Movement> movements = {
            {MotorController::Direction::kLeft, 90},
            {MotorController::Direction::kRight, 130},
            {MotorController::Direction::kLeft, 130},
            {MotorController::Direction::kRight, 90},
        };
        Application::GetInstance().Schedule([this, movements]() {
            const bool idle = Application::GetInstance().GetDeviceState() == kDeviceStateIdle;
            bool floor_safe = true;
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
            floor_safe = distance_valid_.load(std::memory_order_relaxed) && !IsCliffDetected();
#endif
            if (idle && floor_safe && !motor_activity_active_.load(std::memory_order_relaxed)) {
                QueueTemporaryEmotion("surprised", 1600);
                if (!motors_.PlaySequence(movements)) {
                    ESP_LOGW(TAG, "Pressed reaction motor sequence was rejected");
                }
            }
            press_reaction_pending_.store(false, std::memory_order_release);
        });
        return true;
    }
#endif

    void InitializeAudioSettings() {
        Settings settings("audio", false);
        const int stored_speaker_volume = static_cast<int>(settings.GetInt("output_volume", 70));
        const int stored_microphone_gain = static_cast<int>(settings.GetInt("input_gain", 1));
        const int speaker_volume = std::clamp(stored_speaker_volume, 0, 100);
        const int microphone_gain = std::clamp(stored_microphone_gain, 1, 3);
        speaker_volume_.store(speaker_volume);
        microphone_gain_.store(microphone_gain);
        GetAudioCodec()->SetInputGain(static_cast<float>(microphone_gain));
    }

    void ApplyStatusLightBrightness(int brightness_percent) {
        const int safe_brightness = std::clamp(brightness_percent, 0, 100);
#if BUILTIN_LED_COUNT > 1
        const uint8_t high = static_cast<uint8_t>((safe_brightness * 255) / 100);
        const uint8_t low = high == 0 ? 0 : std::max<uint8_t>(1, high / 8);
        static_cast<CircularStrip*>(GetLed())->SetBrightness(high, low);
#else
        auto* led = static_cast<GpioLed*>(GetLed());
        led->SetBrightnessScale(static_cast<uint8_t>(safe_brightness));
#ifdef BUILTIN_LED_STATUS_PROFILE_EDISON
        if (BUILTIN_LED_STATUS_PROFILE_EDISON) {
            led->SetStatusProfile(GpioLed::StatusProfile::kEdison);
        }
#endif
#endif
    }

    void InitializeLightingSettings() {
        Settings settings("desk_robot", false);
        const int brightness = std::clamp(
            static_cast<int>(settings.GetInt("led_brightness", STATUS_LIGHT_DEFAULT_BRIGHTNESS)), 0,
            100);
        status_light_brightness_.store(brightness);
        if (brightness > 0) {
            status_light_saved_brightness_.store(brightness);
        }
        ApplyStatusLightBrightness(brightness);
    }

    void InitializeMotorStatusLight() {
        motors_.SetMovementStateCallback([this](bool moving) {
            motor_activity_active_.store(moving, std::memory_order_relaxed);
#if BUILTIN_LED_COUNT == 1 && defined(BUILTIN_LED_STATUS_PROFILE_EDISON)
            Application::GetInstance().Schedule(
                [this, moving]() { static_cast<GpioLed*>(GetLed())->SetActivityOverride(moving); });
#endif
        });
    }

    void QueueSpeakerVolume(int volume) {
        const int safe_volume = std::clamp(volume, 0, 100);
        speaker_volume_.store(safe_volume);
        Application::GetInstance().Schedule(
            [this, safe_volume]() { GetAudioCodec()->SetOutputVolume(safe_volume); });
    }

    void QueueMicrophoneGain(int gain) {
        const int safe_gain = std::clamp(gain, 1, 3);
        microphone_gain_.store(safe_gain);
        Application::GetInstance().Schedule([this, safe_gain]() {
            GetAudioCodec()->SetInputGain(static_cast<float>(safe_gain));
            Settings settings("audio", true);
            settings.SetInt("input_gain", safe_gain);
        });
    }

    void QueueScreenBrightness(int brightness) {
        const int safe_brightness = std::clamp(brightness, 10, 100);
        Application::GetInstance().Schedule([this, safe_brightness]() {
            if (GetBacklight() != nullptr) {
                GetBacklight()->SetBrightness(static_cast<uint8_t>(safe_brightness), true);
            }
        });
    }

    void QueueStatusLightBrightness(int brightness) {
        const int safe_brightness = std::clamp(brightness, 0, 100);
        status_light_brightness_.store(safe_brightness);
        if (safe_brightness > 0) {
            status_light_saved_brightness_.store(safe_brightness);
        }
        Application::GetInstance().Schedule([this, safe_brightness]() {
            ApplyStatusLightBrightness(safe_brightness);
            Settings settings("desk_robot", true);
            settings.SetInt("led_brightness", safe_brightness);
        });
    }

    static void LiveCameraTask(void* arg) {
        static_cast<DeskRobotBoard*>(arg)->RunLiveCameraTask();
    }

    void RunLiveCameraTask() {
        bool preview_visible = false;
        while (true) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            while (live_camera_enabled_.load()) {
                if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                    if (camera_->CapturePreview()) {
                        preview_visible = true;
                    }
                    vTaskDelay(pdMS_TO_TICKS(250));
                } else {
                    // Live preview is intentionally one-shot per idle session.
                    // Starting a conversation turns the mode off; returning to
                    // idle requires an explicit toggle from the local UI.
                    live_camera_enabled_.store(false);
                    if (preview_visible) {
                        display_->SetPreviewImage(nullptr);
                        preview_visible = false;
                    }
                }
            }
            if (preview_visible) {
                display_->SetPreviewImage(nullptr);
                preview_visible = false;
            }
        }
    }

    void InitializeLiveCamera() {
        BaseType_t result =
            xTaskCreate(LiveCameraTask, "live_camera", 6144, this, 1, &live_camera_task_);
        if (result != pdPASS) {
            live_camera_task_ = nullptr;
            ESP_LOGE(TAG, "Failed to create live camera task");
        }
    }

    bool ToggleLiveCamera() {
        if (live_camera_task_ == nullptr) {
            return false;
        }
        const bool enabled = !live_camera_enabled_.load();
        live_camera_enabled_.store(enabled);
        xTaskNotifyGive(live_camera_task_);
        return enabled;
    }

    bool HandleWebAction(const std::string& action, int duration_ms, const std::string& text,
                         std::string& message) {
        MotorController::Direction direction;
        if (ParseDirection(action, direction)) {
            const int safe_duration = std::clamp(duration_ms, 50, 2000);
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
            if (IsDirectionBlockedByCliff(direction)) {
                message = "Movement blocked: table edge detected; reverse remains available";
                return false;
            }
#endif
            Application::GetInstance().Schedule([this, direction, safe_duration]() {
                motors_.Stop();
                motors_.Drive(direction, static_cast<uint32_t>(safe_duration));
            });
            message = "Moving " + action;
            return true;
        }
        if (action == "stop") {
            Application::GetInstance().Schedule([this]() { motors_.Stop(); });
            message = "Motors stopped";
            return true;
        }
        if (action == "dance") {
            const bool started = QueueDance();
            message = started ? "Random dance started" : "Dance blocked: table edge detected";
            return started;
        }
        if (action == "wake") {
            Application::GetInstance().ToggleChatState();
            message = "Wake toggled";
            return true;
        }
        if (action == "camera_flip") {
            message = QueueCameraFlip() ? "Camera flipped" : "Camera restored";
            return true;
        }
        if (action == "display_flip") {
            message = QueueDisplayFlip() ? "Main display flipped" : "Main display restored";
            return true;
        }
#ifdef SECONDARY_OLED_I2C_ADDRESS
        if (action == "oled_flip" || action == "oled_show_brand" || action == "oled_show_state" ||
            action == "oled_show_distance" || action == "oled_show_battery" ||
            action == "oled_show_voltage" || action == "oled_show_current" ||
            action == "oled_scale" || action == "oled_brand" || action == "oled_prefix") {
            SecondaryOled::Config config = secondary_oled_.GetConfig();
            if (action == "oled_flip") {
                config.flip_180 = !config.flip_180;
            } else if (action == "oled_show_brand") {
                config.show_brand = duration_ms != 0;
            } else if (action == "oled_show_state") {
                config.show_state = duration_ms != 0;
            } else if (action == "oled_show_distance") {
                config.show_distance = duration_ms != 0;
            } else if (action == "oled_show_battery") {
                config.show_battery = duration_ms != 0;
            } else if (action == "oled_show_voltage") {
                config.show_voltage = duration_ms != 0;
            } else if (action == "oled_show_current") {
                config.show_current = duration_ms != 0;
            } else if (action == "oled_scale") {
                config.text_scale = std::clamp(duration_ms, 1, 3);
            } else if (action == "oled_brand") {
                config.brand = NormalizeOledText(text, "Xiaozhi");
            } else {
                config.distance_prefix = NormalizeOledText(text, "Dist");
            }
            QueueSecondaryOledConfig(config);
            message = "OLED settings updated";
            return true;
        }
#endif
        if (action == "lights_toggle") {
            message = ToggleStatusLight() ? "Status lights enabled" : "Status lights disabled";
            return true;
        }
        if (action == "emotion") {
            if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
                message = "Manual emotions are available only while Idle";
                return false;
            }
            const bool accepted = QueueTemporaryEmotion(text, duration_ms > 0 ? duration_ms : 5000);
            message = accepted ? "Emotion: " + text : "Unsupported emotion";
            return accepted;
        }
        if (action == "audio_test") {
            if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
                message = "Audio test is available only while Idle";
                return false;
            }
            Application::GetInstance().Schedule(
                []() { Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP); });
            message = "Playing speaker test";
            return true;
        }
        if (action == "return_idle") {
            ReturnToIdle();
            message = "Returning robot to idle";
            return true;
        }
        if (action == "reboot") {
            const bool queued = QueueReboot();
            message = queued ? "Robot is rebooting" : "Could not schedule reboot";
            return queued;
        }
        if (action == "speaker_volume") {
            const int safe_volume = std::clamp(duration_ms, 0, 100);
            QueueSpeakerVolume(safe_volume);
            message = "Speaker volume " + std::to_string(safe_volume) + "%";
            return true;
        }
        if (action == "microphone_gain") {
            const int safe_gain = std::clamp(duration_ms, 1, 3);
            QueueMicrophoneGain(safe_gain);
            message = "Microphone gain " + std::to_string(safe_gain) + "x";
            return true;
        }
        if (action == "screen_brightness") {
            const int safe_brightness = std::clamp(duration_ms, 10, 100);
            QueueScreenBrightness(safe_brightness);
            message = "Screen brightness " + std::to_string(safe_brightness) + "%";
            return true;
        }
        if (action == "motor_speed") {
            motors_.SetSpeedPercent(duration_ms);
            message = "Motor speed " + std::to_string(motors_.GetSpeedPercent()) + "%";
            return true;
        }
        if (action == "status_light_brightness") {
            const int safe_brightness = std::clamp(duration_ms, 0, 100);
            QueueStatusLightBrightness(safe_brightness);
            message = "Status light brightness " + std::to_string(safe_brightness) + "%";
            return true;
        }
#ifdef INA219_I2C_ADDRESS
        if (action == "battery_capacity_start") {
            if (!power_monitor_.IsAvailable()) {
                message = "INA219 is unavailable";
                return false;
            }
            battery_capacity_test_active_.store(true);
            PersistBatteryCapacityTest();
            message = "Battery capacity measurement started";
            return true;
        }
        if (action == "battery_capacity_stop") {
            battery_capacity_test_active_.store(false);
            battery_capacity_test_measuring_.store(false);
            PersistBatteryCapacityTest();
            message = "Battery capacity measurement stopped";
            return true;
        }
        if (action == "battery_capacity_reset") {
            battery_capacity_test_active_.store(false);
            battery_capacity_test_measuring_.store(false);
            battery_capacity_test_uah_.store(0);
            battery_capacity_test_seconds_.store(0);
            PersistBatteryCapacityTest();
            message = "Battery capacity measurement reset";
            return true;
        }
#endif
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
        if (action == "cliff_threshold") {
            const int safe_edge_mm = std::clamp(duration_ms, 50, 500);
            QueueCliffThreshold(safe_edge_mm);
            message = "Cliff threshold " + std::to_string(safe_edge_mm) + " mm";
            return true;
        }
#endif
#ifdef MPU6050_I2C_ADDRESS
        if (action == "motion_emotions") {
            const bool enabled = duration_ms != 0;
            motion_emotions_enabled_.store(enabled);
            Application::GetInstance().Schedule([enabled]() {
                Settings settings("desk_robot", true);
                settings.SetBool("motion_emotions", enabled);
            });
            message = enabled ? "Motion emotions enabled" : "Motion emotions disabled";
            return true;
        }
#endif
        if (action == "live_camera") {
            const bool enabled = ToggleLiveCamera();
            message = enabled ? "Live preview enabled" : "Live preview disabled";
            return live_camera_task_ != nullptr;
        }
        if (action == "wifi_config") {
            EnterWifiConfigMode();
            message = "Entering Wi-Fi setup";
            return true;
        }
        message = "Unknown action";
        return false;
    }

    void InitializeWebControl() {
        RobotWebControlServer::SnapshotHandler snapshot_handler;
#ifdef DESK_ROBOT_USE_ESP32_CAMERA
        snapshot_handler = [this](const RobotWebControlServer::SnapshotSender& sender) {
            return Application::GetInstance().GetDeviceState() == kDeviceStateIdle &&
                   camera_ != nullptr && camera_->SendWebSnapshot(sender);
        };
#endif
        web_control_server_ = std::make_unique<RobotWebControlServer>(
            [this](const std::string& action, int duration_ms, const std::string& text,
                   std::string& message) {
                return HandleWebAction(action, duration_ms, text, message);
            },
            [this]() {
                const char* state =
                    DeviceStateMachine::GetStateName(Application::GetInstance().GetDeviceState());
                cJSON* root = cJSON_CreateObject();
                if (root == nullptr) {
                    return std::string(R"({"state":"unknown","error":"out of memory"})");
                }
                cJSON_AddStringToObject(root, "state", state != nullptr ? state : "unknown");
                cJSON_AddBoolToObject(root, "camera_available",
                                      camera_ != nullptr && camera_->IsAvailable());
                cJSON_AddBoolToObject(root, "camera_flipped", camera_flipped_.load());
                cJSON_AddBoolToObject(root, "display_flipped", display_flipped_.load());
                cJSON_AddStringToObject(root, "emotion", display_->GetCurrentEmotion().c_str());
                cJSON_AddNumberToObject(root, "speaker_volume", speaker_volume_.load());
                cJSON_AddNumberToObject(root, "microphone_gain", microphone_gain_.load());
                auto& audio_service = Application::GetInstance().GetAudioService();
                cJSON_AddNumberToObject(root, "microphone_level", audio_service.GetInputLevel());
                cJSON_AddBoolToObject(root, "microphone_clipping", audio_service.IsInputClipping());
                cJSON_AddNumberToObject(
                    root, "screen_brightness",
                    GetBacklight() != nullptr ? GetBacklight()->brightness() : 0);
                cJSON_AddNumberToObject(root, "status_light_brightness",
                                        status_light_brightness_.load());
                cJSON_AddBoolToObject(root, "live_camera", live_camera_enabled_.load());
                cJSON_AddNumberToObject(root, "motor_speed", motors_.GetSpeedPercent());
                cJSON* motor_status = cJSON_Parse(motors_.StatusJson().c_str());
                cJSON_AddItemToObject(
                    root, "motors", motor_status != nullptr ? motor_status : cJSON_CreateObject());
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
                cJSON_AddNumberToObject(root, "distance_mm", distance_mm_.load());
                cJSON_AddBoolToObject(root, "distance_valid", distance_valid_.load());
                cJSON_AddBoolToObject(root, "cliff_detected", IsCliffDetected());
                cJSON_AddNumberToObject(root, "cliff_edge_mm", cliff_edge_mm_.load());
#endif
#ifdef INA219_I2C_ADDRESS
                cJSON_AddBoolToObject(root, "battery_available", power_monitor_.IsAvailable());
                cJSON_AddBoolToObject(root, "battery_valid", battery_valid_.load());
                cJSON_AddNumberToObject(root, "battery_percent", battery_percent_.load());
                cJSON_AddNumberToObject(root, "battery_voltage_v", battery_voltage_v_.load());
                cJSON_AddNumberToObject(root, "battery_current_ma", battery_current_ma_.load());
                cJSON_AddNumberToObject(root, "battery_power_mw", battery_power_mw_.load());
                cJSON_AddBoolToObject(root, "battery_charging", battery_charging_.load());
                cJSON_AddBoolToObject(root, "battery_discharging", battery_discharging_.load());
                cJSON_AddBoolToObject(root, "battery_capacity_test_active",
                                      battery_capacity_test_active_.load());
                cJSON_AddBoolToObject(root, "battery_capacity_test_measuring",
                                      battery_capacity_test_measuring_.load());
                cJSON_AddNumberToObject(root, "battery_capacity_test_mah",
                                        battery_capacity_test_uah_.load() / 1000.0);
                cJSON_AddNumberToObject(root, "battery_capacity_test_seconds",
                                        battery_capacity_test_seconds_.load());
#endif
#ifdef MPU6050_I2C_ADDRESS
                cJSON_AddBoolToObject(root, "motion_sensor_available",
                                      motion_sensor_.IsAvailable());
                cJSON_AddBoolToObject(root, "motion_sensor_valid", motion_sensor_valid_.load());
                cJSON_AddBoolToObject(root, "motion_emotions_enabled",
                                      motion_emotions_enabled_.load());
                cJSON_AddNumberToObject(root, "motion_roll_deg", motion_roll_deg_.load());
                cJSON_AddNumberToObject(root, "motion_pitch_deg", motion_pitch_deg_.load());
                cJSON_AddNumberToObject(root, "motion_acceleration_g",
                                        motion_acceleration_g_.load());
                cJSON_AddNumberToObject(root, "motion_rotation_dps", motion_rotation_dps_.load());
                cJSON_AddStringToObject(root, "motion_gesture",
                                        MotionGestureName(motion_gesture_.load()));
#endif
#ifdef SECONDARY_OLED_I2C_ADDRESS
                cJSON_AddBoolToObject(root, "oled_available", secondary_oled_.IsAvailable());
                const SecondaryOled::Config oled_config = secondary_oled_.GetConfig();
                cJSON_AddBoolToObject(root, "oled_flipped", oled_config.flip_180);
                cJSON_AddBoolToObject(root, "oled_show_brand", oled_config.show_brand);
                cJSON_AddBoolToObject(root, "oled_show_state", oled_config.show_state);
                cJSON_AddBoolToObject(root, "oled_show_distance", oled_config.show_distance);
                cJSON_AddBoolToObject(root, "oled_show_battery", oled_config.show_battery);
                cJSON_AddBoolToObject(root, "oled_show_voltage", oled_config.show_voltage);
                cJSON_AddBoolToObject(root, "oled_show_current", oled_config.show_current);
                cJSON_AddNumberToObject(root, "oled_text_scale", oled_config.text_scale);
                cJSON_AddStringToObject(root, "oled_brand", oled_config.brand.c_str());
                cJSON_AddStringToObject(root, "oled_distance_prefix",
                                        oled_config.distance_prefix.c_str());
#endif
                const esp_app_desc_t* app = esp_app_get_description();
                cJSON_AddStringToObject(root, "version", app != nullptr ? app->version : "unknown");
                cJSON_AddStringToObject(root, "ip",
                                        WifiManager::GetInstance().GetIpAddress().c_str());
                wifi_ap_record_t access_point = {};
                if (esp_wifi_sta_get_ap_info(&access_point) == ESP_OK) {
                    const size_t ssid_length =
                        strnlen(reinterpret_cast<const char*>(access_point.ssid),
                                sizeof(access_point.ssid));
                    cJSON_AddStringToObject(
                        root, "ssid",
                        std::string(reinterpret_cast<const char*>(access_point.ssid), ssid_length)
                            .c_str());
                    cJSON_AddNumberToObject(root, "rssi", access_point.rssi);
                } else {
                    cJSON_AddStringToObject(root, "ssid", "—");
                    cJSON_AddNumberToObject(root, "rssi", 0);
                }
                cJSON_AddNumberToObject(root, "uptime_sec", esp_timer_get_time() / 1000000);
                cJSON_AddNumberToObject(root, "free_internal_bytes",
                                        heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                cJSON_AddNumberToObject(root, "free_psram_bytes",
                                        heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

                char* encoded = cJSON_PrintUnformatted(root);
                const std::string result = encoded != nullptr ? encoded : R"({"state":"unknown"})";
                cJSON_free(encoded);
                cJSON_Delete(root);
                return result;
            },
            std::move(snapshot_handler));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        boot_button_.OnLongPress(
            [this]() { Application::GetInstance().Schedule([this]() { EnterWifiConfigMode(); }); });
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool(
            "self.robot.drive",
            "Drive the two-wheel base. The movement always stops after duration_ms.",
            PropertyList({
                Property("direction", kPropertyTypeString),
                Property("duration_ms", kPropertyTypeInteger, 250, 50, 2000),
            }),
            [this](const PropertyList& properties) -> ToolResult {
                const auto direction = properties["direction"].value<std::string>();
                MotorController::Direction command;
                if (!ParseDirection(direction, command)) {
                    return std::string("direction must be forward, backward, left, or right");
                }
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
                if (IsDirectionBlockedByCliff(command)) {
                    return std::string(
                        "Movement blocked: table edge detected; reverse remains available");
                }
#endif
                const int duration_ms = properties["duration_ms"].value<int>();
                Application::GetInstance().Schedule([this, command, duration_ms]() {
                    motors_.Drive(command, static_cast<uint32_t>(duration_ms));
                });
                return true;
            });
        mcp_server.AddTool("self.robot.stop", "Stop both drive motors immediately.", PropertyList(),
                           [this](const PropertyList&) -> ReturnValue {
                               Application::GetInstance().Schedule([this]() { motors_.Stop(); });
                               return true;
                           });
        mcp_server.AddTool(
            "self.robot.get_status", "Get the drive motor state.", PropertyList(),
            [this](const PropertyList&) -> ReturnValue { return motors_.StatusJson(); });
        mcp_server.AddTool("self.robot.dance", "Run a bounded randomized dance movement.",
                           PropertyList(),
                           [this](const PropertyList&) -> ReturnValue { return QueueDance(); });
        mcp_server.AddTool(
            "self.face.set_emotion",
            "Temporarily show a face emotion, then return to the current assistant state. "
            "Supported emotions: neutral, happy, laughing, funny, sad, angry, crying, loving, "
            "embarrassed, surprised, shocked, thinking, winking, cool, relaxed, delicious, "
            "kissy, confident, sleepy, silly, confused, suspicious, and shake.",
            PropertyList({
                Property("emotion", kPropertyTypeString, "neutral"),
                Property("duration_ms", kPropertyTypeInteger, 5000, 250, 30000),
            }),
            [this](const PropertyList& properties) -> ToolResult {
                const std::string emotion = properties["emotion"].value<std::string>();
                if (!QueueTemporaryEmotion(emotion, properties["duration_ms"].value<int>())) {
                    return std::string("Unsupported face emotion");
                }
                return true;
            });
        mcp_server.AddTool(
            "self.face.look",
            "Temporarily move the eyes, then return to center. Supported directions: center, "
            "left, right, up, down, up_left, up_right, down_left, and down_right.",
            PropertyList({
                Property("direction", kPropertyTypeString, "center"),
                Property("duration_ms", kPropertyTypeInteger, 2500, 250, 15000),
            }),
            [this](const PropertyList& properties) -> ToolResult {
                std::string direction = properties["direction"].value<std::string>();
                if (direction == "center") {
                    direction = "neutral";
                }
                if (!QueueTemporaryEmotion(direction, properties["duration_ms"].value<int>())) {
                    return std::string("Unsupported look direction");
                }
                return true;
            });
#ifdef SECONDARY_OLED_I2C_ADDRESS
        mcp_server.AddTool(
            "self.secondary_display.show_text",
            "Temporarily show a short ASCII message on the secondary OLED. The automatic brand, "
            "assistant state, and distance marquee returns afterward.",
            PropertyList({
                Property("text", kPropertyTypeString),
                Property("duration_ms", kPropertyTypeInteger, 5000, 500, 60000),
            }),
            [this](const PropertyList& properties) -> ToolResult {
                if (!QueueTemporaryOledText(properties["text"].value<std::string>(),
                                            properties["duration_ms"].value<int>())) {
                    return std::string("OLED is unavailable or text is empty");
                }
                return true;
            });
#endif
        mcp_server.AddTool(
            "self.status_light.set_effect",
            "Temporarily control the monochrome Edison status lights. Supported effects: steady, "
            "breathe, blink, and off. Motor movement still has priority, and normal status "
            "behavior "
            "returns afterward.",
            PropertyList({
                Property("effect", kPropertyTypeString, "steady"),
                Property("duration_ms", kPropertyTypeInteger, 5000, 250, 30000),
            }),
            [this](const PropertyList& properties) -> ToolResult {
                if (!QueueStatusLightEffect(properties["effect"].value<std::string>(),
                                            properties["duration_ms"].value<int>())) {
                    return std::string("Unsupported status-light effect");
                }
                return true;
            });
        mcp_server.AddTool("self.camera.set_camera_flipped",
                           "Rotate the camera image by 180 degrees.", PropertyList(),
                           [this](const PropertyList&) -> ReturnValue {
                               QueueCameraFlip();
                               return true;
                           });
        mcp_server.AddTool("self.audio_microphone.set_gain",
                           "Set the microphone software gain. Use 1 for normal, 2 for louder, "
                           "or 3 for maximum.",
                           PropertyList({Property("gain", kPropertyTypeInteger, 1, 1, 3)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               QueueMicrophoneGain(properties["gain"].value<int>());
                               return true;
                           });
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
        mcp_server.AddTool(
            "self.distance.get",
            "Get the downward VL53L0X floor distance and cliff-detection state.", PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                if (distance_sensor_ == nullptr) {
                    return std::string(R"({"available":false})");
                }
                return std::string("{\"available\":true,\"valid\":") +
                       (distance_valid_.load() ? "true" : "false") +
                       ",\"distance_mm\":" + std::to_string(distance_mm_.load()) +
                       ",\"cliff_detected\":" + (cliff_detected_.load() ? "true" : "false") +
                       ",\"edge_mm\":" + std::to_string(cliff_edge_mm_.load()) + "}";
            });
#endif
#ifdef INA219_I2C_ADDRESS
        mcp_server.AddTool(
            "self.battery.get_status",
            "Get INA219 battery voltage, estimated charge percentage, current, and power.",
            PropertyList(), [this](const PropertyList&) -> ToolResult {
                cJSON* result = cJSON_CreateObject();
                if (result == nullptr) {
                    return std::unexpected("Out of memory");
                }
                cJSON_AddBoolToObject(result, "available", power_monitor_.IsAvailable());
                cJSON_AddBoolToObject(result, "valid", battery_valid_.load());
                if (battery_valid_.load()) {
                    cJSON_AddNumberToObject(result, "percent", battery_percent_.load());
                    cJSON_AddNumberToObject(result, "voltage_v", battery_voltage_v_.load());
                    cJSON_AddNumberToObject(result, "current_ma", battery_current_ma_.load());
                    cJSON_AddNumberToObject(result, "power_mw", battery_power_mw_.load());
                    cJSON_AddBoolToObject(result, "charging", battery_charging_.load());
                    cJSON_AddBoolToObject(result, "discharging", battery_discharging_.load());
                }
                return result;
            });
#endif
    }

public:
    DeskRobotBoard() : boot_button_(BOOT_BUTTON_GPIO, BUTTON_ACTIVE_HIGH, 3000) {
        // The web dashboard is not reachable until Wi-Fi comes up, so begin
        // buffering here to retain display, camera, and audio initialization logs.
        RobotWebControlServer::BeginLogCapture();
        InitializeSpi();
        InitializeDisplay();
        // Bring up the panel backlight before camera/audio initialization. A
        // peripheral failure later in boot must not leave the display looking
        // completely unpowered and hide the splash or diagnostic state.
        if (GetBacklight() != nullptr) {
            GetBacklight()->RestoreBrightness();
        }
        InitializeButtons();
#ifdef AUXILIARY_I2C_SDA_PIN
        InitializeAuxiliaryI2c();
#endif
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
        InitializeCameraI2c();
        InitializeCliffSettings();
#endif
        InitializeCamera();
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
        InitializeDistanceSensor();
        motors_.SetMotionGuard([this](MotorController::Direction direction) {
            return !IsDirectionBlockedByCliff(direction);
        });
#endif
#ifdef SECONDARY_OLED_I2C_ADDRESS
        InitializeSecondaryOled();
#endif
#ifdef INA219_I2C_ADDRESS
        InitializePowerMonitor();
#endif
#ifdef MPU6050_I2C_ADDRESS
        InitializeMotionSensor();
#endif
        InitializeAudioSettings();
        InitializeLightingSettings();
        InitializeMotorStatusLight();
        InitializeLiveCamera();
        InitializeInteractionTimers();
#if defined(INA219_I2C_ADDRESS) || defined(MPU6050_I2C_ADDRESS)
        StartAuxiliarySensorTask();
#endif
        InitializeTools();
#ifdef MPU6050_I2C_ADDRESS
        RegisterMotionTools();
#endif
        InitializeWebControl();
        ESP_LOGI(TAG, "Desk robot board initialized");
    }

    AudioCodec* GetAudioCodec() override {
#if AUDIO_MIC_IS_PDM
        static NoAudioCodecSimplexPdm audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                                  AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK,
                                                  AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK,
                                                  AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                               AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK,
                                               AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK,
                                               AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#endif
        return &audio_codec;
    }

    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
#ifdef INA219_I2C_ADDRESS
        if (!battery_valid_.load()) {
            return false;
        }
        level = battery_percent_.load();
        charging = battery_charging_.load();
        discharging = battery_discharging_.load();
        return true;
#else
        return false;
#endif
    }

    Led* GetLed() override {
#if BUILTIN_LED_COUNT > 1
        static CircularStrip led(BUILTIN_LED_GPIO, BUILTIN_LED_COUNT);
#else
#if defined(BUILTIN_LED_LEDC_TIMER) && defined(BUILTIN_LED_LEDC_CHANNEL)
        static GpioLed led(BUILTIN_LED_GPIO, BUILTIN_LED_OUTPUT_INVERT, BUILTIN_LED_LEDC_TIMER,
                           BUILTIN_LED_LEDC_CHANNEL);
#else
        static GpioLed led(BUILTIN_LED_GPIO, true);
#endif
#endif
        return &led;
    }

    Backlight* GetBacklight() override {
#if HAS_DISPLAY_BACKLIGHT
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
#else
        return nullptr;
#endif
    }

    Display* GetDisplay() override { return display_; }
    Camera* GetCamera() override { return camera_; }
};

DECLARE_BOARD(DeskRobotBoard);
