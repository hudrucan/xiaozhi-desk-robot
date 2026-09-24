#include "wifi_board.h"

#include "application.h"
#include "assets/lang_config.h"
#include "button.h"
#include "camera/camera_settings.h"
#include "camera/desk_robot_camera.h"
#include "codecs/no_audio_codec.h"
#include "config/hardware_config.h"
#include "config/tuning.h"
#include "control/robot_controller.h"
#include "control/robot_settings.h"
#include "display/lcd_display.h"
#include "display/mochan_display.h"
#include "display/auto_brightness_policy.h"
#ifdef SECONDARY_OLED_I2C_ADDRESS
#include "display/secondary_display_controller.h"
#endif
#ifdef INA219_I2C_ADDRESS
#include "power/battery_controller.h"
#endif
#include "led/gpio_led.h"
#include "mcp/robot_mcp_tools.h"
#include "motion/expressive_motion_planner.h"
#include "motion/motor_controller.h"
#ifdef MPU6050_I2C_ADDRESS
#include "motion/gyro_turn_controller.h"
#include "motion/motion_reactions.h"
#include "sensors/mpu6050_motion_sensor.h"
#endif
#include "robot_web_control_server.h"
#include "sensors/environment_controller.h"
#include "sensors/shared_i2c_bus.h"
#include "system_info.h"
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
#include "sensors/cliff_sensor.h"
#endif

#include <driver/spi_common.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <wifi_manager.h>

#include <algorithm>
#include <atomic>
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

class DeskRobotBoard : public WifiBoard, public RobotController {
private:
    enum class EmotionSource : uint8_t { kAssistant, kPreview, kMpuReaction };

    static constexpr int kDefaultDriveDurationMs = 250;
    static constexpr uint32_t kLiveDriveLeaseMs = 350;
    static constexpr int64_t kEmotionMovementCooldownUs = 1500 * 1000LL;
    static constexpr uint8_t kWebControlMaxStartAttempts = 3;
    static constexpr int64_t kWebControlRetryDelayUs = 3 * 1000 * 1000LL;

    Button boot_button_;
    MochanDisplay* display_ = nullptr;
    DeskRobotCamera* camera_ = nullptr;
    MotorController motors_{MOTOR_LEFT_IN1, MOTOR_LEFT_IN2, MOTOR_RIGHT_IN1, MOTOR_RIGHT_IN2};
    ExpressiveMotionPlanner expressive_motion_planner_;
    RobotSettings robot_settings_;
    CameraSettingsStore camera_settings_;
    CameraImagePolicy camera_image_policy_;
    std::unique_ptr<RobotWebControlServer> web_control_server_;
    std::atomic_bool camera_flipped_{false};
    std::atomic_bool display_flipped_{false};
    std::atomic_int speaker_volume_{70};
    std::atomic_int microphone_gain_{1};
    std::atomic_int status_light_brightness_{STATUS_LIGHT_DEFAULT_BRIGHTNESS};
    std::atomic_int status_light_saved_brightness_{STATUS_LIGHT_DEFAULT_BRIGHTNESS};
    std::atomic_bool motor_activity_active_{false};
    std::atomic_int drive_duration_ms_{kDefaultDriveDurationMs};
    std::atomic_uint32_t live_drive_command_{100u | (100u << 8)};
    std::atomic_uint32_t live_drive_generation_{0};
    std::atomic_bool live_drive_update_scheduled_{false};
    std::atomic_bool emotion_movement_enabled_{false};
    std::atomic_bool emotion_movement_active_{false};
    int64_t last_emotion_movement_us_ = 0;
    TaskHandle_t live_camera_task_ = nullptr;
    esp_timer_handle_t face_reset_timer_ = nullptr;
    esp_timer_handle_t light_effect_reset_timer_ = nullptr;
    esp_timer_handle_t web_control_retry_timer_ = nullptr;
    uint8_t web_control_start_attempts_ = 0;
    std::mutex temporary_emotion_mutex_;
    std::string temporary_emotion_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    SharedI2cBus primary_i2c_{PRIMARY_I2C_PORT, PRIMARY_I2C_SDA_PIN, PRIMARY_I2C_SCL_PIN,
                              "primary"};
    EnvironmentController environment_controller_;
    AutoBrightnessPolicy auto_brightness_policy_;
#ifdef AUXILIARY_I2C_SDA_PIN
    SharedI2cBus auxiliary_i2c_{AUXILIARY_I2C_PORT, AUXILIARY_I2C_SDA_PIN,
                                AUXILIARY_I2C_SCL_PIN, "auxiliary"};
#endif
#ifdef INA219_I2C_ADDRESS
    BatteryController battery_controller_;
#endif
#ifdef MPU6050_I2C_ADDRESS
    Mpu6050MotionSensor motion_sensor_;
    GyroTurnController gyro_turn_controller_{motors_};
    MotionReactions motion_reactions_;
    std::atomic_bool motion_sensor_valid_{false};
    std::atomic_bool motion_emotions_enabled_{true};
    std::atomic<float> motion_roll_deg_{0.0f};
    std::atomic<float> motion_pitch_deg_{0.0f};
    std::atomic<float> motion_acceleration_g_{0.0f};
    std::atomic<float> motion_rotation_dps_{0.0f};
    std::atomic_bool press_reaction_pending_{false};
#endif
#if defined(INA219_I2C_ADDRESS) || defined(MPU6050_I2C_ADDRESS)
    TaskHandle_t auxiliary_sensor_task_ = nullptr;
#endif
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
    CliffSensor cliff_sensor_;
    std::atomic_bool cliff_retreat_pending_{false};
#endif
#ifdef SECONDARY_OLED_I2C_ADDRESS
    SecondaryDisplayController secondary_display_;
#endif

