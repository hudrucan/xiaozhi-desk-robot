#include "wifi_board.h"

#include "application.h"
#include "assets/lang_config.h"
#include "button.h"
#include "codecs/no_audio_codec.h"
#include "config.h"
#include "display/lcd_display.h"
#include "esp32_camera.h"
#ifdef INA219_I2C_ADDRESS
#include "battery_soc_estimator.h"
#include "ina219_power_monitor.h"
#endif
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
#include <array>
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

#ifndef MPU6050_YAW_AXIS
#define MPU6050_YAW_AXIS 2
#endif

#ifndef MPU6050_YAW_SIGN
#define MPU6050_YAW_SIGN 1.0f
#endif

using DeskRobotCameraBase = Esp32Camera;
using DeskRobotCameraConfig = camera_config_t;

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

    bool IsAvailable() const { return DeskRobotCameraBase::IsAvailable(); }

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
    enum class EmotionSource : uint8_t { kAssistant, kPreview, kMpuReaction };

    static constexpr int kDefaultMotorSpeedPercent = 70;
    static constexpr int kDefaultDriveDurationMs = 250;
    static constexpr int64_t kEmotionMovementCooldownUs = 1500 * 1000LL;
#ifdef MPU6050_I2C_ADDRESS
    static constexpr int kGyroBiasSamples = 50;
    static constexpr float kGyroBiasYawStabilityDps = 1.5f;
    static constexpr int64_t kGyroSampleStaleUs = 160 * 1000LL;
    static constexpr float kGyroTurnToleranceDeg = 2.5f;
    static constexpr uint32_t kGyroTurnMaxTimeoutMs = 10000;
#endif

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
    std::atomic_int drive_duration_ms_{kDefaultDriveDurationMs};
    std::atomic_bool emotion_movement_enabled_{false};
    std::atomic_bool emotion_movement_active_{false};
    int64_t last_emotion_movement_us_ = 0;
    std::array<uint8_t, 8> last_emotion_variants_{};
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
    TaskHandle_t auxiliary_init_task_ = nullptr;
