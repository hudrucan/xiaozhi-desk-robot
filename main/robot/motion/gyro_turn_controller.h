#pragma once

#include "motor_controller.h"
#include "sensors/mpu6050_motion_sensor.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <string>

class GyroTurnController {
public:
    enum class StopReason : uint8_t {
        kNone,
        kActive,
        kTargetReached,
        kTimeout,
        kStaleSensor,
        kDirectionMismatch,
        kCancelled,
        kRejected,
    };

    struct Status {
        bool available = false;
        bool bias_valid = false;
        bool pending = false;
        bool active = false;
        float target_deg = 0.0f;
        float progress_deg = 0.0f;
        StopReason stop_reason = StopReason::kNone;
        float yaw_rate_dps = 0.0f;
        float yaw_bias_dps = 0.0f;
        int64_t sample_timestamp_us = 0;
        uint8_t intensity_percent = 80;
    };

    using FloorSafeProvider = bool (*)(void* context);

    explicit GyroTurnController(MotorController& motors);
    bool Initialize(void* floor_context, FloorSafeProvider floor_safe_provider);
    void ProcessSample(const Mpu6050MotionSensor::Sample& sample, bool motors_active,
                       int64_t now_us);
    void SetMotionSensorValid(bool valid) { motion_sensor_valid_.store(valid); }
    bool RequestTurn(int target_degrees, std::string& message);
    bool StartTurn(float target_deg, uint8_t intensity_percent);
    void Cancel();
    bool IsAvailable() const;
    Status GetStatus() const;

    static const char* StopReasonName(StopReason reason);

private:
    static void TaskEntry(void* arg);
    static float SelectYawRate(const Mpu6050MotionSensor::Sample& sample);
    void RunTask();
    bool IsFloorSafe() const;

    MotorController& motors_;
    void* floor_context_ = nullptr;
    FloorSafeProvider floor_safe_provider_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::atomic_bool motion_sensor_valid_{false};
    std::atomic<float> yaw_rate_dps_{0.0f};
    std::atomic<float> yaw_bias_dps_{0.0f};
    std::atomic<int64_t> sample_timestamp_us_{0};
    std::atomic_bool bias_valid_{false};
    int bias_samples_ = 0;
    float bias_sum_ = 0.0f;
    float previous_bias_yaw_rate_dps_ = 0.0f;
    bool have_previous_bias_sample_ = false;
    std::atomic_bool pending_{false};
    std::atomic_bool active_{false};
    std::atomic<float> target_deg_{0.0f};
    std::atomic<float> progress_deg_{0.0f};
    std::atomic<StopReason> stop_reason_{StopReason::kNone};
    std::atomic<uint8_t> intensity_percent_{80};
    std::atomic<uint32_t> timeout_ms_{0};
};