    static void InitializeDeferredPrimaryDevices(void* arg) {
        auto* self = static_cast<DeskRobotBoard*>(arg);
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
        self->InitializeDistanceSensor();
#endif
        if (!self->environment_controller_.Start(
                self->primary_i2c_.handle(), self->primary_i2c_.mutex(), self,
                &DeskRobotBoard::OnAmbientLight)) {
            ESP_LOGW(TAG, "Failed to start environment controller");
        }
    }

#ifdef AUXILIARY_I2C_SDA_PIN
    static void InitializeDeferredAuxiliaryDevices(void* arg) {
        auto* self = static_cast<DeskRobotBoard*>(arg);
        // Do not start any periodic I2C task until every device has completed its one-time setup.
        // This guarantees that SSD1306 panel creation cannot overlap a sensor read.
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
#endif

#ifdef INA219_I2C_ADDRESS
    void InitializePowerMonitor() {
        battery_controller_.Initialize(auxiliary_i2c_.handle(), auxiliary_i2c_.mutex());
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

    bool AreMotorsMoving() const {
        return motors_.IsMoving(MotorController::Direction::kForward) ||
               motors_.IsMoving(MotorController::Direction::kBackward) ||
               motors_.IsMoving(MotorController::Direction::kLeft) ||
               motors_.IsMoving(MotorController::Direction::kRight);
    }

    bool InitializeMotionSensor() {
        motion_emotions_enabled_.store(robot_settings_.GetMotionEmotionsEnabled());
        std::lock_guard<std::mutex> lock(auxiliary_i2c_.mutex());
        if (auxiliary_i2c_.handle() == nullptr ||
            !motion_sensor_.Initialize(auxiliary_i2c_.handle(), MPU6050_I2C_ADDRESS)) {
            ESP_LOGW(TAG, "MPU6050 not detected at 0x68 or 0x69");
            return false;
        }
        return true;
    }

#endif

#if defined(INA219_I2C_ADDRESS) || defined(MPU6050_I2C_ADDRESS)
    static void AuxiliarySensorTask(void* arg) {
        static_cast<DeskRobotBoard*>(arg)->RunAuxiliarySensorTask();
    }

    void RunAuxiliarySensorTask() {
        TickType_t last_wake_time = xTaskGetTickCount();
#ifdef MPU6050_I2C_ADDRESS
        constexpr int kCalibrationSamples = 50;
        int calibration_samples = 0;
        float calibration_roll_reference = 0.0f;
        float calibration_pitch_reference = 0.0f;
        float calibration_roll_sum = 0.0f;
        float calibration_pitch_sum = 0.0f;
        float roll_offset_deg = 0.0f;
        float pitch_offset_deg = 0.0f;
        unsigned motion_failures = 0;
#endif

        while (true) {
            const int64_t now_us = esp_timer_get_time();
#ifdef INA219_I2C_ADDRESS
            if (battery_controller_.Sample(now_us, !motors_.IsActive())) {
                const auto status = battery_controller_.GetStatus();
                Application::GetInstance().Schedule([this, status]() {
                    display_->SetBatteryStatus(status.percent, status.voltage_v, status.charging);
                });
            }
#endif

#ifdef MPU6050_I2C_ADDRESS
            if (motion_sensor_.IsAvailable()) {
                Mpu6050MotionSensor::Sample sample;
                bool read_ok = false;
                {
                    std::lock_guard<std::mutex> lock(auxiliary_i2c_.mutex());
                    read_ok = motion_sensor_.Read(sample);
                }
                if (!read_ok) {
                    motion_sensor_valid_.store(false);
                    gyro_turn_controller_.SetMotionSensorValid(false);
                    if (++motion_failures == 1 || motion_failures % 250 == 0) {
                        ESP_LOGW(TAG, "MPU6050 read failed (%u consecutive)", motion_failures);
                    }
                } else {
                    motion_failures = 0;
                    gyro_turn_controller_.ProcessSample(
                        sample, motor_activity_active_.load(std::memory_order_relaxed), now_us);
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
                            gyro_turn_controller_.SetMotionSensorValid(true);
                            motion_reactions_.SetCalibrated();
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
                        gyro_turn_controller_.SetMotionSensorValid(true);
                        const bool can_animate =
                            motion_emotions_enabled_.load() &&
                            Application::GetInstance().GetDeviceState() == kDeviceStateIdle &&
                            !motor_activity_active_.load(std::memory_order_relaxed);
                        bool face_busy = false;
                        {
                            std::lock_guard<std::mutex> lock(temporary_emotion_mutex_);
                            face_busy = !temporary_emotion_.empty();
                        }
                        const auto decision = motion_reactions_.Evaluate(
                            sample, roll, pitch, can_animate, face_busy, now_us);
                        if (decision.type != MotionReactions::DecisionType::kNone) {
                            bool accepted = decision.type == MotionReactions::DecisionType::kPress
                                                ? QueuePressReaction()
                                                : QueueTemporaryEmotion(
                                                      decision.emotion, decision.duration_ms,
                                                      EmotionSource::kMpuReaction);
                            bool decision_complete =
                                decision.type == MotionReactions::DecisionType::kEmotion || accepted;
                            if (!accepted && decision.type == MotionReactions::DecisionType::kPress &&
                                decision.emotion != nullptr) {
                                accepted = QueueTemporaryEmotion(
                                    decision.emotion, decision.duration_ms,
                                    EmotionSource::kMpuReaction);
                                decision_complete = true;
                            }
                            if (decision_complete) {
                                motion_reactions_.CompleteDecision(accepted, now_us);
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
            battery_controller_.IsAvailable() ||
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
    static bool IsFloorSafeForGyro(void* arg) {
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
        return static_cast<DeskRobotBoard*>(arg)->cliff_sensor_.IsFloorSafe();
#else
        return true;
#endif
    }

    bool StartGyroTurn(float target_deg, uint8_t intensity_percent, bool emotion_owned = false) {
        const bool started = gyro_turn_controller_.StartTurn(target_deg, intensity_percent);
        if (started) {
            emotion_movement_active_.store(emotion_owned, std::memory_order_relaxed);
        }
        return started;
    }

    void InitializeGyroTurnController() {
        gyro_turn_controller_.Initialize(this, &DeskRobotBoard::IsFloorSafeForGyro);
    }

    bool TryStartGyroEmotionTurn(const std::string& emotion) {
        float magnitude_deg = 0.0f;
        uint8_t intensity_percent = 0;
        if (!expressive_motion_planner_.GetEmotionTurn(emotion, magnitude_deg,
                                                       intensity_percent)) {
            return false;
        }
        return StartGyroTurn(magnitude_deg, intensity_percent, true);
    }
#endif

    void StartWebControl(bool reset_attempts) {
        if (reset_attempts) {
            web_control_start_attempts_ = 0;
            if (web_control_retry_timer_ != nullptr &&
                esp_timer_is_active(web_control_retry_timer_)) {
                esp_timer_stop(web_control_retry_timer_);
            }
        }
        if (!WifiManager::GetInstance().IsConnected()) {
            return;
        }
        if (web_control_server_ != nullptr && web_control_server_->Start(8080)) {
            web_control_start_attempts_ = 0;
            ESP_LOGI(TAG, "Local control: http://%s:8080",
                     WifiManager::GetInstance().GetIpAddress().c_str());
            return;
        }

        ++web_control_start_attempts_;
        if (web_control_start_attempts_ >= kWebControlMaxStartAttempts ||
            web_control_retry_timer_ == nullptr) {
            ESP_LOGE(TAG, "Local control unavailable after %u start attempts",
                     static_cast<unsigned>(web_control_start_attempts_));
            return;
        }
        const esp_err_t result =
            esp_timer_start_once(web_control_retry_timer_, kWebControlRetryDelayUs);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Failed to schedule local control retry: %s", esp_err_to_name(result));
            return;
        }
        ESP_LOGW(TAG, "Local control start failed; retrying attempt %u of %u",
                 static_cast<unsigned>(web_control_start_attempts_ + 1),
                 static_cast<unsigned>(kWebControlMaxStartAttempts));
    }

    void OnNetworkEvent(NetworkEvent event, const std::string& data = "") override {
        WifiBoard::OnNetworkEvent(event, data);

        switch (event) {
            case NetworkEvent::Scanning:
#ifdef SECONDARY_OLED_I2C_ADDRESS
                secondary_display_.SetNetworkState(SecondaryOled::NetworkState::kScanning);
#endif
                display_->SetWifiConnected(false);
                display_->ShowBootSplash();
                break;
            case NetworkEvent::Connecting:
#ifdef SECONDARY_OLED_I2C_ADDRESS
                secondary_display_.SetNetworkState(SecondaryOled::NetworkState::kConnecting);
#endif
                display_->SetWifiConnected(false);
                display_->ShowBootSplash();
                break;
            case NetworkEvent::Disconnected:
#ifdef SECONDARY_OLED_I2C_ADDRESS
                secondary_display_.SetNetworkState(SecondaryOled::NetworkState::kDisconnected);
#endif
                display_->SetWifiConnected(false);
                display_->ShowBootSplash();
                break;
            case NetworkEvent::Connected:
#ifdef SECONDARY_OLED_I2C_ADDRESS
                secondary_display_.SetNetworkState(SecondaryOled::NetworkState::kConnected);
#endif
                display_->SetWifiConnected(true);
                display_->HideBootSplash();
                Application::GetInstance().Schedule([this]() { StartWebControl(true); });
                break;
            case NetworkEvent::WifiConfigModeEnter:
#ifdef SECONDARY_OLED_I2C_ADDRESS
                secondary_display_.SetNetworkState(SecondaryOled::NetworkState::kConfigMode);
#endif
                display_->SetWifiConnected(false);
                display_->HideBootSplash();
                break;
            case NetworkEvent::WifiConfigModeExit:
#ifdef SECONDARY_OLED_I2C_ADDRESS
                secondary_display_.SetNetworkState(SecondaryOled::NetworkState::kConnecting);
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
        const bool display_flipped = robot_settings_.GetDisplayFlipped();
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
        const bool legacy_flipped = robot_settings_.GetCameraFlipped();
        camera_settings_.Load(legacy_flipped);
        const CameraSettingsConfig camera_settings = camera_settings_.Get();
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
        // Reuse the primary new-driver I2C bus instead of creating another SCCB owner.
        config.pin_sccb_sda = GPIO_NUM_NC;
        config.pin_sccb_scl = GPIO_NUM_NC;
        config.sccb_i2c_port = PRIMARY_I2C_PORT;
        config.pin_pwdn = CAMERA_PIN_PWDN;
        config.pin_reset = CAMERA_PIN_RESET;
        config.xclk_freq_hz = CAMERA_XCLK_FREQ_HZ;
        config.ledc_timer = LEDC_TIMER_0;
        config.ledc_channel = LEDC_CHANNEL_0;
        config.pixel_format = PIXFORMAT_JPEG;
        // Allocate framebuffers for the sensor's maximum mode once. Runtime
        // consumers then switch down to their persisted Web/Mochan/MCP profile
        // without reallocating or reinitializing the shared camera/I2C bus.
        config.frame_size = FRAMESIZE_UXGA;
        config.jpeg_quality = 12;
        config.fb_count = 2;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_LATEST;
        camera_ = new DeskRobotCamera(config, primary_i2c_.mutex(), camera_settings,
                                      camera_image_policy_);

        const bool flipped = camera_settings.sensor.mirror && camera_settings.sensor.flip;
        camera_flipped_.store(flipped);
    }

#ifdef DISTANCE_SENSOR_I2C_ADDRESS
    static void OnCliffDetected(void* arg) {
        auto* self = static_cast<DeskRobotBoard*>(arg);
        const bool was_moving_forward =
            self->motors_.IsMoving(MotorController::Direction::kForward);
        const bool was_moving_unsafe =
            was_moving_forward || self->motors_.IsMoving(MotorController::Direction::kLeft) ||
            self->motors_.IsMoving(MotorController::Direction::kRight);
        if (was_moving_unsafe) {
            self->motors_.EmergencyStop();
            if (was_moving_forward) {
                self->QueueCliffRetreat();
            }
        }
    }

    bool IsCliffDetected() const { return cliff_sensor_.IsCliffDetected(); }

    bool IsDirectionBlockedByCliff(MotorController::Direction direction) const {
        return cliff_sensor_.IsDirectionBlocked(direction);
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
        const int edge_mm = robot_settings_.GetCliffEdgeMm();
        cliff_sensor_.SetEdgeMm(edge_mm);
        ESP_LOGI(TAG, "Cliff threshold set to %d mm", edge_mm);
    }

    void QueueCliffThreshold(int edge_mm) {
        const int safe_edge_mm = std::clamp(edge_mm, 50, 500);
        cliff_sensor_.SetEdgeMm(safe_edge_mm);
        Application::GetInstance().Schedule([this, safe_edge_mm]() {
            robot_settings_.SetCliffEdgeMm(safe_edge_mm);
        });
    }

    void InitializeDistanceSensor() {
        cliff_sensor_.Initialize(primary_i2c_.handle(), primary_i2c_.mutex(),
                                 robot_settings_.GetCliffEdgeMm(), this,
                                 &DeskRobotBoard::OnCliffDetected);
    }
#endif

#ifdef SECONDARY_OLED_I2C_ADDRESS
    static void CollectSecondaryOledTelemetry(void* arg, SecondaryOled::Telemetry& telemetry) {
        auto* self = static_cast<DeskRobotBoard*>(arg);
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
        const auto cliff = self->cliff_sensor_.GetStatus();
        telemetry.distance_mm = cliff.distance_mm;
        telemetry.distance_valid = cliff.valid;
        telemetry.cliff_detected = cliff.cliff_detected;
#endif
#ifdef INA219_I2C_ADDRESS
        const auto battery = self->battery_controller_.GetStatus();
        telemetry.power_valid = battery.valid;
        telemetry.current_ma = static_cast<int>(std::lround(battery.current_ma));
        telemetry.power_mw = static_cast<int>(std::lround(battery.power_mw));
        telemetry.capacity_active = battery.capacity_test_active;
        telemetry.capacity_measuring = battery.capacity_test_measuring;
        telemetry.capacity_uah = battery.capacity_test_uah;
        telemetry.capacity_seconds = battery.capacity_test_seconds;
        telemetry.battery_percent = battery.percent;
        telemetry.battery_remaining_mah = static_cast<uint32_t>(
            std::lround(std::max(0.0f, battery.remaining_mah)));
        telemetry.battery_voltage_mv =
            static_cast<int>(std::lround(battery.voltage_v * 1000.0f));
        telemetry.low_battery = telemetry.power_valid && !battery.charging &&
                                telemetry.battery_percent >= 0 &&
                                telemetry.battery_percent < 20;
#endif
#ifdef MPU6050_I2C_ADDRESS
        const auto gyro = self->gyro_turn_controller_.GetStatus();
        telemetry.motion_valid = self->motion_sensor_valid_.load(std::memory_order_relaxed);
        telemetry.motion_state =
            MotionReactions::GestureName(self->motion_reactions_.GetGesture());
        telemetry.roll_deg =
            static_cast<int>(std::lround(self->motion_roll_deg_.load(std::memory_order_relaxed)));
        telemetry.pitch_deg =
            static_cast<int>(std::lround(self->motion_pitch_deg_.load(std::memory_order_relaxed)));
        telemetry.motion_calibrating =
            self->motion_sensor_.IsAvailable() && (!telemetry.motion_valid || !gyro.bias_valid);
        telemetry.gyro_turn_pending = gyro.pending;
        telemetry.gyro_turn_active = gyro.active;
        telemetry.gyro_turn_target_deg = static_cast<int>(std::lround(gyro.target_deg));
        telemetry.gyro_turn_progress_deg = static_cast<int>(std::lround(gyro.progress_deg));
        telemetry.gyro_turn_intensity_percent = gyro.intensity_percent;
#endif
        const auto environment = self->environment_controller_.GetStatus();
        telemetry.temperature_valid = environment.temperature_valid;
        telemetry.temperature_tenths_c =
            static_cast<int>(std::lround(environment.temperature_c * 10.0f));
        telemetry.humidity_valid = environment.humidity_valid;
        telemetry.humidity_tenths_percent =
            static_cast<int>(std::lround(environment.humidity_percent * 10.0f));
        telemetry.pressure_valid = environment.pressure_valid;
        telemetry.pressure_tenths_hpa =
            static_cast<int>(std::lround(environment.pressure_hpa * 10.0f));
        telemetry.illuminance_valid = environment.illuminance_valid;
        telemetry.illuminance_lux =
            static_cast<int>(std::lround(environment.illuminance_lux));
        telemetry.light_level = LightLevelName(environment.light_level);
    }

    void InitializeSecondaryOled() {
        secondary_display_.Initialize(auxiliary_i2c_.handle(), auxiliary_i2c_.mutex(), this,
                                      &DeskRobotBoard::CollectSecondaryOledTelemetry);
    }
#endif

    void ApplyCameraFlip(bool flipped) {
        CameraSettingsConfig settings = camera_settings_.Get();
        settings.sensor.mirror = flipped;
        settings.sensor.flip = flipped;
        if (!ApplyCameraSettings(settings)) {
            camera_flipped_.store(!flipped);
        }
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
        robot_settings_.SetDisplayFlipped(flipped);
    }

    bool QueueDisplayFlip() {
        const bool flipped = !display_flipped_.load();
        display_flipped_.store(flipped);
        Application::GetInstance().Schedule([this, flipped]() { ApplyDisplayFlip(flipped); });
        return flipped;
    }

    bool ToggleStatusLight() override {
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


    void MaybeStartEmotionMovement(const std::string& emotion, EmotionSource source) {
        if (!emotion_movement_enabled_.load(std::memory_order_relaxed) ||
            source == EmotionSource::kMpuReaction || motors_.IsActive()) {
            return;
        }
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
        // Automatic movement is conservative: unlike manual reverse, it requires a valid floor
        // sample and never starts while a cliff is active.
        if (!cliff_sensor_.IsFloorSafe()) {
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
        auto movements = expressive_motion_planner_.BuildEmotionMovement(emotion);
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
        if (normalized.empty()) {
            return false;
        }
        return secondary_display_.QueueTemporaryText(normalized, duration_ms);
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
        if (camera_ != nullptr) {
            camera_->ForceOff();
        }
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
        auto movements = expressive_motion_planner_.BuildDance();
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
        if (!cliff_sensor_.IsFloorSafe()) {
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
            floor_safe = cliff_sensor_.IsFloorSafe();
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
        const int speaker_volume = robot_settings_.GetSpeakerVolume();
        const int microphone_gain = robot_settings_.GetMicrophoneGain();
        speaker_volume_.store(speaker_volume);
        microphone_gain_.store(microphone_gain);
        GetAudioCodec()->SetInputGain(static_cast<float>(microphone_gain));
        if (robot_settings_.GetMicrophoneMuted()) {
            Application::GetInstance().SetMicrophoneMuted(true);
        }
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
        const int brightness = robot_settings_.GetStatusLightBrightness();
        status_light_brightness_.store(brightness);
        if (brightness > 0) {
            status_light_saved_brightness_.store(brightness);
        }
        ApplyStatusLightBrightness(brightness);
        AutoBrightnessPolicy::Config auto_config = {
            .enabled = robot_settings_.GetAutoBrightnessEnabled(),
            .minimum_percent = robot_settings_.GetAutoBrightnessMinimum(),
            .maximum_percent = robot_settings_.GetAutoBrightnessMaximum(),
        };
        if (auto_config.maximum_percent < auto_config.minimum_percent) {
            auto_config.maximum_percent = auto_config.minimum_percent;
        }
        const int screen_brightness =
            GetBacklight() != nullptr ? GetBacklight()->brightness() : 75;
        auto_brightness_policy_.Configure(auto_config, screen_brightness,
                                          esp_timer_get_time());
    }

    static void OnAmbientLight(void* arg, bool valid, float illuminance_lux,
                               int64_t timestamp_us) {
        auto* self = static_cast<DeskRobotBoard*>(arg);
        self->camera_image_policy_.UpdateAmbientLight(valid, illuminance_lux, timestamp_us);
#ifdef SECONDARY_OLED_I2C_ADDRESS
        self->secondary_display_.UpdateAmbientLight(valid, illuminance_lux, timestamp_us);
#endif
        if (!valid) {
            self->auto_brightness_policy_.ResetReading();
            return;
        }
        const auto target =
            self->auto_brightness_policy_.Update(illuminance_lux, timestamp_us);
        if (!target.has_value()) {
            return;
        }
        Application::GetInstance().Schedule([self, brightness = *target]() {
            if (self->auto_brightness_policy_.GetConfig().enabled &&
                self->GetBacklight() != nullptr) {
                self->GetBacklight()->SetBrightness(static_cast<uint8_t>(brightness), false);
            }
        });
    }

    void ApplyAutoBrightnessConfig(const AutoBrightnessPolicy::Config& requested) {
        AutoBrightnessPolicy::Config config = requested;
        config.minimum_percent = std::clamp(config.minimum_percent, 10, 100);
        config.maximum_percent = std::clamp(config.maximum_percent, config.minimum_percent, 100);
        const int current_brightness =
            GetBacklight() != nullptr ? GetBacklight()->brightness() : 75;
        auto_brightness_policy_.Configure(config, current_brightness, esp_timer_get_time());
        robot_settings_.SetAutoBrightnessEnabled(config.enabled);
        robot_settings_.SetAutoBrightnessMinimum(config.minimum_percent);
        robot_settings_.SetAutoBrightnessMaximum(config.maximum_percent);
        if (!config.enabled && GetBacklight() != nullptr) {
            GetBacklight()->SetBrightness(
                static_cast<uint8_t>(robot_settings_.GetManualScreenBrightness()), false);
        }
    }

    void InitializeMotorStatusLight() {
        motors_.SetMovementStateCallback([this](bool moving) {
            motor_activity_active_.store(moving, std::memory_order_relaxed);
            if (!moving) {
                emotion_movement_active_.store(false, std::memory_order_relaxed);
#ifdef MPU6050_I2C_ADDRESS
                gyro_turn_controller_.Cancel();
#endif
            }
#ifdef BUILTIN_LED_STATUS_PROFILE_EDISON
            Application::GetInstance().Schedule(
                [this, moving]() { static_cast<GpioLed*>(GetLed())->SetActivityOverride(moving); });
#endif
        });
    }

    void InitializeMotorSettings() {
        const int speed = robot_settings_.GetMotorSpeed();
        const int drive_duration = robot_settings_.GetDriveDurationMs();
        motors_.SetSpeedPercent(speed);
        drive_duration_ms_.store(drive_duration, std::memory_order_relaxed);
        emotion_movement_enabled_.store(robot_settings_.GetEmotionMovementEnabled(),
                                        std::memory_order_relaxed);
        ESP_LOGI(TAG, "Motor speed %d%%, drive time %d ms, emotion movement %s", speed,
                 drive_duration, emotion_movement_enabled_.load() ? "enabled" : "disabled");
    }

    void QueueMotorSpeed(int speed) {
        const int safe_speed =
            std::clamp(speed, MotorController::kMinSpeedPercent, MotorController::kMaxSpeedPercent);
        Application::GetInstance().Schedule([this, safe_speed]() {
            motors_.SetSpeedPercent(safe_speed);
            robot_settings_.SetMotorSpeed(safe_speed);
        });
    }

    void QueueDriveDuration(int duration_ms) {
        const int safe_duration = std::clamp(duration_ms, 50, 2000);
        drive_duration_ms_.store(safe_duration, std::memory_order_relaxed);
        Application::GetInstance().Schedule([this, safe_duration]() {
            robot_settings_.SetDriveDurationMs(safe_duration);
        });
    }

    void QueueEmotionMovementEnabled(bool enabled) {
        emotion_movement_enabled_.store(enabled, std::memory_order_relaxed);
        Application::GetInstance().Schedule([this, enabled]() {
            if (!enabled && emotion_movement_active_.load(std::memory_order_relaxed)) {
                motors_.Stop();
            }
            robot_settings_.SetEmotionMovementEnabled(enabled);
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
            robot_settings_.SetMicrophoneGain(safe_gain);
        });
    }

    void QueueScreenBrightness(int brightness) {
        const int safe_brightness = std::clamp(brightness, 10, 100);
        Application::GetInstance().Schedule([this, safe_brightness]() {
            AutoBrightnessPolicy::Config config = auto_brightness_policy_.GetConfig();
            config.enabled = false;
            auto_brightness_policy_.Configure(config, safe_brightness, esp_timer_get_time());
            robot_settings_.SetAutoBrightnessEnabled(false);
            if (GetBacklight() != nullptr) {
                GetBacklight()->SetBrightness(static_cast<uint8_t>(safe_brightness), true);
            }
        });
    }

    void QueueAutoBrightnessEnabled(bool enabled) {
        Application::GetInstance().Schedule([this, enabled]() {
            AutoBrightnessPolicy::Config config = auto_brightness_policy_.GetConfig();
            config.enabled = enabled;
            ApplyAutoBrightnessConfig(config);
        });
    }

    void QueueAutoBrightnessMinimum(int brightness) {
        const int safe_brightness = std::clamp(brightness, 10, 100);
        Application::GetInstance().Schedule([this, safe_brightness]() {
            AutoBrightnessPolicy::Config config = auto_brightness_policy_.GetConfig();
            config.minimum_percent = safe_brightness;
            config.maximum_percent = std::max(config.maximum_percent, safe_brightness);
            ApplyAutoBrightnessConfig(config);
        });
    }

    void QueueAutoBrightnessMaximum(int brightness) {
        const int safe_brightness = std::clamp(brightness, 10, 100);
        Application::GetInstance().Schedule([this, safe_brightness]() {
            AutoBrightnessPolicy::Config config = auto_brightness_policy_.GetConfig();
            config.maximum_percent = safe_brightness;
            config.minimum_percent = std::min(config.minimum_percent, safe_brightness);
            ApplyAutoBrightnessConfig(config);
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
            robot_settings_.SetStatusLightBrightness(safe_brightness);
        });
    }

    static void LiveCameraTask(void* arg) {
        static_cast<DeskRobotBoard*>(arg)->RunLiveCameraTask();
    }

    void RunLiveCameraTask() {
        while (true) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            while (camera_->IsMochanPreviewActive()) {
                if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                    camera_->CapturePreview();
                    vTaskDelay(pdMS_TO_TICKS(250));
                } else {
                    // Live preview is intentionally one-shot per idle session.
                    // Starting a conversation turns the mode off; returning to
                    // idle requires an explicit toggle from the local UI.
                    camera_->ForceOff();
                }
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

    bool ToggleLiveCamera() override {
        if (live_camera_task_ == nullptr) {
            return false;
        }
        if (camera_->IsMochanPreviewActive()) {
            camera_->StopMochanPreview();
            xTaskNotifyGive(live_camera_task_);
            return false;
        }
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle ||
            !camera_->StartMochanPreview()) {
            return false;
        }
        xTaskNotifyGive(live_camera_task_);
        return true;
    }

    bool StartWebCameraStream() override {
        return camera_ != nullptr &&
               Application::GetInstance().GetDeviceState() == kDeviceStateIdle &&
               camera_->StartWebLive();
    }

    bool IsWebCameraStreamEnabled() const override {
        return camera_ != nullptr && camera_->IsWebLiveActive();
    }

    bool SendWebCameraFrame(const SnapshotSender& sender) override {
        return camera_ != nullptr &&
               Application::GetInstance().GetDeviceState() == kDeviceStateIdle &&
               camera_->SendWebLiveFrame(sender);
    }

    int GetWebCameraFrameIntervalMs() const override {
        return camera_ != nullptr ? camera_->WebFrameIntervalMs() : 200;
    }

    void StopWebCameraStream() override {
        if (camera_ != nullptr) {
            camera_->StopWebLive();
        }
    }

    bool Move(MotorController::Direction direction, int duration_ms, MovePolicy policy) override {
        const int safe_duration = std::clamp(duration_ms, 50, 2000);
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
        if (IsDirectionBlockedByCliff(direction)) {
            return false;
        }
#endif
        Application::GetInstance().Schedule([this, direction, safe_duration, policy]() {
            if (policy == MovePolicy::kReplaceCurrent ||
                emotion_movement_active_.load(std::memory_order_relaxed)) {
                motors_.Stop();
            }
            motors_.Drive(direction, static_cast<uint32_t>(safe_duration));
        });
        return true;
    }

    bool SetLiveDrive(int left_percent, int right_percent) override {
        const int safe_left = std::clamp(left_percent, -100, 100);
        const int safe_right = std::clamp(right_percent, -100, 100);
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
        if (IsCliffDetected() && (safe_left > 0 || safe_right > 0)) {
            return false;
        }
#endif
        const uint32_t packed = static_cast<uint32_t>(safe_left + 100) |
                                (static_cast<uint32_t>(safe_right + 100) << 8);
        live_drive_command_.store(packed, std::memory_order_relaxed);
        live_drive_generation_.fetch_add(1, std::memory_order_release);
        if (!live_drive_update_scheduled_.exchange(true, std::memory_order_acq_rel)) {
            Application::GetInstance().Schedule([this]() { ApplyPendingLiveDrive(); });
        }
        return true;
    }

    void ApplyPendingLiveDrive() {
        const uint32_t generation = live_drive_generation_.load(std::memory_order_acquire);
        const uint32_t packed = live_drive_command_.load(std::memory_order_acquire);
        const int left = static_cast<int>(packed & 0xff) - 100;
        const int right = static_cast<int>((packed >> 8) & 0xff) - 100;
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
        if (IsCliffDetected() && (left > 0 || right > 0)) {
            motors_.Stop();
        } else {
            motors_.DriveWheels(left, right, kLiveDriveLeaseMs);
        }
#else
        motors_.DriveWheels(left, right, kLiveDriveLeaseMs);
#endif

        live_drive_update_scheduled_.store(false, std::memory_order_release);
        if (live_drive_generation_.load(std::memory_order_acquire) != generation &&
            !live_drive_update_scheduled_.exchange(true, std::memory_order_acq_rel)) {
            Application::GetInstance().Schedule([this]() { ApplyPendingLiveDrive(); });
        }
    }

    void Stop() override {
        SetLiveDrive(0, 0);
    }

    bool Dance() override { return QueueDance(); }

    bool TurnRelative(int degrees, std::string& message) override {
#ifdef MPU6050_I2C_ADDRESS
        return gyro_turn_controller_.RequestTurn(degrees, message);
#else
        message = "Gyro turn unavailable: MPU6050 is not configured";
        return false;
#endif
    }

    bool ShowEmotion(const std::string& emotion, int duration_ms) override {
        return QueueTemporaryEmotion(emotion, duration_ms);
    }

    bool ShowSecondaryText(const std::string& text, int duration_ms) override {
#ifdef SECONDARY_OLED_I2C_ADDRESS
        return QueueTemporaryOledText(text, duration_ms);
#else
        return false;
#endif
    }

    bool SetStatusLightEffect(const std::string& effect, int duration_ms) override {
        return QueueStatusLightEffect(effect, duration_ms);
    }

    bool ToggleCameraFlip() override { return QueueCameraFlip(); }
    bool ToggleDisplayFlip() override { return QueueDisplayFlip(); }

    bool SendSnapshot(const SnapshotSender& sender) override {
        return Application::GetInstance().GetDeviceState() == kDeviceStateIdle &&
               camera_ != nullptr && camera_->SendSnapshot(sender);
    }

    CameraSettingsConfig GetCameraSettings() const override {
        return camera_settings_.Get();
    }

    bool ApplyCameraSettings(const CameraSettingsConfig& requested) override {
        if (camera_ == nullptr) {
            return false;
        }
        const CameraSettingsConfig settings = CameraSettingsStore::Normalize(requested);
        const DeskRobotCamera::PreviewMode previous_mode = camera_->preview_mode();
        if (!camera_->ApplySettings(settings)) {
            return false;
        }
        camera_settings_.Save(settings);
        const bool flipped = settings.sensor.mirror && settings.sensor.flip;
        camera_flipped_.store(flipped);
        // Keep the legacy orientation key as a downgrade/migration fallback.
        robot_settings_.SetCameraFlipped(flipped);

        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
            return true;
        }
        // Web settings are applied through the HTTP route, which owns the
        // stream-client stop/restart handshake. Do not revive a headless Web
        // mode after its MJPEG client has already disconnected.
        if (previous_mode == DeskRobotCamera::PreviewMode::kWebLive) {
            return true;
        }
        if (previous_mode == DeskRobotCamera::PreviewMode::kMochanPreview) {
            const bool started = camera_->StartMochanPreview();
            if (started && live_camera_task_ != nullptr) {
                xTaskNotifyGive(live_camera_task_);
            }
            return started;
        }
        return true;
    }

    bool ResetCameraSettings() override {
        return ApplyCameraSettings(CameraSettingsStore::Defaults(false));
    }

    std::string GetCameraSensorName() const override {
        return camera_ != nullptr ? camera_->SensorName() : "Unavailable";
    }

    SecondaryOled::Config GetSecondaryDisplayConfig() const override {
#ifdef SECONDARY_OLED_I2C_ADDRESS
        return secondary_display_.GetConfig();
#else
        return {};
#endif
    }

    void SetSecondaryDisplayConfig(const SecondaryOled::Config& config) override {
#ifdef SECONDARY_OLED_I2C_ADDRESS
        secondary_display_.QueueConfig(config);
#else
        (void)config;
#endif
    }

    void SetSpeakerVolume(int volume) override { QueueSpeakerVolume(volume); }
    void SetMicrophoneGain(int gain) override { QueueMicrophoneGain(gain); }
    void SetMicrophoneMuted(bool muted) override {
        Application::GetInstance().SetMicrophoneMuted(muted);
        Application::GetInstance().Schedule(
            [this, muted]() { robot_settings_.SetMicrophoneMuted(muted); });
    }
    void SetScreenBrightness(int brightness) override { QueueScreenBrightness(brightness); }
    void SetAutoBrightnessEnabled(bool enabled) override {
        QueueAutoBrightnessEnabled(enabled);
    }
    void SetAutoBrightnessMinimum(int brightness) override {
        QueueAutoBrightnessMinimum(brightness);
    }
    void SetAutoBrightnessMaximum(int brightness) override {
        QueueAutoBrightnessMaximum(brightness);
    }
    void SetMotorSpeed(int speed) override { QueueMotorSpeed(speed); }
    void SetDriveDuration(int duration_ms) override { QueueDriveDuration(duration_ms); }
    void SetEmotionMovementEnabled(bool enabled) override {
        QueueEmotionMovementEnabled(enabled);
    }
    void SetStatusLightBrightness(int brightness) override {
        QueueStatusLightBrightness(brightness);
    }
    void SetCliffThreshold(int edge_mm) override {
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
        QueueCliffThreshold(edge_mm);
#else
        (void)edge_mm;
#endif
    }
    void SetMotionEmotionsEnabled(bool enabled) override {
#ifdef MPU6050_I2C_ADDRESS
        motion_emotions_enabled_.store(enabled);
        Application::GetInstance().Schedule(
            [this, enabled]() { robot_settings_.SetMotionEmotionsEnabled(enabled); });
#else
        (void)enabled;
#endif
    }

    bool StartBatteryCapacityTest() override {
#ifdef INA219_I2C_ADDRESS
        return battery_controller_.StartCapacityTest();
#else
        return false;
#endif
    }
    void StopBatteryCapacityTest() override {
#ifdef INA219_I2C_ADDRESS
        battery_controller_.StopCapacityTest();
#endif
    }
    void ResetBatteryCapacityTest() override {
#ifdef INA219_I2C_ADDRESS
        battery_controller_.ResetCapacityTest();
#endif
    }

    void ToggleWake() override { Application::GetInstance().ToggleChatState(); }

    bool PlayAudioTest() override {
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
            return false;
        }
        Application::GetInstance().Schedule(
            []() { Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP); });
        return true;
    }

    void ReturnToIdleState() override { ReturnToIdle(); }
    bool Reboot() override { return QueueReboot(); }
    void EnterWifiSetup() override { EnterWifiConfigMode(); }

    RobotStatus GetStatus() override {
        RobotStatus status;
        auto& app = Application::GetInstance();
        const char* state = DeviceStateMachine::GetStateName(app.GetDeviceState());
        status.state = state != nullptr ? state : "unknown";
        status.server_status_phase = app.GetServerStatusPhase();
        status.asr_ready = app.IsAsrReady();
        status.asr_preparing = app.IsGeminiAsrPreparing();
        status.camera_available = camera_ != nullptr && camera_->IsAvailable();
        status.camera_flipped = camera_flipped_.load();
        status.display_flipped = display_flipped_.load();
        status.emotion = display_->GetCurrentEmotion();
        status.speaker_volume = speaker_volume_.load();
        status.microphone_gain = microphone_gain_.load();
        status.microphone_muted = app.IsMicrophoneMuted();
        auto& audio_service = app.GetAudioService();
        status.microphone_level = status.microphone_muted ? 0 : audio_service.GetInputLevel();
        status.microphone_clipping = !status.microphone_muted && audio_service.IsInputClipping();
        status.screen_brightness = GetBacklight() != nullptr ? GetBacklight()->brightness() : 0;
        const AutoBrightnessPolicy::Config auto_brightness =
            auto_brightness_policy_.GetConfig();
        status.auto_brightness_enabled = auto_brightness.enabled;
        status.auto_brightness_minimum = auto_brightness.minimum_percent;
        status.auto_brightness_maximum = auto_brightness.maximum_percent;
        status.status_light_brightness = status_light_brightness_.load();
        status.live_camera_available = live_camera_task_ != nullptr;
        status.live_camera = camera_ != nullptr && camera_->IsMochanPreviewActive();
        status.web_camera_live = camera_ != nullptr && camera_->IsWebLiveActive();
        status.camera_sensor = camera_ != nullptr ? camera_->SensorName() : "Unavailable";
        status.camera_mode = camera_ != nullptr && camera_->IsMcpOperationActive()
                                 ? "mcp"
                                 : status.web_camera_live
                                       ? "web"
                                       : status.live_camera ? "mochan" : "off";
        switch (camera_settings_.Get().sensor.profile) {
            case CameraImageProfile::kLowLight:
                status.camera_profile = "low_light";
                break;
            case CameraImageProfile::kCustom:
                status.camera_profile = "custom";
                break;
            case CameraImageProfile::kAuto:
                status.camera_profile = "auto";
                break;
            default:
                status.camera_profile = "normal";
                break;
        }
        if (camera_ != nullptr) {
            const CameraImagePolicy::Status policy = camera_->GetImagePolicyStatus();
            status.camera_auto_profile_available = policy.ambient_light_available;
            status.camera_effective_profile =
                policy.effective_profile == CameraImageProfile::kLowLight
                    ? "low_light"
                    : "normal";
            const DeskRobotCamera::McpRequestHealth camera_health =
                camera_->GetMcpRequestHealth();
            status.camera_request_state =
                DeskRobotCamera::McpRequestStateName(camera_health.state);
            status.camera_capture_ms = camera_health.capture_ms;
            status.camera_vision_ms = camera_health.vision_ms;
            status.camera_image_bytes = camera_health.image_bytes;
            status.camera_response_bytes = camera_health.response_bytes;
            if (camera_health.started_us > 0) {
                status.camera_request_age_sec =
                    std::max<int64_t>(0, (esp_timer_get_time() - camera_health.started_us) /
                                             1000000);
            }
        }
        status.motor_speed = motors_.GetSpeedPercent();
        status.drive_duration_ms = drive_duration_ms_.load(std::memory_order_relaxed);
        status.emotion_movement_enabled =
            emotion_movement_enabled_.load(std::memory_order_relaxed);
        status.emotion_movement_active = emotion_movement_active_.load(std::memory_order_relaxed);
        status.motors = motors_.GetStatus();
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
        status.cliff = cliff_sensor_.GetStatus();
#endif
#ifdef INA219_I2C_ADDRESS
        status.battery = battery_controller_.GetStatus();
#endif
        status.environment = environment_controller_.GetStatus();
#ifdef MPU6050_I2C_ADDRESS
        status.motion_sensor_available = motion_sensor_.IsAvailable();
        status.motion_sensor_valid = motion_sensor_valid_.load();
        status.motion_emotions_enabled = motion_emotions_enabled_.load();
        status.motion_roll_deg = motion_roll_deg_.load();
        status.motion_pitch_deg = motion_pitch_deg_.load();
        status.motion_acceleration_g = motion_acceleration_g_.load();
        status.motion_rotation_dps = motion_rotation_dps_.load();
        status.motion_gesture = MotionReactions::GestureName(motion_reactions_.GetGesture());
        status.gyro = gyro_turn_controller_.GetStatus();
#endif
#ifdef SECONDARY_OLED_I2C_ADDRESS
        status.oled_available = secondary_display_.IsAvailable();
        status.oled_config = secondary_display_.GetConfig();
        status.oled_effective_contrast = secondary_display_.GetEffectiveContrast();
        status.oled_auto_contrast_available =
            secondary_display_.IsAmbientLightAvailable();
        status.oled_page_count = secondary_display_.GetPageCount();
#endif
        const esp_app_desc_t* app_desc = esp_app_get_description();
        status.version = app_desc != nullptr ? app_desc->version : "unknown";
        status.ip = WifiManager::GetInstance().GetIpAddress();
        wifi_ap_record_t access_point = {};
        if (esp_wifi_sta_get_ap_info(&access_point) == ESP_OK) {
            const size_t ssid_length =
                strnlen(reinterpret_cast<const char*>(access_point.ssid), sizeof(access_point.ssid));
            status.ssid.assign(reinterpret_cast<const char*>(access_point.ssid), ssid_length);
            status.rssi = access_point.rssi;
        }
        status.uptime_sec = esp_timer_get_time() / 1000000;
        status.reset_reason = SystemInfo::GetResetReasonName(esp_reset_reason());
        status.server_transport = app.GetServerTransport();
        status.server_connected = app.IsServerConnected();
        status.free_internal_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        status.minimum_free_internal_bytes =
            heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        status.free_psram_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        return status;
    }

    void InitializeWebControl() {
        web_control_server_ = std::make_unique<RobotWebControlServer>(
            static_cast<RobotController&>(*this),
            [](const std::string& text, std::string& message) {
                return Application::GetInstance().SubmitTextChat(text, message);
            });
        Application::GetInstance().RegisterTextChatCallback(
            [this](const std::string& event, const std::string& text) {
                if (web_control_server_ != nullptr) {
                    web_control_server_->OnChatProbeEvent(event, text);
                }
            });
        esp_timer_create_args_t retry_args = {
            .callback =
                [](void* arg) {
                    auto* self = static_cast<DeskRobotBoard*>(arg);
                    Application::GetInstance().Schedule(
                        [self]() { self->StartWebControl(false); });
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "web_control_retry",
            .skip_unhandled_events = true,
        };
        const esp_err_t result =
            esp_timer_create(&retry_args, &web_control_retry_timer_);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create local control retry timer: %s",
                     esp_err_to_name(result));
        }
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

public:
    DeskRobotBoard() : boot_button_(BOOT_BUTTON_GPIO, BUTTON_ACTIVE_HIGH, 3000) {
        // The web dashboard is not reachable until Wi-Fi comes up, so begin
        // buffering here to retain display, camera, and audio initialization logs.
        RobotWebControlServer::BeginLogCapture();
        const esp_reset_reason_t reset_reason = esp_reset_reason();
        ESP_LOGI(TAG, "Boot reset reason: %s (%d)", SystemInfo::GetResetReasonName(reset_reason),
                 static_cast<int>(reset_reason));
        InitializeSpi();
        InitializeDisplay();
        // Bring up the panel backlight before camera/audio initialization. A
        // peripheral failure later in boot must not leave the display looking
        // completely unpowered and hide the splash or diagnostic state.
        if (GetBacklight() != nullptr) {
            GetBacklight()->RestoreBrightness();
        }
        InitializeButtons();
        ESP_ERROR_CHECK(primary_i2c_.Initialize() ? ESP_OK : ESP_FAIL);
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
        InitializeCliffSettings();
#endif
        InitializeCamera();
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
        motors_.SetMotionGuard([this](MotorController::Direction direction) {
            return !IsDirectionBlockedByCliff(direction);
        });
        motors_.SetLiveMotionGuard([this](int left_percent, int right_percent) {
            return !IsCliffDetected() || (left_percent <= 0 && right_percent <= 0);
        });
#endif
        InitializeAudioSettings();
        InitializeLightingSettings();
        InitializeMotorSettings();
        InitializeMotorStatusLight();
        InitializeLiveCamera();
        InitializeInteractionTimers();
        RobotMcpTools::Register(*this);
#ifdef MPU6050_I2C_ADDRESS
        InitializeGyroTurnController();
#endif
        InitializeWebControl();
        ESP_LOGI(TAG, "Desk robot board initialized");
        // Camera owns the first synchronous use of primary I2C. Defer the remaining primary-bus
        // sensors until board/audio initialization has returned to the application loop.
        Application::GetInstance().Schedule([this]() {
            primary_i2c_.StartDeferredInitialization(
                this, &DeskRobotBoard::InitializeDeferredPrimaryDevices);
        });
#ifdef AUXILIARY_I2C_SDA_PIN
        // Board construction runs inside Application::Initialize(). Queue only the lightweight
        // task creation here; Application::Run() executes it after initialization has returned.
        Application::GetInstance().Schedule([this]() {
            auxiliary_i2c_.StartDeferredInitialization(
                this, &DeskRobotBoard::InitializeDeferredAuxiliaryDevices);
        });
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
        const auto battery = battery_controller_.GetStatus();
        if (!battery.valid) {
            return false;
        }
        level = battery.percent;
        charging = battery.charging;
        discharging = battery.discharging;
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