#endif
#ifdef INA219_I2C_ADDRESS
    Ina219PowerMonitor power_monitor_;
    BatterySocEstimator battery_soc_estimator_{BatterySocEstimator::Config {
        .usable_capacity_mah = BATTERY_SOC_USABLE_CAPACITY_MAH,
        .maximum_integration_gap_us = BATTERY_SOC_MAX_INTEGRATION_GAP_MS * 1000LL,
        .quasi_rest_max_current_ma = BATTERY_SOC_QUASI_REST_MAX_CURRENT_MA,
        .quasi_rest_current_stddev_ma = BATTERY_SOC_QUASI_REST_CURRENT_STDDEV_MA,
        .quasi_rest_voltage_stddev_v = BATTERY_SOC_QUASI_REST_VOLTAGE_STDDEV_MV / 1000.0f,
        .quasi_rest_current_transition_ma = BATTERY_SOC_QUASI_REST_CURRENT_TRANSITION_MA,
        .quasi_rest_voltage_transition_v = BATTERY_SOC_QUASI_REST_VOLTAGE_TRANSITION_MV / 1000.0f,
        .quasi_rest_qualification_us = BATTERY_SOC_QUASI_REST_QUALIFICATION_MS * 1000LL,
        .quasi_rest_correction_interval_us = BATTERY_SOC_QUASI_REST_CORRECTION_INTERVAL_MS * 1000LL,
        .quasi_rest_correction_time_constant_us =
            BATTERY_SOC_QUASI_REST_CORRECTION_TIME_CONSTANT_MS * 1000LL,
        .full_anchor_min_voltage_v = BATTERY_SOC_FULL_ANCHOR_MIN_VOLTAGE_V,
        .full_anchor_taper_current_ma = BATTERY_SOC_FULL_ANCHOR_TAPER_CURRENT_MA,
        .full_anchor_qualification_us = BATTERY_SOC_FULL_ANCHOR_QUALIFICATION_MS * 1000LL,
        .bootstrap_full_anchor_min_voltage_soc_percent =
            BATTERY_SOC_BOOTSTRAP_FULL_ANCHOR_MIN_VOLTAGE_SOC_PERCENT,
        .bootstrap_voltage_rebase_min_delta_percent =
            BATTERY_SOC_BOOTSTRAP_VOLTAGE_REBASE_MIN_DELTA_PERCENT,
        .empty_anchor_max_voltage_v = BATTERY_SOC_EMPTY_ANCHOR_MAX_VOLTAGE_V,
        .empty_anchor_qualification_us = BATTERY_SOC_EMPTY_ANCHOR_QUALIFICATION_MS * 1000LL,
    }};
    std::atomic_bool battery_valid_{false};
    std::atomic_int battery_percent_{-1};
    std::atomic<float> battery_voltage_v_{0.0f};
    std::atomic<float> battery_current_ma_{0.0f};
    std::atomic<float> battery_power_mw_{0.0f};
    std::atomic<float> battery_signed_current_ma_{0.0f};
    std::atomic<float> battery_shunt_voltage_mv_{0.0f};
    std::atomic<float> battery_bus_voltage_v_{0.0f};
    std::atomic<float> battery_remaining_mah_{0.0f};
    std::atomic_bool battery_conversion_ready_{false};
    std::atomic_bool battery_math_overflow_{false};
    std::atomic_bool battery_soc_tracking_degraded_{false};
    std::atomic_bool battery_soc_quasi_resting_{false};
    std::atomic<float> battery_soc_voltage_reference_percent_{0.0f};
    std::atomic<float> battery_soc_voltage_correction_mah_{0.0f};
    std::atomic_bool battery_soc_full_anchored_{false};
    std::atomic_bool battery_soc_bootstrap_voltage_rebased_{false};
    std::atomic_bool battery_soc_empty_anchored_{false};
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
    enum class GyroTurnStopReason : uint8_t {
        kNone,
        kActive,
        kTargetReached,
        kTimeout,
        kStaleSensor,
        kDirectionMismatch,
        kCancelled,
        kRejected,
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
    std::atomic<float> motion_yaw_rate_dps_{0.0f};
    std::atomic<float> motion_yaw_bias_dps_{0.0f};
    std::atomic<int64_t> motion_sample_timestamp_us_{0};
    std::atomic_bool motion_gyro_bias_valid_{false};
    std::atomic_bool gyro_turn_pending_{false};
    std::atomic_bool gyro_turn_active_{false};
    std::atomic<float> gyro_turn_target_deg_{0.0f};
    std::atomic<float> gyro_turn_progress_deg_{0.0f};
    std::atomic<GyroTurnStopReason> gyro_turn_stop_reason_{GyroTurnStopReason::kNone};
    std::atomic<uint8_t> gyro_turn_intensity_percent_{80};
    std::atomic<uint32_t> gyro_turn_timeout_ms_{0};
    TaskHandle_t gyro_turn_task_ = nullptr;
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
    std::atomic<SecondaryOled::NetworkState> secondary_oled_network_state_{
        SecondaryOled::NetworkState::kConnecting};
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

    static void DeferredAuxiliaryInitTask(void* arg) {
        auto* self = static_cast<DeskRobotBoard*>(arg);

        // This task is queued from Application::Run(), after Application::Initialize() returns.
        // A short delay also lets the main display and audio DMA settle before another driver is
        // added.
        vTaskDelay(pdMS_TO_TICKS(250));
        ESP_LOGI(TAG, "Deferred auxiliary I2C initialization starting");

        self->InitializeAuxiliaryI2c();
        if (self->auxiliary_i2c_bus_ != nullptr) {
            // Do not start any periodic I2C task until every device has completed its one-time
            // setup. This guarantees that SSD1306 panel creation cannot overlap a sensor read.
#ifdef INA219_I2C_ADDRESS
            self->InitializePowerMonitor();
            vTaskDelay(pdMS_TO_TICKS(50));
#endif
#ifdef MPU6050_I2C_ADDRESS
            self->InitializeMotionSensor();
            vTaskDelay(pdMS_TO_TICKS(50));
#endif
#ifdef SECONDARY_OLED_I2C_ADDRESS
            self->InitializeSecondaryOled();
#endif
#if defined(INA219_I2C_ADDRESS) || defined(MPU6050_I2C_ADDRESS)
            self->StartAuxiliarySensorTask();
#endif
        }

        ESP_LOGI(TAG, "Deferred auxiliary I2C initialization complete");
        self->auxiliary_init_task_ = nullptr;
        vTaskDelete(nullptr);
    }

    void StartDeferredAuxiliaryInit() {
        if (auxiliary_init_task_ != nullptr) {
            return;
        }
        if (xTaskCreate(DeferredAuxiliaryInitTask, "aux_i2c_init", 10240, this, 1,
                        &auxiliary_init_task_) != pdPASS) {
            auxiliary_init_task_ = nullptr;
            ESP_LOGE(TAG, "Failed to create deferred auxiliary I2C initialization task");
        }
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
        BatterySocEstimator::PersistedState soc_state;
        soc_state.version = settings.GetInt("soc_ver", 0);
        soc_state.remaining_uah = settings.GetInt("soc_rem_uah", -1);
        soc_state.usable_capacity_uah = settings.GetInt("soc_cap_uah", -1);
        soc_state.soc_basis_points = settings.GetInt("soc_bp", -1);
        soc_state.last_voltage_mv = settings.GetInt("soc_last_mv", -1);
        soc_state.tracking_degraded = settings.GetBool("soc_degraded", false);
        if (battery_soc_estimator_.Restore(soc_state)) {
            battery_remaining_mah_.store(battery_soc_estimator_.GetRemainingMah());
            battery_percent_.store(
                static_cast<int>(std::lround(battery_soc_estimator_.GetSocPercent())));
            battery_soc_tracking_degraded_.store(battery_soc_estimator_.IsTrackingDegraded());
            ESP_LOGI(TAG, "Battery SoC restored: %.1f mAh (%.2f%%), degraded=%s",
                     battery_soc_estimator_.GetRemainingMah(),
                     battery_soc_estimator_.GetSocPercent(),
                     battery_soc_estimator_.IsTrackingDegraded() ? "yes" : "no");
        } else if (soc_state.version != 0) {
            ESP_LOGW(TAG, "Ignoring incompatible or invalid persisted battery SoC state");
        }
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

    void PersistBatterySoc(const char* reason) {
        if (!battery_soc_estimator_.IsInitialized()) {
            return;
        }
        const auto state = battery_soc_estimator_.GetPersistedState();
        Settings settings("desk_robot", true);
        settings.SetInt("soc_ver", state.version);
        settings.SetInt("soc_rem_uah", state.remaining_uah);
        settings.SetInt("soc_cap_uah", state.usable_capacity_uah);
        settings.SetInt("soc_bp", state.soc_basis_points);
        settings.SetInt("soc_last_mv", state.last_voltage_mv);
        settings.SetBool("soc_degraded", state.tracking_degraded);
        ESP_LOGI(TAG, "Battery SoC saved (%s): %.1f mAh (%.2f%%), degraded=%s", reason,
                 state.remaining_uah / 1000.0f, state.soc_basis_points / 100.0f,
                 state.tracking_degraded ? "yes" : "no");
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

    static const char* GyroTurnStopReasonName(GyroTurnStopReason reason) {
        switch (reason) {
            case GyroTurnStopReason::kNone:
                return "none";
            case GyroTurnStopReason::kActive:
                return "active";
            case GyroTurnStopReason::kTargetReached:
                return "target_reached";
            case GyroTurnStopReason::kTimeout:
                return "timeout";
            case GyroTurnStopReason::kStaleSensor:
                return "stale_sensor";
            case GyroTurnStopReason::kDirectionMismatch:
                return "direction_mismatch";
            case GyroTurnStopReason::kCancelled:
                return "cancelled";
            case GyroTurnStopReason::kRejected:
                return "rejected";
        }
        return "none";
    }

    static float SelectYawRate(const Mpu6050MotionSensor::Sample& sample) {
        float rate = sample.gyro_z_dps;
#if MPU6050_YAW_AXIS == 0
        rate = sample.gyro_x_dps;
#elif MPU6050_YAW_AXIS == 1
        rate = sample.gyro_y_dps;
#elif MPU6050_YAW_AXIS != 2
#error "MPU6050_YAW_AXIS must be 0 (X), 1 (Y), or 2 (Z)"
#endif
        return rate * MPU6050_YAW_SIGN;
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
                cJSON_AddBoolToObject(result, "gyro_bias_valid", motion_gyro_bias_valid_.load());
                const int64_t gyro_sample_us =
                    motion_sample_timestamp_us_.load(std::memory_order_acquire);
                cJSON_AddNumberToObject(
                    result, "gyro_sample_age_ms",
                    gyro_sample_us > 0 ? (esp_timer_get_time() - gyro_sample_us) / 1000.0 : -1.0);
                cJSON_AddBoolToObject(result, "gyro_turn_available", IsGyroTurnAvailable());
                cJSON_AddBoolToObject(result, "gyro_turn_pending", gyro_turn_pending_.load());
                cJSON_AddBoolToObject(result, "gyro_turn_active", gyro_turn_active_.load());
                cJSON_AddNumberToObject(result, "gyro_turn_target_deg",
                                        gyro_turn_target_deg_.load());
                cJSON_AddNumberToObject(result, "gyro_turn_progress_deg",
                                        gyro_turn_progress_deg_.load());
                cJSON_AddStringToObject(result, "gyro_turn_stop_reason",
                                        GyroTurnStopReasonName(gyro_turn_stop_reason_.load()));
                cJSON_AddNumberToObject(result, "yaw_rate_dps", motion_yaw_rate_dps_.load());
                cJSON_AddNumberToObject(result, "yaw_bias_dps", motion_yaw_bias_dps_.load());
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
        mcp_server.AddTool(
            "self.robot.turn_relative",
            "Turn the robot by a relative gyro-measured angle. Positive degrees turn right; "
            "negative degrees turn left. Use 90 for right 90 degrees or -90 for left 90 degrees. "
            "The command is rejected unless the MPU6050 is calibrated, the motors are idle, and "
            "the floor is safe.",
            PropertyList({Property("degrees", kPropertyTypeInteger, 90, -180, 180)}),
            [this](const PropertyList& properties) -> ToolResult {
                std::string message;
                if (!RequestGyroTurn(properties["degrees"].value<int>(), message)) {
                    return std::unexpected(message);
                }
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
        unsigned power_failures = 0;
        unsigned power_invalid_samples = 0;
        int64_t next_battery_display_us = 0;
        int64_t soc_last_save_us = esp_timer_get_time();
        float soc_last_saved_remaining_mah = battery_soc_estimator_.GetRemainingMah();
        bool soc_last_saved_degraded = battery_soc_estimator_.IsTrackingDegraded();
        bool capacity_test_was_active = false;
        bool capacity_previous_sample_valid = false;
        int64_t capacity_previous_sample_us = 0;
        float capacity_previous_current_ma = 0.0f;
        int64_t capacity_last_save_us = 0;
        double capacity_fractional_uah = 0.0;
        int64_t capacity_fractional_time_us = 0;
        int64_t capacity_low_voltage_started_us = 0;
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
        int gyro_bias_samples = 0;
        float gyro_bias_sum = 0.0f;
        float previous_gyro_bias_yaw_rate_dps = 0.0f;
        bool have_previous_gyro_bias_sample = false;
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
                battery_conversion_ready_.store(reading.conversion_ready);
                battery_math_overflow_.store(reading.math_overflow);
                if (read_ok && reading.valid) {
                    constexpr float kFilterAlpha = 0.25f;
                    if (!power_filter_initialized) {
                        filtered_voltage_v = reading.battery_voltage_v;
                        filtered_current_ma = reading.current_ma;
                        filtered_power_mw = reading.power_mw;
                        power_filter_initialized = true;
                    } else {
                        filtered_voltage_v +=
                            kFilterAlpha * (reading.battery_voltage_v - filtered_voltage_v);
                        filtered_current_ma +=
                            kFilterAlpha * (reading.current_ma - filtered_current_ma);
                        filtered_power_mw += kFilterAlpha * (reading.power_mw - filtered_power_mw);
                    }

                    bool seeded_soc = false;
                    if (!battery_soc_estimator_.IsInitialized()) {
                        battery_soc_estimator_.SeedFromVoltage(reading.battery_voltage_v);
                        seeded_soc = true;
                        ESP_LOGI(TAG, "Battery SoC seeded from %.3f V: %.2f%%",
                                 reading.battery_voltage_v, battery_soc_estimator_.GetSocPercent());
                    }
                    const float soc_before_update = battery_soc_estimator_.GetSocPercent();
                    const bool was_quasi_resting = battery_soc_estimator_.IsQuasiResting();
                    const bool was_full_anchored = battery_soc_estimator_.IsFullAnchored();
                    const bool was_bootstrap_voltage_rebased =
                        battery_soc_estimator_.WasBootstrapVoltageRebased();
                    const bool was_empty_anchored = battery_soc_estimator_.IsEmptyAnchored();
                    battery_soc_estimator_.Update(reading.battery_voltage_v, reading.current_ma,
                                                  !motors_.IsActive(), reading.charging, now_us);
                    if (was_quasi_resting != battery_soc_estimator_.IsQuasiResting()) {
                        ESP_LOGI(TAG,
                                 "Battery quasi-rest %s: voltage_soc=%.2f%% correction=%+.4f mAh",
                                 battery_soc_estimator_.IsQuasiResting() ? "qualified" : "reset",
                                 battery_soc_estimator_.GetQuasiRestVoltageSocPercent(),
                                 battery_soc_estimator_.GetCumulativeVoltageCorrectionMah());
                    }
                    const bool full_anchor_applied =
                        !was_full_anchored && battery_soc_estimator_.IsFullAnchored();
                    if (full_anchor_applied) {
                        ESP_LOGI(TAG, "Battery full anchor qualified at %.3f V, %+.1f mA",
                                 reading.battery_voltage_v, reading.current_ma);
                    }
                    const bool bootstrap_voltage_rebase_applied =
                        !was_bootstrap_voltage_rebased &&
                        battery_soc_estimator_.WasBootstrapVoltageRebased();
                    if (bootstrap_voltage_rebase_applied) {
                        ESP_LOGI(TAG, "Battery startup SoC rebased from %.2f%% to %.2f%%",
                                 soc_before_update, battery_soc_estimator_.GetSocPercent());
                    }
                    const bool empty_anchor_applied =
                        !was_empty_anchored && battery_soc_estimator_.IsEmptyAnchored();
                    if (empty_anchor_applied) {
                        ESP_LOGI(TAG, "Battery empty anchor qualified at %.3f V, %+.1f mA",
                                 reading.battery_voltage_v, reading.current_ma);
                    }
                    battery_voltage_v_.store(filtered_voltage_v);
                    battery_current_ma_.store(std::fabs(filtered_current_ma));
                    battery_power_mw_.store(std::fabs(filtered_power_mw));
                    battery_signed_current_ma_.store(reading.current_ma);
                    battery_shunt_voltage_mv_.store(reading.shunt_voltage_mv);
                    battery_bus_voltage_v_.store(reading.bus_voltage_v);
                    battery_remaining_mah_.store(battery_soc_estimator_.GetRemainingMah());
                    battery_percent_.store(std::clamp(
                        static_cast<int>(std::lround(battery_soc_estimator_.GetSocPercent())), 0,
                        100));
                    battery_soc_tracking_degraded_.store(
                        battery_soc_estimator_.IsTrackingDegraded());
                    battery_soc_quasi_resting_.store(battery_soc_estimator_.IsQuasiResting());
                    battery_soc_voltage_reference_percent_.store(
                        battery_soc_estimator_.GetQuasiRestVoltageSocPercent());
                    battery_soc_voltage_correction_mah_.store(
                        battery_soc_estimator_.GetCumulativeVoltageCorrectionMah());
                    battery_soc_full_anchored_.store(battery_soc_estimator_.IsFullAnchored());
                    battery_soc_bootstrap_voltage_rebased_.store(
                        battery_soc_estimator_.WasBootstrapVoltageRebased());
                    battery_soc_empty_anchored_.store(battery_soc_estimator_.IsEmptyAnchored());
                    battery_charging_.store(reading.charging);
                    battery_discharging_.store(reading.discharging);
                    battery_valid_.store(true);

                    constexpr int64_t kSocMinimumSaveIntervalUs = 5 * 60 * 1000000LL;
                    constexpr float kSocMinimumSaveChangeMah = 10.0f;
                    const float remaining_mah = battery_soc_estimator_.GetRemainingMah();
                    const bool degraded_changed =
                        battery_soc_estimator_.IsTrackingDegraded() != soc_last_saved_degraded;
                    const bool meaningful_change =
                        std::fabs(remaining_mah - soc_last_saved_remaining_mah) >=
                            kSocMinimumSaveChangeMah ||
                        degraded_changed;
                    if (full_anchor_applied || bootstrap_voltage_rebase_applied ||
                        empty_anchor_applied || seeded_soc ||
                        (meaningful_change &&
                         now_us - soc_last_save_us >= kSocMinimumSaveIntervalUs)) {
                        const char* save_reason = "periodic";
                        if (full_anchor_applied) {
                            save_reason = "full anchor";
                        } else if (bootstrap_voltage_rebase_applied) {
                            save_reason = "startup voltage rebase";
                        } else if (empty_anchor_applied) {
                            save_reason = "empty anchor";
                        } else if (seeded_soc) {
                            save_reason = "voltage seed";
                        }
                        PersistBatterySoc(save_reason);
                        soc_last_save_us = now_us;
                        soc_last_saved_remaining_mah = remaining_mah;
                        soc_last_saved_degraded = battery_soc_estimator_.IsTrackingDegraded();
                    }

                    const bool capacity_active = battery_capacity_test_active_.load();
                    if (capacity_active && !capacity_test_was_active) {
                        capacity_previous_sample_valid = false;
                        capacity_previous_sample_us = 0;
                        capacity_previous_current_ma = 0.0f;
                        capacity_last_save_us = now_us;
                        capacity_fractional_uah = 0.0;
                        capacity_fractional_time_us = 0;
                        capacity_low_voltage_started_us = 0;
                    }
                    const bool capacity_measuring = capacity_active && reading.discharging;
                    battery_capacity_test_measuring_.store(capacity_measuring);
                    if (capacity_measuring) {
                        if (capacity_previous_sample_valid) {
                            const int64_t elapsed_us = now_us - capacity_previous_sample_us;
                            if (elapsed_us > 0 &&
                                elapsed_us <= BATTERY_SOC_MAX_INTEGRATION_GAP_MS * 1000LL) {
                                const double average_discharge_ma =
                                    0.5 * (static_cast<double>(capacity_previous_current_ma) +
                                           reading.current_ma);
                                if (average_discharge_ma > 0.0) {
                                    // mA * us / 3,600,000 = uAh.
                                    capacity_fractional_uah +=
                                        average_discharge_ma * elapsed_us / 3600000.0;
                                    const uint32_t whole_uah =
                                        static_cast<uint32_t>(capacity_fractional_uah);
                                    if (whole_uah > 0) {
                                        battery_capacity_test_uah_.fetch_add(whole_uah);
                                        capacity_fractional_uah -= whole_uah;
                                    }
                                    capacity_fractional_time_us += elapsed_us;
                                    const uint32_t whole_seconds = static_cast<uint32_t>(
                                        capacity_fractional_time_us / 1000000LL);
                                    if (whole_seconds > 0) {
                                        battery_capacity_test_seconds_.fetch_add(whole_seconds);
                                        capacity_fractional_time_us -=
                                            static_cast<int64_t>(whole_seconds) * 1000000LL;
                                    }
                                }
                            } else {
                                ESP_LOGW(TAG,
                                         "Capacity test skipped unknown integration gap: %lld us",
                                         static_cast<long long>(elapsed_us));
                            }
                        }
                        capacity_previous_sample_valid = true;
                        capacity_previous_sample_us = now_us;
                        capacity_previous_current_ma = reading.current_ma;
                    } else {
                        capacity_previous_sample_valid = false;
                    }

                    if (capacity_measuring &&
                        reading.battery_voltage_v <= BATTERY_CAPACITY_LOW_VOLTAGE_V) {
                        if (capacity_low_voltage_started_us == 0) {
                            capacity_low_voltage_started_us = now_us;
                        }
                    } else {
                        capacity_low_voltage_started_us = 0;
                    }
                    if (capacity_low_voltage_started_us > 0 &&
                        now_us - capacity_low_voltage_started_us >=
                            BATTERY_CAPACITY_LOW_VOLTAGE_DURATION_MS * 1000LL) {
                        battery_capacity_test_active_.store(false);
                        battery_capacity_test_measuring_.store(false);
                        PersistBatteryCapacityTest();
                        capacity_low_voltage_started_us = 0;
                        capacity_previous_sample_valid = false;
                        ESP_LOGI(TAG,
                                 "Battery capacity measurement stopped after %d ms at/below "
                                 "%.2f V",
                                 BATTERY_CAPACITY_LOW_VOLTAGE_DURATION_MS,
                                 BATTERY_CAPACITY_LOW_VOLTAGE_V);
                    }
                    if (capacity_measuring && now_us - capacity_last_save_us >= 60000000LL) {
                        PersistBatteryCapacityTest();
                        capacity_last_save_us = now_us;
                    }
                    capacity_test_was_active = battery_capacity_test_active_.load();
                    power_failures = 0;
                    power_invalid_samples = 0;

                    if (now_us >= next_battery_display_us) {
                        next_battery_display_us = now_us + 1000000LL;
                        const int percent = battery_percent_.load();
                        const float voltage = battery_voltage_v_.load();
                        const bool charging = battery_charging_.load();
                        Application::GetInstance().Schedule([this, percent, voltage, charging]() {
                            display_->SetBatteryStatus(percent, voltage, charging);
                        });
                    }
                } else {
                    battery_valid_.store(false);
                    battery_capacity_test_measuring_.store(false);
                    capacity_previous_sample_valid = false;
                    capacity_low_voltage_started_us = 0;
                    battery_soc_estimator_.MarkMeasurementGap();
                    battery_soc_tracking_degraded_.store(
                        battery_soc_estimator_.IsTrackingDegraded());
                    battery_soc_quasi_resting_.store(false);
                    battery_soc_voltage_reference_percent_.store(0.0f);
                    if (!read_ok) {
                        power_invalid_samples = 0;
                        if (++power_failures == 1 || power_failures % 30 == 0) {
                            ESP_LOGW(TAG, "INA219 I2C read failed (%u consecutive)",
                                     power_failures);
                        }
                    } else {
                        power_failures = 0;
                        if (++power_invalid_samples == 1 || power_invalid_samples % 30 == 0) {
                            ESP_LOGW(TAG,
                                     "INA219 sample invalid (%u consecutive): "
                                     "conversion_ready=%s math_overflow=%s",
                                     power_invalid_samples, reading.conversion_ready ? "yes" : "no",
                                     reading.math_overflow ? "yes" : "no");
                        }
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
                    const float raw_yaw_rate_dps = SelectYawRate(sample);

                    // Bias calibration must measure the yaw axis' zero-rate offset, so do not
                    // require the absolute gyro reading (or the 3-axis magnitude) to be near
                    // zero. A real MPU6050 may sit still with a several-dps constant offset.
                    // Instead require the yaw reading itself to remain stable between samples.
                    bool yaw_stable = true;
                    if (have_previous_gyro_bias_sample) {
                        yaw_stable =
                            std::fabs(raw_yaw_rate_dps - previous_gyro_bias_yaw_rate_dps) <=
                            kGyroBiasYawStabilityDps;
                    }
                    previous_gyro_bias_yaw_rate_dps = raw_yaw_rate_dps;
                    have_previous_gyro_bias_sample = true;

                    const bool gyro_still =
                        !motor_activity_active_.load(std::memory_order_relaxed) && yaw_stable &&
                        sample.acceleration_magnitude_g > 0.85f &&
                        sample.acceleration_magnitude_g < 1.15f;

                    if (!motion_gyro_bias_valid_.load(std::memory_order_relaxed)) {
                        if (gyro_still) {
                            gyro_bias_sum += raw_yaw_rate_dps;
                            ++gyro_bias_samples;
                            if (gyro_bias_samples >= kGyroBiasSamples) {
                                const float bias = gyro_bias_sum / gyro_bias_samples;
                                motion_yaw_bias_dps_.store(bias, std::memory_order_relaxed);
                                motion_gyro_bias_valid_.store(true, std::memory_order_release);
                                ESP_LOGI(TAG, "MPU6050 yaw bias calibrated: %.3f dps (%d samples)",
                                         bias, gyro_bias_samples);
                            }
                        } else {
                            gyro_bias_samples = 0;
                            gyro_bias_sum = 0.0f;
                        }
                    }
                    if (motion_gyro_bias_valid_.load(std::memory_order_acquire)) {
                        motion_yaw_rate_dps_.store(
                            raw_yaw_rate_dps - motion_yaw_bias_dps_.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
                        motion_sample_timestamp_us_.store(now_us, std::memory_order_release);
                    }
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
                                if (QueueTemporaryEmotion(MotionGestureName(gesture), duration_ms,
                                                          EmotionSource::kMpuReaction)) {
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

#ifdef MPU6050_I2C_ADDRESS
    bool IsGyroTurnAvailable() const {
        const int64_t sample_us = motion_sample_timestamp_us_.load(std::memory_order_acquire);
        return gyro_turn_task_ != nullptr && motion_sensor_valid_.load(std::memory_order_relaxed) &&
               motion_gyro_bias_valid_.load(std::memory_order_acquire) && sample_us > 0 &&
               esp_timer_get_time() - sample_us <= kGyroSampleStaleUs;
    }

    bool RequestGyroTurn(int target_degrees, std::string& message) {
        if (target_degrees < -180 || target_degrees > 180 || std::abs(target_degrees) < 3) {
            message = "Gyro turn angle must be between 3 and 180 degrees";
            return false;
        }
        if (!IsGyroTurnAvailable()) {
            message = "Gyro turn unavailable: MPU6050 is not ready";
            return false;
        }
        if (motors_.IsActive()) {
            message = "Gyro turn blocked: motors are busy";
            return false;
        }
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
        if (!distance_valid_.load(std::memory_order_relaxed) || IsCliffDetected()) {
            message = "Gyro turn blocked: no safe floor detected";
            return false;
        }
#endif
        if (gyro_turn_pending_.exchange(true, std::memory_order_acq_rel)) {
            message = "Gyro turn blocked: another turn is pending";
            return false;
        }

        gyro_turn_target_deg_.store(static_cast<float>(target_degrees), std::memory_order_relaxed);
        gyro_turn_progress_deg_.store(0.0f, std::memory_order_relaxed);
        gyro_turn_stop_reason_.store(GyroTurnStopReason::kNone, std::memory_order_relaxed);
        Application::GetInstance().Schedule([this, target_degrees]() {
            if (!StartGyroTurn(static_cast<float>(target_degrees), 100)) {
                gyro_turn_stop_reason_.store(GyroTurnStopReason::kRejected,
                                             std::memory_order_relaxed);
                ESP_LOGW(TAG, "Relative turn %d deg was rejected", target_degrees);
            }
            gyro_turn_pending_.store(false, std::memory_order_release);
        });
        const int magnitude = target_degrees < 0 ? -target_degrees : target_degrees;
        message = std::string("Turning ") + (target_degrees < 0 ? "left " : "right ") +
                  std::to_string(magnitude) + " degrees";
        return true;
    }

    bool StartGyroTurn(float target_deg, uint8_t intensity_percent, bool emotion_owned = false) {
        if (!IsGyroTurnAvailable() || gyro_turn_active_.load(std::memory_order_relaxed) ||
            motors_.IsActive() || std::fabs(target_deg) < kGyroTurnToleranceDeg) {
            return false;
        }
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
        if (!distance_valid_.load(std::memory_order_relaxed) || IsCliffDetected()) {
            return false;
        }
#endif
        const uint32_t timeout_ms = std::min<uint32_t>(
            kGyroTurnMaxTimeoutMs, static_cast<uint32_t>(500.0f + std::fabs(target_deg) * 55.0f));
        const auto direction = target_deg > 0.0f ? MotorController::Direction::kRight
                                                 : MotorController::Direction::kLeft;
        if (!motors_.Drive(direction, timeout_ms, intensity_percent)) {
            return false;
        }
        gyro_turn_target_deg_.store(target_deg, std::memory_order_relaxed);
        gyro_turn_progress_deg_.store(0.0f, std::memory_order_relaxed);
        gyro_turn_intensity_percent_.store(intensity_percent, std::memory_order_relaxed);
        gyro_turn_timeout_ms_.store(timeout_ms, std::memory_order_relaxed);
        gyro_turn_stop_reason_.store(GyroTurnStopReason::kActive, std::memory_order_relaxed);
        gyro_turn_active_.store(true, std::memory_order_release);
        emotion_movement_active_.store(emotion_owned, std::memory_order_relaxed);
        xTaskNotifyGive(gyro_turn_task_);
        return true;
    }

    static void GyroTurnTask(void* arg) { static_cast<DeskRobotBoard*>(arg)->RunGyroTurnTask(); }

    void RunGyroTurnTask() {
        while (true) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            if (!gyro_turn_active_.load(std::memory_order_acquire)) {
                continue;
            }

            const float target_deg = gyro_turn_target_deg_.load(std::memory_order_relaxed);
            const uint8_t maximum_intensity =
                gyro_turn_intensity_percent_.load(std::memory_order_relaxed);
            const int64_t started_us = esp_timer_get_time();
            const int64_t deadline_us =
                started_us + static_cast<int64_t>(gyro_turn_timeout_ms_.load()) * 1000;
            int64_t previous_sample_us = 0;
            float turned_deg = 0.0f;
            uint8_t applied_intensity = maximum_intensity;
            GyroTurnStopReason stop_reason = GyroTurnStopReason::kNone;

            while (gyro_turn_active_.load(std::memory_order_acquire)) {
                const int64_t now_us = esp_timer_get_time();
                const int64_t sample_us =
                    motion_sample_timestamp_us_.load(std::memory_order_acquire);
                if (now_us >= deadline_us) {
                    stop_reason = GyroTurnStopReason::kTimeout;
                    break;
                }
                if (!motion_sensor_valid_.load(std::memory_order_relaxed) ||
                    !motion_gyro_bias_valid_.load(std::memory_order_acquire) || sample_us <= 0 ||
                    now_us - sample_us > kGyroSampleStaleUs) {
                    stop_reason = GyroTurnStopReason::kStaleSensor;
                    break;
                }
                if (sample_us != previous_sample_us) {
                    if (previous_sample_us > 0) {
                        const float dt_seconds =
                            std::clamp((sample_us - previous_sample_us) / 1000000.0f, 0.0f, 0.1f);
                        turned_deg +=
                            motion_yaw_rate_dps_.load(std::memory_order_relaxed) * dt_seconds;
                        gyro_turn_progress_deg_.store(turned_deg, std::memory_order_relaxed);
                    }
                    previous_sample_us = sample_us;
                    const float remaining_deg = target_deg - turned_deg;
                    if (std::fabs(remaining_deg) <= kGyroTurnToleranceDeg ||
                        (target_deg > 0.0f && turned_deg > target_deg) ||
                        (target_deg < 0.0f && turned_deg < target_deg)) {
                        stop_reason = GyroTurnStopReason::kTargetReached;
                        break;
                    }
                    if (target_deg * turned_deg < 0.0f && std::fabs(turned_deg) > 2.0f) {
                        stop_reason = GyroTurnStopReason::kDirectionMismatch;
                        break;
                    }

                    uint8_t desired_intensity = maximum_intensity;
                    if (std::fabs(remaining_deg) <= 4.0f) {
                        desired_intensity = std::min<uint8_t>(maximum_intensity, 60);
                    } else if (std::fabs(remaining_deg) <= 10.0f) {
                        desired_intensity = std::min<uint8_t>(maximum_intensity, 70);
                    }
                    if (desired_intensity != applied_intensity &&
                        motors_.SetActiveIntensityPercent(desired_intensity)) {
                        applied_intensity = desired_intensity;
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            if (stop_reason != GyroTurnStopReason::kNone && gyro_turn_active_.exchange(false)) {
                gyro_turn_progress_deg_.store(turned_deg, std::memory_order_relaxed);
                gyro_turn_stop_reason_.store(stop_reason, std::memory_order_relaxed);
                ESP_LOGI(TAG, "Gyro turn %.1f/%.1f deg stopped: %s", turned_deg, target_deg,
                         GyroTurnStopReasonName(stop_reason));
                motors_.EmergencyStop();
            }
        }
    }

    void InitializeGyroTurnController() {
        if (xTaskCreate(GyroTurnTask, "gyro_turn", 4096, this, 2, &gyro_turn_task_) != pdPASS) {
            gyro_turn_task_ = nullptr;
            ESP_LOGE(TAG, "Failed to create gyro turn controller task");
        }
    }

    bool TryStartGyroEmotionTurn(const std::string& emotion) {
        float magnitude_deg = 0.0f;
        uint8_t intensity_percent = 0;
        if (emotion == "thinking" || emotion == "suspicious") {
            magnitude_deg = 8.0f + static_cast<float>(esp_random() % 5);
            intensity_percent = 68;
        } else if (emotion == "confused") {
            magnitude_deg = 10.0f + static_cast<float>(esp_random() % 5);
            intensity_percent = 72;
        } else if (emotion == "surprised" || emotion == "shocked") {
            magnitude_deg = 7.0f + static_cast<float>(esp_random() % 4);
            intensity_percent = 85;
        } else {
            return false;
        }
        if ((esp_random() & 1U) == 0) {
            magnitude_deg = -magnitude_deg;
        }
        return StartGyroTurn(magnitude_deg, intensity_percent, true);
    }
#endif

    void OnNetworkEvent(NetworkEvent event, const std::string& data = "") override {
        WifiBoard::OnNetworkEvent(event, data);

        switch (event) {
            case NetworkEvent::Scanning:
#ifdef SECONDARY_OLED_I2C_ADDRESS
                secondary_oled_network_state_.store(SecondaryOled::NetworkState::kScanning,
                                                    std::memory_order_relaxed);
#endif
                display_->SetWifiConnected(false);
                display_->ShowBootSplash();
                break;
            case NetworkEvent::Connecting:
#ifdef SECONDARY_OLED_I2C_ADDRESS
                secondary_oled_network_state_.store(SecondaryOled::NetworkState::kConnecting,
                                                    std::memory_order_relaxed);
#endif
                display_->SetWifiConnected(false);
                display_->ShowBootSplash();
                break;
            case NetworkEvent::Disconnected:
#ifdef SECONDARY_OLED_I2C_ADDRESS
                secondary_oled_network_state_.store(SecondaryOled::NetworkState::kDisconnected,
                                                    std::memory_order_relaxed);
#endif
                display_->SetWifiConnected(false);
                display_->ShowBootSplash();
                break;
            case NetworkEvent::Connected:
#ifdef SECONDARY_OLED_I2C_ADDRESS
                secondary_oled_network_state_.store(SecondaryOled::NetworkState::kConnected,
                                                    std::memory_order_relaxed);
#endif
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
#ifdef SECONDARY_OLED_I2C_ADDRESS
                secondary_oled_network_state_.store(SecondaryOled::NetworkState::kConfigMode,
                                                    std::memory_order_relaxed);
#endif
                display_->SetWifiConnected(false);
                display_->HideBootSplash();
                break;
            case NetworkEvent::WifiConfigModeExit:
#ifdef SECONDARY_OLED_I2C_ADDRESS
                secondary_oled_network_state_.store(SecondaryOled::NetworkState::kConnecting,
                                                    std::memory_order_relaxed);
#endif
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
    static std::string SecondaryOledWidgetKey(size_t index, const char* field) {
        return "ow" + std::to_string(index) + "_" + field;
    }

    static int SecondaryOledWidgetActionIndex(const std::string& action,
                                              const std::string& prefix) {
        if (action.size() != prefix.size() + 1 || action.compare(0, prefix.size(), prefix) != 0) {
            return -1;
        }
        const char digit = action.back();
        if (digit < '0' || static_cast<size_t>(digit - '0') >= secondary_oled_layout::kMaxWidgets) {
            return -1;
        }
        return digit - '0';
    }

    static const char* SecondaryOledWidgetTypeName(SecondaryOled::WidgetType type) {
        switch (type) {
            case SecondaryOled::WidgetType::kBranding:
                return "branding";
            case SecondaryOled::WidgetType::kDistance:
                return "distance";
            case SecondaryOled::WidgetType::kPower:
                return "power";
            case SecondaryOled::WidgetType::kMotion:
                return "motion";
            case SecondaryOled::WidgetType::kCapacity:
                return "capacity";
        }
        return "branding";
    }

    static void LoadSecondaryOledWidgets(Settings& settings, SecondaryOled::Config& config) {
        if (settings.GetInt("oled_w_ver", 0) != 1) {
            return;
        }
        auto widgets = config.widgets;
        std::array<bool, secondary_oled_layout::kMaxWidgets> seen = {};
        for (size_t index = 0; index < widgets.size(); ++index) {
            const int type = settings.GetInt(SecondaryOledWidgetKey(index, "type"), -1);
            if (type < 0 || type >= static_cast<int>(secondary_oled_layout::kMaxWidgets) ||
                seen[type]) {
                ESP_LOGW(TAG, "Ignoring invalid persisted secondary OLED widget order");
                return;
            }
            seen[type] = true;
            widgets[index].type = static_cast<SecondaryOled::WidgetType>(type);
            widgets[index].size = static_cast<SecondaryOled::WidgetSize>(std::clamp(
                static_cast<int>(settings.GetInt(SecondaryOledWidgetKey(index, "size"), 0)), 0, 2));
            widgets[index].enabled = settings.GetBool(SecondaryOledWidgetKey(index, "on"), true);
            widgets[index].mode = static_cast<uint8_t>(std::clamp(
                static_cast<int>(settings.GetInt(SecondaryOledWidgetKey(index, "mode"), 0)), 0, 2));
        }
        config.widgets = widgets;
    }

    static void SecondaryOledTask(void* arg) {
        auto* self = static_cast<DeskRobotBoard*>(arg);
        // Let the board constructor and application singleton finish before reading runtime state.
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_LOGI(TAG, "Secondary OLED dashboard task started");
        TickType_t last_wake_time = xTaskGetTickCount();
        while (true) {
            SecondaryOled::Telemetry telemetry;
            telemetry.network_state =
                self->secondary_oled_network_state_.load(std::memory_order_relaxed);
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
            telemetry.distance_mm = self->distance_mm_.load(std::memory_order_relaxed);
            telemetry.distance_valid = self->distance_valid_.load(std::memory_order_relaxed);
            telemetry.cliff_detected = self->IsCliffDetected();
#endif
#ifdef INA219_I2C_ADDRESS
            telemetry.power_valid = self->battery_valid_.load(std::memory_order_relaxed);
            telemetry.current_ma = static_cast<int>(
                std::lround(self->battery_current_ma_.load(std::memory_order_relaxed)));
            telemetry.power_mw = static_cast<int>(
                std::lround(self->battery_power_mw_.load(std::memory_order_relaxed)));
            telemetry.capacity_active =
                self->battery_capacity_test_active_.load(std::memory_order_relaxed);
            telemetry.capacity_measuring =
                self->battery_capacity_test_measuring_.load(std::memory_order_relaxed);
            telemetry.capacity_uah =
                self->battery_capacity_test_uah_.load(std::memory_order_relaxed);
            telemetry.capacity_seconds =
                self->battery_capacity_test_seconds_.load(std::memory_order_relaxed);
            telemetry.battery_percent = self->battery_percent_.load(std::memory_order_relaxed);
            telemetry.battery_voltage_mv = static_cast<int>(
                std::lround(self->battery_voltage_v_.load(std::memory_order_relaxed) * 1000.0f));
            telemetry.low_battery =
                telemetry.power_valid && !self->battery_charging_.load(std::memory_order_relaxed) &&
                telemetry.battery_percent >= 0 && telemetry.battery_percent < 20;
#endif
#ifdef MPU6050_I2C_ADDRESS
            telemetry.motion_valid = self->motion_sensor_valid_.load(std::memory_order_relaxed);
            telemetry.motion_state =
                MotionGestureName(self->motion_gesture_.load(std::memory_order_relaxed));
            telemetry.roll_deg = static_cast<int>(
                std::lround(self->motion_roll_deg_.load(std::memory_order_relaxed)));
            telemetry.pitch_deg = static_cast<int>(
                std::lround(self->motion_pitch_deg_.load(std::memory_order_relaxed)));
            telemetry.motion_calibrating =
                self->motion_sensor_.IsAvailable() &&
                (!telemetry.motion_valid ||
                 !self->motion_gyro_bias_valid_.load(std::memory_order_acquire));
            telemetry.gyro_turn_pending = self->gyro_turn_pending_.load(std::memory_order_relaxed);
            telemetry.gyro_turn_active = self->gyro_turn_active_.load(std::memory_order_relaxed);
            telemetry.gyro_turn_target_deg = static_cast<int>(
                std::lround(self->gyro_turn_target_deg_.load(std::memory_order_relaxed)));
            telemetry.gyro_turn_progress_deg = static_cast<int>(
                std::lround(self->gyro_turn_progress_deg_.load(std::memory_order_relaxed)));
            telemetry.gyro_turn_intensity_percent =
                self->gyro_turn_intensity_percent_.load(std::memory_order_relaxed);
#endif
            self->secondary_oled_.UpdateTelemetry(telemetry);
            self->secondary_oled_.Tick();
            vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(100));
        }
    }

    void InitializeSecondaryOled() {
        Settings settings("desk_robot", false);
        SecondaryOled::Config oled_config;
        oled_config.flip_180 = settings.GetBool("oled_flip", SECONDARY_OLED_FLIP_180);
        oled_config.contrast = static_cast<uint8_t>(
            std::clamp(static_cast<int>(settings.GetInt("oled_contrast", 128)), 0, 255));
        oled_config.brand = settings.GetString("oled_brand", "Desk Robot");
        oled_config.distance_prefix = settings.GetString("oled_prefix", "Dist");
        LoadSecondaryOledWidgets(settings, oled_config);
        if (!secondary_oled_.Initialize(auxiliary_i2c_bus_, auxiliary_i2c_mutex_,
                                        SECONDARY_OLED_I2C_ADDRESS, SECONDARY_OLED_WIDTH,
                                        SECONDARY_OLED_HEIGHT, oled_config.flip_180)) {
            return;
        }
        secondary_oled_.Configure(oled_config);
        if (xTaskCreate(SecondaryOledTask, "status_oled", 8192, this, 2, &secondary_oled_task_) !=
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
        config.brand = NormalizeOledText(config.brand, "Desk Robot");
        config.distance_prefix = NormalizeOledText(config.distance_prefix, "Dist");
        if (config.distance_prefix.size() > 10) {
            config.distance_prefix.resize(10);
        }
        Application::GetInstance().Schedule([this, config = std::move(config)]() {
            if (!secondary_oled_.Configure(config)) {
                return;
            }
            Settings settings("desk_robot", true);
            settings.SetBool("oled_flip", config.flip_180);
            settings.SetInt("oled_contrast", config.contrast);
            settings.SetString("oled_brand", config.brand);
            settings.SetString("oled_prefix", config.distance_prefix);
            settings.SetInt("oled_w_ver", 1);
            for (size_t index = 0; index < config.widgets.size(); ++index) {
                const auto& widget = config.widgets[index];
                settings.SetInt(SecondaryOledWidgetKey(index, "type"),
                                static_cast<int>(widget.type));
                settings.SetInt(SecondaryOledWidgetKey(index, "size"),
                                static_cast<int>(widget.size));
                settings.SetBool(SecondaryOledWidgetKey(index, "on"), widget.enabled);
                settings.SetInt(SecondaryOledWidgetKey(index, "mode"), widget.mode);
            }
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

    std::vector<MotorController::Movement> BuildEmotionMovement(const std::string& emotion) {
        enum Group : size_t {
            kJoy,
            kAffection,
            kSad,
            kBold,
            kSurprise,
            kCurious,
            kAwkward,
            kShake,
        };

        size_t group = 0;
        if (emotion == "happy" || emotion == "laughing" || emotion == "funny" ||
            emotion == "delicious") {
            group = kJoy;
        } else if (emotion == "loving" || emotion == "kissy" || emotion == "winking") {
            group = kAffection;
        } else if (emotion == "sad" || emotion == "crying" || emotion == "sleepy") {
            group = kSad;
        } else if (emotion == "angry" || emotion == "confident" || emotion == "cool") {
            group = kBold;
        } else if (emotion == "surprised" || emotion == "shocked") {
            group = kSurprise;
        } else if (emotion == "thinking" || emotion == "confused" || emotion == "suspicious") {
            group = kCurious;
        } else if (emotion == "embarrassed" || emotion == "silly") {
            group = kAwkward;
        } else if (emotion == "shake") {
            group = kShake;
        } else {
            // Neutral, relaxed, activity states, and directional looks are face-only.
            return {};
        }

        constexpr uint8_t kVariantCount = 3;
        uint8_t variant = static_cast<uint8_t>(esp_random() % kVariantCount);
        if (variant == last_emotion_variants_[group]) {
            variant = static_cast<uint8_t>((variant + 1 + esp_random() % (kVariantCount - 1)) %
                                           kVariantCount);
        }
        last_emotion_variants_[group] = variant;

        using Direction = MotorController::Direction;
        switch (group) {
            case kJoy:
                if (variant == 0) {
                    return {{Direction::kLeft, 100, 85},
                            {Direction::kRight, 160, 85},
                            {Direction::kLeft, 100, 80}};
                }
                if (variant == 1) {
                    return {{Direction::kForward, 100, 75},
                            {Direction::kBackward, 100, 70},
                            {Direction::kRight, 120, 80}};
                }
                return {{Direction::kRight, 110, 85},
                        {Direction::kLeft, 170, 85},
                        {Direction::kRight, 110, 80}};
            case kAffection:
                if (variant == 0) {
                    return {{Direction::kLeft, 120, 65}, {Direction::kRight, 120, 65}};
                }
                if (variant == 1) {
                    return {{Direction::kForward, 90, 60}, {Direction::kBackward, 90, 60}};
                }
                return {{Direction::kRight, 130, 65}, {Direction::kLeft, 100, 60}};
            case kSad:
                if (variant == 0) {
                    return {{Direction::kBackward, 100, 60}, {Direction::kLeft, 110, 60}};
                }
                if (variant == 1) {
                    return {{Direction::kRight, 120, 60}, {Direction::kLeft, 120, 60}};
                }
                return {{Direction::kLeft, 140, 60}};
            case kBold:
                if (variant == 0) {
                    return {{Direction::kForward, 130, 90}, {Direction::kBackward, 80, 75}};
                }
                if (variant == 1) {
                    return {{Direction::kLeft, 150, 90}, {Direction::kRight, 80, 75}};
                }
                return {{Direction::kRight, 150, 90}, {Direction::kLeft, 80, 75}};
            case kSurprise:
                if (variant == 0) {
                    return {{Direction::kBackward, 80, 90}};
                }
                if (variant == 1) {
                    return {{Direction::kLeft, 80, 90}, {Direction::kRight, 90, 85}};
                }
                return {{Direction::kRight, 80, 90}, {Direction::kLeft, 90, 85}};
            case kCurious:
                if (variant == 0) {
                    return {{Direction::kLeft, 110, 65}};
                }
                if (variant == 1) {
                    return {{Direction::kRight, 110, 65}};
                }
                return {{Direction::kLeft, 90, 65}, {Direction::kRight, 140, 65}};
            case kAwkward:
                if (variant == 0) {
                    return {{Direction::kLeft, 90, 70},
                            {Direction::kRight, 130, 70},
                            {Direction::kLeft, 80, 65}};
                }
                if (variant == 1) {
                    return {{Direction::kBackward, 80, 65}, {Direction::kRight, 100, 70}};
                }
                return {{Direction::kRight, 90, 70},
                        {Direction::kLeft, 130, 70},
                        {Direction::kRight, 80, 65}};
            case kShake:
                if (variant == 0) {
                    return {{Direction::kLeft, 100, 85},
                            {Direction::kRight, 160, 85},
                            {Direction::kLeft, 160, 85},
                            {Direction::kRight, 100, 80}};
                }
                if (variant == 1) {
                    return {{Direction::kRight, 100, 85},
                            {Direction::kLeft, 160, 85},
                            {Direction::kRight, 160, 85},
                            {Direction::kLeft, 100, 80}};
                }
                return {{Direction::kLeft, 120, 80},
                        {Direction::kRight, 120, 80},
                        {Direction::kLeft, 120, 80},
                        {Direction::kRight, 120, 80}};
        }
        return {};
    }

    void MaybeStartEmotionMovement(const std::string& emotion, EmotionSource source) {
        if (!emotion_movement_enabled_.load(std::memory_order_relaxed) ||
            source == EmotionSource::kMpuReaction || motors_.IsActive()) {
            return;
        }
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
        // Automatic movement is conservative: unlike manual reverse, it requires a valid floor
        // sample and never starts while a cliff is active.
        if (!distance_valid_.load(std::memory_order_relaxed) || IsCliffDetected()) {
            return;
        }
#endif
        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_emotion_movement_us_ < kEmotionMovementCooldownUs) {
            return;
        }
#ifdef MPU6050_I2C_ADDRESS
        if (TryStartGyroEmotionTurn(emotion)) {
            last_emotion_movement_us_ = now_us;
            return;
        }
#endif
        auto movements = BuildEmotionMovement(emotion);
        if (movements.empty()) {
            return;
        }
        if (motors_.PlaySequence(movements)) {
            emotion_movement_active_.store(true, std::memory_order_relaxed);
            last_emotion_movement_us_ = now_us;
        } else {
            emotion_movement_active_.store(false, std::memory_order_relaxed);
            ESP_LOGW(TAG, "Emotion movement was rejected for %s", emotion.c_str());
        }
    }

    bool ApplyRobotEmotion(const std::string& emotion, EmotionSource source) {
        if (!MochanDisplay::IsSupportedEmotion(emotion)) {
            return false;
        }
        display_->SetEmotion(emotion.c_str());
        MaybeStartEmotionMovement(emotion, source);
        return true;
    }

    bool QueueTemporaryEmotion(const std::string& emotion, int duration_ms,
                               EmotionSource source = EmotionSource::kPreview) {
        if (!MochanDisplay::IsSupportedEmotion(emotion)) {
            return false;
        }
        const int safe_duration = std::clamp(duration_ms, 250, 30000);
        {
            std::lock_guard<std::mutex> lock(temporary_emotion_mutex_);
            temporary_emotion_ = emotion;
        }
        Application::GetInstance().Schedule(
            [this, emotion, source]() { ApplyRobotEmotion(emotion, source); });
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
                QueueTemporaryEmotion("surprised", 1600, EmotionSource::kMpuReaction);
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
        auto* led = static_cast<GpioLed*>(GetLed());
        led->SetBrightnessScale(static_cast<uint8_t>(safe_brightness));
#ifdef BUILTIN_LED_STATUS_PROFILE_EDISON
        if (BUILTIN_LED_STATUS_PROFILE_EDISON) {
            led->SetStatusProfile(GpioLed::StatusProfile::kEdison);
        }
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
            if (!moving) {
                emotion_movement_active_.store(false, std::memory_order_relaxed);
#ifdef MPU6050_I2C_ADDRESS
                if (gyro_turn_active_.exchange(false, std::memory_order_acq_rel)) {
                    gyro_turn_stop_reason_.store(GyroTurnStopReason::kCancelled,
                                                 std::memory_order_relaxed);
                }
#endif
            }
#ifdef BUILTIN_LED_STATUS_PROFILE_EDISON
            Application::GetInstance().Schedule(
                [this, moving]() { static_cast<GpioLed*>(GetLed())->SetActivityOverride(moving); });
#endif
        });
    }

    void InitializeMotorSettings() {
        Settings settings("desk_robot", false);
        const int speed =
            std::clamp(static_cast<int>(settings.GetInt("motor_speed", kDefaultMotorSpeedPercent)),
                       MotorController::kMinSpeedPercent, MotorController::kMaxSpeedPercent);
        const int drive_duration = std::clamp(
            static_cast<int>(settings.GetInt("drive_time", kDefaultDriveDurationMs)), 50, 2000);
        motors_.SetSpeedPercent(speed);
        drive_duration_ms_.store(drive_duration, std::memory_order_relaxed);
        emotion_movement_enabled_.store(settings.GetBool("emotion_move", false),
                                        std::memory_order_relaxed);
        ESP_LOGI(TAG, "Motor speed %d%%, drive time %d ms, emotion movement %s", speed,
                 drive_duration, emotion_movement_enabled_.load() ? "enabled" : "disabled");
    }

    void QueueMotorSpeed(int speed) {
        const int safe_speed =
            std::clamp(speed, MotorController::kMinSpeedPercent, MotorController::kMaxSpeedPercent);
        Application::GetInstance().Schedule([this, safe_speed]() {
            motors_.SetSpeedPercent(safe_speed);
            Settings settings("desk_robot", true);
            settings.SetInt("motor_speed", safe_speed);
        });
    }

    void QueueDriveDuration(int duration_ms) {
        const int safe_duration = std::clamp(duration_ms, 50, 2000);
        drive_duration_ms_.store(safe_duration, std::memory_order_relaxed);
        Application::GetInstance().Schedule([safe_duration]() {
            Settings settings("desk_robot", true);
            settings.SetInt("drive_time", safe_duration);
        });
    }

    void QueueEmotionMovementEnabled(bool enabled) {
        emotion_movement_enabled_.store(enabled, std::memory_order_relaxed);
        Application::GetInstance().Schedule([this, enabled]() {
            if (!enabled && emotion_movement_active_.load(std::memory_order_relaxed)) {
                motors_.Stop();
            }
            Settings settings("desk_robot", true);
            settings.SetBool("emotion_move", enabled);
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
#ifdef MPU6050_I2C_ADDRESS
        if (action == "turn_relative") {
            return RequestGyroTurn(duration_ms, message);
        }
#endif
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
        if (action == "oled_flip" || action == "oled_contrast" || action == "oled_brand" ||
            action == "oled_prefix") {
            SecondaryOled::Config config = secondary_oled_.GetConfig();
            if (action == "oled_flip") {
                config.flip_180 = !config.flip_180;
            } else if (action == "oled_contrast") {
                config.contrast = static_cast<uint8_t>(std::clamp(duration_ms, 0, 255));
            } else if (action == "oled_brand") {
                config.brand = NormalizeOledText(text, "Desk Robot");
            } else {
                config.distance_prefix = NormalizeOledText(text, "Dist");
            }
            QueueSecondaryOledConfig(config);
            message = "OLED settings updated";
            return true;
        }
        const int enabled_index = SecondaryOledWidgetActionIndex(action, "oled_widget_on_");
        const int size_index = SecondaryOledWidgetActionIndex(action, "oled_widget_size_");
        const int mode_index = SecondaryOledWidgetActionIndex(action, "oled_widget_mode_");
        const int up_index = SecondaryOledWidgetActionIndex(action, "oled_widget_up_");
        const int down_index = SecondaryOledWidgetActionIndex(action, "oled_widget_down_");
        if (enabled_index >= 0 || size_index >= 0 || mode_index >= 0 || up_index >= 0 ||
            down_index >= 0) {
            SecondaryOled::Config config = secondary_oled_.GetConfig();
            if (enabled_index >= 0) {
                config.widgets[enabled_index].enabled = duration_ms != 0;
            } else if (size_index >= 0) {
                config.widgets[size_index].size =
                    static_cast<SecondaryOled::WidgetSize>(std::clamp(duration_ms, 0, 2));
            } else if (mode_index >= 0) {
                config.widgets[mode_index].mode =
                    static_cast<uint8_t>(std::clamp(duration_ms, 0, 2));
            } else if (up_index > 0) {
                std::swap(config.widgets[up_index], config.widgets[up_index - 1]);
            } else if (down_index >= 0 &&
                       down_index + 1 < static_cast<int>(config.widgets.size())) {
                std::swap(config.widgets[down_index], config.widgets[down_index + 1]);
            } else {
                message = "Widget is already at that edge";
                return false;
            }
            QueueSecondaryOledConfig(config);
            message = "OLED widget layout updated";
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
            const int safe_speed = std::clamp(duration_ms, MotorController::kMinSpeedPercent,
                                              MotorController::kMaxSpeedPercent);
            QueueMotorSpeed(safe_speed);
            message = "Motor speed " + std::to_string(safe_speed) + "%";
            return true;
        }
        if (action == "drive_duration") {
            const int safe_duration = std::clamp(duration_ms, 50, 2000);
            QueueDriveDuration(safe_duration);
            message = "Drive time " + std::to_string(safe_duration) + " ms";
            return true;
        }
        if (action == "emotion_movement") {
            const bool enabled = duration_ms != 0;
            QueueEmotionMovementEnabled(enabled);
            message = enabled ? "Emotion movement enabled" : "Emotion movement disabled";
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
        snapshot_handler = [this](const RobotWebControlServer::SnapshotSender& sender) {
            return Application::GetInstance().GetDeviceState() == kDeviceStateIdle &&
                   camera_ != nullptr && camera_->SendWebSnapshot(sender);
        };
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
                cJSON_AddNumberToObject(root, "drive_duration_ms",
                                        drive_duration_ms_.load(std::memory_order_relaxed));
                cJSON_AddBoolToObject(root, "emotion_movement_enabled",
                                      emotion_movement_enabled_.load(std::memory_order_relaxed));
                cJSON_AddBoolToObject(root, "emotion_movement_active",
                                      emotion_movement_active_.load(std::memory_order_relaxed));
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
                cJSON_AddNumberToObject(root, "battery_signed_current_ma",
                                        battery_signed_current_ma_.load());
                cJSON_AddNumberToObject(root, "battery_shunt_voltage_mv",
                                        battery_shunt_voltage_mv_.load());
                cJSON_AddNumberToObject(root, "battery_bus_voltage_v",
                                        battery_bus_voltage_v_.load());
                cJSON_AddNumberToObject(root, "battery_remaining_mah",
                                        battery_remaining_mah_.load());
                cJSON_AddNumberToObject(root, "battery_capacity_mah",
                                        BATTERY_SOC_USABLE_CAPACITY_MAH);
                cJSON_AddStringToObject(root, "battery_soc_method", "coulomb_quasi_rest_anchors");
                cJSON_AddBoolToObject(root, "battery_soc_tracking_degraded",
                                      battery_soc_tracking_degraded_.load());
                cJSON_AddBoolToObject(root, "battery_soc_quasi_resting",
                                      battery_soc_quasi_resting_.load());
                cJSON_AddNumberToObject(root, "battery_soc_voltage_reference_percent",
                                        battery_soc_voltage_reference_percent_.load());
                cJSON_AddNumberToObject(root, "battery_soc_voltage_correction_mah",
                                        battery_soc_voltage_correction_mah_.load());
                cJSON_AddBoolToObject(root, "battery_soc_full_anchored",
                                      battery_soc_full_anchored_.load());
                cJSON_AddBoolToObject(root, "battery_soc_bootstrap_voltage_rebased",
                                      battery_soc_bootstrap_voltage_rebased_.load());
                cJSON_AddBoolToObject(root, "battery_soc_empty_anchored",
                                      battery_soc_empty_anchored_.load());
                cJSON_AddBoolToObject(root, "battery_conversion_ready",
                                      battery_conversion_ready_.load());
                cJSON_AddBoolToObject(root, "battery_math_overflow", battery_math_overflow_.load());
                const float signed_current_ma = battery_signed_current_ma_.load();
                const char* flow_state = !battery_valid_.load()       ? "unknown"
                                         : signed_current_ma < -20.0f ? "charging"
                                         : signed_current_ma > 20.0f  ? "discharging"
                                                                      : "near_zero";
                cJSON_AddStringToObject(root, "battery_flow_state", flow_state);
                cJSON_AddStringToObject(root, "external_power", "unknown");
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
                cJSON_AddNumberToObject(root, "motion_yaw_rate_dps", motion_yaw_rate_dps_.load());
                cJSON_AddNumberToObject(root, "motion_yaw_bias_dps", motion_yaw_bias_dps_.load());
                cJSON_AddBoolToObject(root, "gyro_bias_valid", motion_gyro_bias_valid_.load());
                const int64_t gyro_sample_us =
                    motion_sample_timestamp_us_.load(std::memory_order_acquire);
                cJSON_AddNumberToObject(
                    root, "gyro_sample_age_ms",
                    gyro_sample_us > 0 ? (esp_timer_get_time() - gyro_sample_us) / 1000.0 : -1.0);
                cJSON_AddBoolToObject(root, "gyro_turn_available", IsGyroTurnAvailable());
                cJSON_AddBoolToObject(root, "gyro_turn_pending", gyro_turn_pending_.load());
                cJSON_AddBoolToObject(root, "gyro_turn_active", gyro_turn_active_.load());
                cJSON_AddNumberToObject(root, "gyro_turn_target_deg", gyro_turn_target_deg_.load());
                cJSON_AddNumberToObject(root, "gyro_turn_progress_deg",
                                        gyro_turn_progress_deg_.load());
                cJSON_AddStringToObject(root, "gyro_turn_stop_reason",
                                        GyroTurnStopReasonName(gyro_turn_stop_reason_.load()));
                cJSON_AddStringToObject(root, "motion_gesture",
                                        MotionGestureName(motion_gesture_.load()));
#endif
#ifdef SECONDARY_OLED_I2C_ADDRESS
                cJSON_AddBoolToObject(root, "oled_available", secondary_oled_.IsAvailable());
                const SecondaryOled::Config oled_config = secondary_oled_.GetConfig();
                cJSON_AddBoolToObject(root, "oled_flipped", oled_config.flip_180);
                cJSON_AddNumberToObject(root, "oled_contrast", oled_config.contrast);
                cJSON_AddNumberToObject(root, "oled_page_count", secondary_oled_.GetPageCount());
                cJSON_AddStringToObject(root, "oled_brand", oled_config.brand.c_str());
                cJSON_AddStringToObject(root, "oled_distance_prefix",
                                        oled_config.distance_prefix.c_str());
                cJSON* oled_widgets = cJSON_AddArrayToObject(root, "oled_widgets");
                if (oled_widgets != nullptr) {
                    for (const auto& widget : oled_config.widgets) {
                        cJSON* item = cJSON_CreateObject();
                        if (item == nullptr) {
                            break;
                        }
                        cJSON_AddStringToObject(item, "type",
                                                SecondaryOledWidgetTypeName(widget.type));
                        cJSON_AddBoolToObject(item, "enabled", widget.enabled);
                        cJSON_AddNumberToObject(item, "size", static_cast<int>(widget.size));
                        cJSON_AddNumberToObject(item, "mode", widget.mode);
                        cJSON_AddItemToArray(oled_widgets, item);
                    }
                }
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
                if (web_control_server_ != nullptr) {
                    web_control_server_->AppendConversationStatus(root);
                    web_control_server_->AppendAsrStatus(root);
                }

                char* encoded = cJSON_PrintUnformatted(root);
                const std::string result = encoded != nullptr ? encoded : R"({"state":"unknown"})";
                cJSON_free(encoded);
                cJSON_Delete(root);
                return result;
            },
            std::move(snapshot_handler),
            [](const std::string& text, std::string& message) {
                return Application::GetInstance().SubmitTextChat(text, message);
            });
        Application::GetInstance().RegisterTextChatCallback(
            [this](const std::string& event, const std::string& text) {
                if (web_control_server_ != nullptr) {
                    web_control_server_->OnChatProbeEvent(event, text);
                }
            });
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
                    if (emotion_movement_active_.load(std::memory_order_relaxed)) {
                        motors_.Stop();
                    }
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
            "Supported emotions: neutral, happy, bored, laughing, funny, sad, angry, crying, "
            "loving, "
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
            "sensor, motion, power, and capacity dashboard returns afterward.",
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
                    cJSON_AddNumberToObject(result, "signed_current_ma",
                                            battery_signed_current_ma_.load());
                    cJSON_AddNumberToObject(result, "shunt_voltage_mv",
                                            battery_shunt_voltage_mv_.load());
                    cJSON_AddNumberToObject(result, "bus_voltage_v", battery_bus_voltage_v_.load());
                    cJSON_AddNumberToObject(result, "remaining_mah", battery_remaining_mah_.load());
                    cJSON_AddNumberToObject(result, "capacity_mah",
                                            BATTERY_SOC_USABLE_CAPACITY_MAH);
                    cJSON_AddStringToObject(result, "soc_method", "coulomb_quasi_rest_anchors");
                    cJSON_AddBoolToObject(result, "soc_tracking_degraded",
                                          battery_soc_tracking_degraded_.load());
                    cJSON_AddBoolToObject(result, "soc_quasi_resting",
                                          battery_soc_quasi_resting_.load());
                    cJSON_AddNumberToObject(result, "soc_voltage_reference_percent",
                                            battery_soc_voltage_reference_percent_.load());
                    cJSON_AddNumberToObject(result, "soc_voltage_correction_mah",
                                            battery_soc_voltage_correction_mah_.load());
                    cJSON_AddBoolToObject(result, "soc_full_anchored",
                                          battery_soc_full_anchored_.load());
                    cJSON_AddBoolToObject(result, "soc_bootstrap_voltage_rebased",
                                          battery_soc_bootstrap_voltage_rebased_.load());
                    cJSON_AddBoolToObject(result, "soc_empty_anchored",
                                          battery_soc_empty_anchored_.load());
                    cJSON_AddBoolToObject(result, "conversion_ready",
                                          battery_conversion_ready_.load());
                    cJSON_AddBoolToObject(result, "math_overflow", battery_math_overflow_.load());
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
        InitializeAudioSettings();
        InitializeLightingSettings();
        InitializeMotorSettings();
        InitializeMotorStatusLight();
        InitializeLiveCamera();
        InitializeInteractionTimers();
        InitializeTools();
#ifdef MPU6050_I2C_ADDRESS
        RegisterMotionTools();
        InitializeGyroTurnController();
#endif
        InitializeWebControl();
        ESP_LOGI(TAG, "Desk robot board initialized");
#ifdef AUXILIARY_I2C_SDA_PIN
        // Board construction runs inside Application::Initialize(). Queue only the lightweight
        // task creation here; Application::Run() executes it after initialization has returned.
        Application::GetInstance().Schedule([this]() { StartDeferredAuxiliaryInit(); });
#endif
    }

    AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                               AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK,
                                               AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK,
                                               AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
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
#if defined(BUILTIN_LED_LEDC_TIMER) && defined(BUILTIN_LED_LEDC_CHANNEL)
        static GpioLed led(BUILTIN_LED_GPIO, BUILTIN_LED_OUTPUT_INVERT, BUILTIN_LED_LEDC_TIMER,
                           BUILTIN_LED_LEDC_CHANNEL);
#else
        static GpioLed led(BUILTIN_LED_GPIO, true);
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
    void ApplyEmotion(const char* emotion) override {
        if (emotion != nullptr) {
            ApplyRobotEmotion(emotion, EmotionSource::kAssistant);
        }
    }
    Camera* GetCamera() override { return camera_; }
};

DECLARE_BOARD(DeskRobotBoard);
