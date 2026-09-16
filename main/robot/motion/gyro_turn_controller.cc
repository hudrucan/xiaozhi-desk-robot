#include "gyro_turn_controller.h"

#include "application.h"
#include "config/hardware_config.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>
#include <cmath>

#define TAG "GyroTurnController"

namespace {
constexpr int kBiasSamples = 50;
constexpr float kBiasYawStabilityDps = 1.5f;
constexpr int64_t kSampleStaleUs = 160 * 1000LL;
constexpr float kTurnToleranceDeg = 2.5f;
constexpr uint32_t kMaximumTimeoutMs = 10000;
}  // namespace

GyroTurnController::GyroTurnController(MotorController& motors) : motors_(motors) {}

bool GyroTurnController::Initialize(void* floor_context, FloorSafeProvider floor_safe_provider) {
    floor_context_ = floor_context;
    floor_safe_provider_ = floor_safe_provider;
    if (xTaskCreate(TaskEntry, "gyro_turn", 4096, this, 2, &task_) != pdPASS) {
        task_ = nullptr;
        ESP_LOGE(TAG, "Failed to create gyro turn controller task");
        return false;
    }
    return true;
}

float GyroTurnController::SelectYawRate(const Mpu6050MotionSensor::Sample& sample) {
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

void GyroTurnController::ProcessSample(const Mpu6050MotionSensor::Sample& sample, bool motors_active,
                                       int64_t now_us) {
    const float raw_yaw_rate_dps = SelectYawRate(sample);
    bool yaw_stable = true;
    if (have_previous_bias_sample_) {
        yaw_stable =
            std::fabs(raw_yaw_rate_dps - previous_bias_yaw_rate_dps_) <= kBiasYawStabilityDps;
    }
    previous_bias_yaw_rate_dps_ = raw_yaw_rate_dps;
    have_previous_bias_sample_ = true;

    const bool gyro_still = !motors_active && yaw_stable && sample.acceleration_magnitude_g > 0.85f &&
                            sample.acceleration_magnitude_g < 1.15f;
    if (!bias_valid_.load(std::memory_order_relaxed)) {
        if (gyro_still) {
            bias_sum_ += raw_yaw_rate_dps;
            ++bias_samples_;
            if (bias_samples_ >= kBiasSamples) {
                const float bias = bias_sum_ / bias_samples_;
                yaw_bias_dps_.store(bias, std::memory_order_relaxed);
                bias_valid_.store(true, std::memory_order_release);
                ESP_LOGI(TAG, "MPU6050 yaw bias calibrated: %.3f dps (%d samples)", bias,
                         bias_samples_);
            }
        } else {
            bias_samples_ = 0;
            bias_sum_ = 0.0f;
        }
    }
    if (bias_valid_.load(std::memory_order_acquire)) {
        yaw_rate_dps_.store(raw_yaw_rate_dps - yaw_bias_dps_.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
        sample_timestamp_us_.store(now_us, std::memory_order_release);
    }
}

bool GyroTurnController::IsAvailable() const {
    const int64_t sample_us = sample_timestamp_us_.load(std::memory_order_acquire);
    return task_ != nullptr && motion_sensor_valid_.load(std::memory_order_relaxed) &&
           bias_valid_.load(std::memory_order_acquire) && sample_us > 0 &&
           esp_timer_get_time() - sample_us <= kSampleStaleUs;
}

bool GyroTurnController::IsFloorSafe() const {
    return floor_safe_provider_ == nullptr || floor_safe_provider_(floor_context_);
}

bool GyroTurnController::RequestTurn(int target_degrees, std::string& message) {
    if (target_degrees < -180 || target_degrees > 180 || std::abs(target_degrees) < 3) {
        message = "Gyro turn angle must be between 3 and 180 degrees";
        return false;
    }
    if (!IsAvailable()) {
        message = "Gyro turn unavailable: MPU6050 is not ready";
        return false;
    }
    if (motors_.IsActive()) {
        message = "Gyro turn blocked: motors are busy";
        return false;
    }
    if (!IsFloorSafe()) {
        message = "Gyro turn blocked: no safe floor detected";
        return false;
    }
    if (pending_.exchange(true, std::memory_order_acq_rel)) {
        message = "Gyro turn blocked: another turn is pending";
        return false;
    }

    target_deg_.store(static_cast<float>(target_degrees), std::memory_order_relaxed);
    progress_deg_.store(0.0f, std::memory_order_relaxed);
    stop_reason_.store(StopReason::kNone, std::memory_order_relaxed);
    Application::GetInstance().Schedule([this, target_degrees]() {
        if (!StartTurn(static_cast<float>(target_degrees), 100)) {
            stop_reason_.store(StopReason::kRejected, std::memory_order_relaxed);
            ESP_LOGW(TAG, "Relative turn %d deg was rejected", target_degrees);
        }
        pending_.store(false, std::memory_order_release);
    });
    const int magnitude = target_degrees < 0 ? -target_degrees : target_degrees;
    message = std::string("Turning ") + (target_degrees < 0 ? "left " : "right ") +
              std::to_string(magnitude) + " degrees";
    return true;
}

bool GyroTurnController::StartTurn(float target_deg, uint8_t intensity_percent) {
    if (!IsAvailable() || active_.load(std::memory_order_relaxed) || motors_.IsActive() ||
        std::fabs(target_deg) < kTurnToleranceDeg || !IsFloorSafe()) {
        return false;
    }
    const uint32_t timeout_ms = std::min<uint32_t>(
        kMaximumTimeoutMs, static_cast<uint32_t>(500.0f + std::fabs(target_deg) * 55.0f));
    const auto direction = target_deg > 0.0f ? MotorController::Direction::kRight
                                             : MotorController::Direction::kLeft;
    if (!motors_.Drive(direction, timeout_ms, intensity_percent)) {
        return false;
    }
    target_deg_.store(target_deg, std::memory_order_relaxed);
    progress_deg_.store(0.0f, std::memory_order_relaxed);
    intensity_percent_.store(intensity_percent, std::memory_order_relaxed);
    timeout_ms_.store(timeout_ms, std::memory_order_relaxed);
    stop_reason_.store(StopReason::kActive, std::memory_order_relaxed);
    active_.store(true, std::memory_order_release);
    xTaskNotifyGive(task_);
    return true;
}

void GyroTurnController::Cancel() {
    if (active_.exchange(false, std::memory_order_acq_rel)) {
        stop_reason_.store(StopReason::kCancelled, std::memory_order_relaxed);
    }
}

void GyroTurnController::TaskEntry(void* arg) {
    static_cast<GyroTurnController*>(arg)->RunTask();
}

void GyroTurnController::RunTask() {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!active_.load(std::memory_order_acquire)) {
            continue;
        }
        const float target_deg = target_deg_.load(std::memory_order_relaxed);
        const uint8_t maximum_intensity = intensity_percent_.load(std::memory_order_relaxed);
        const int64_t started_us = esp_timer_get_time();
        const int64_t deadline_us =
            started_us + static_cast<int64_t>(timeout_ms_.load()) * 1000;
        int64_t previous_sample_us = 0;
        float turned_deg = 0.0f;
        uint8_t applied_intensity = maximum_intensity;
        StopReason stop_reason = StopReason::kNone;

        while (active_.load(std::memory_order_acquire)) {
            const int64_t now_us = esp_timer_get_time();
            const int64_t sample_us = sample_timestamp_us_.load(std::memory_order_acquire);
            if (now_us >= deadline_us) {
                stop_reason = StopReason::kTimeout;
                break;
            }
            if (!motion_sensor_valid_.load(std::memory_order_relaxed) ||
                !bias_valid_.load(std::memory_order_acquire) || sample_us <= 0 ||
                now_us - sample_us > kSampleStaleUs) {
                stop_reason = StopReason::kStaleSensor;
                break;
            }
            if (sample_us != previous_sample_us) {
                if (previous_sample_us > 0) {
                    const float dt_seconds =
                        std::clamp((sample_us - previous_sample_us) / 1000000.0f, 0.0f, 0.1f);
                    turned_deg += yaw_rate_dps_.load(std::memory_order_relaxed) * dt_seconds;
                    progress_deg_.store(turned_deg, std::memory_order_relaxed);
                }
                previous_sample_us = sample_us;
                const float remaining_deg = target_deg - turned_deg;
                if (std::fabs(remaining_deg) <= kTurnToleranceDeg ||
                    (target_deg > 0.0f && turned_deg > target_deg) ||
                    (target_deg < 0.0f && turned_deg < target_deg)) {
                    stop_reason = StopReason::kTargetReached;
                    break;
                }
                if (target_deg * turned_deg < 0.0f && std::fabs(turned_deg) > 2.0f) {
                    stop_reason = StopReason::kDirectionMismatch;
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

        if (stop_reason != StopReason::kNone && active_.exchange(false)) {
            progress_deg_.store(turned_deg, std::memory_order_relaxed);
            stop_reason_.store(stop_reason, std::memory_order_relaxed);
            ESP_LOGI(TAG, "Gyro turn %.1f/%.1f deg stopped: %s", turned_deg, target_deg,
                     StopReasonName(stop_reason));
            motors_.EmergencyStop();
        }
    }
}

GyroTurnController::Status GyroTurnController::GetStatus() const {
    return Status{
        .available = IsAvailable(),
        .bias_valid = bias_valid_.load(),
        .pending = pending_.load(),
        .active = active_.load(),
        .target_deg = target_deg_.load(),
        .progress_deg = progress_deg_.load(),
        .stop_reason = stop_reason_.load(),
        .yaw_rate_dps = yaw_rate_dps_.load(),
        .yaw_bias_dps = yaw_bias_dps_.load(),
        .sample_timestamp_us = sample_timestamp_us_.load(),
        .intensity_percent = intensity_percent_.load(),
    };
}

const char* GyroTurnController::StopReasonName(StopReason reason) {
    switch (reason) {
        case StopReason::kNone:
            return "none";
        case StopReason::kActive:
            return "active";
        case StopReason::kTargetReached:
            return "target_reached";
        case StopReason::kTimeout:
            return "timeout";
        case StopReason::kStaleSensor:
            return "stale_sensor";
        case StopReason::kDirectionMismatch:
            return "direction_mismatch";
        case StopReason::kCancelled:
            return "cancelled";
        case StopReason::kRejected:
            return "rejected";
    }
    return "none";
}
