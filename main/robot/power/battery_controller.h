#pragma once

#include "battery_soc_estimator.h"
#include "ina219_power_monitor.h"

#include <driver/i2c_master.h>

#include <atomic>
#include <cstdint>
#include <mutex>

class BatteryController {
public:
    struct Status {
        bool available = false;
        bool valid = false;
        int percent = -1;
        float voltage_v = 0.0f;
        float current_ma = 0.0f;
        float power_mw = 0.0f;
        float signed_current_ma = 0.0f;
        float shunt_voltage_mv = 0.0f;
        float bus_voltage_v = 0.0f;
        float remaining_mah = 0.0f;
        bool conversion_ready = false;
        bool math_overflow = false;
        bool soc_tracking_degraded = false;
        bool soc_quasi_resting = false;
        float soc_voltage_reference_percent = 0.0f;
        float soc_voltage_correction_mah = 0.0f;
        bool soc_full_anchored = false;
        bool soc_bootstrap_voltage_rebased = false;
        bool soc_empty_anchored = false;
        bool charging = false;
        bool discharging = false;
        bool capacity_test_active = false;
        bool capacity_test_measuring = false;
        uint32_t capacity_test_uah = 0;
        uint32_t capacity_test_seconds = 0;
    };

    BatteryController();

    void Initialize(i2c_master_bus_handle_t bus, std::mutex& bus_mutex);
    bool Sample(int64_t now_us, bool motors_idle);
    bool IsAvailable() const { return power_monitor_.IsAvailable(); }
    Status GetStatus() const;

    bool StartCapacityTest();
    void StopCapacityTest();
    void ResetCapacityTest();

private:
    void InitializeSamplingState(int64_t now_us);
    void PersistCapacityTest();
    void PersistSoc(const char* reason);
    void UpdateCapacityTest(const Ina219PowerMonitor::Reading& reading, int64_t now_us);
    void MarkInvalidMeasurement(bool read_ok, const Ina219PowerMonitor::Reading& reading);

    Ina219PowerMonitor power_monitor_;
    BatterySocEstimator soc_estimator_;
    std::mutex* bus_mutex_ = nullptr;

    bool sampling_state_initialized_ = false;
    int64_t next_sample_us_ = 0;
    bool filter_initialized_ = false;
    float filtered_voltage_v_ = 0.0f;
    float filtered_current_ma_ = 0.0f;
    float filtered_power_mw_ = 0.0f;
    unsigned read_failures_ = 0;
    unsigned invalid_samples_ = 0;
    int64_t next_display_us_ = 0;
    int64_t soc_last_save_us_ = 0;
    float soc_last_saved_remaining_mah_ = 0.0f;
    bool soc_last_saved_degraded_ = false;
    bool capacity_test_was_active_ = false;
    bool capacity_previous_sample_valid_ = false;
    int64_t capacity_previous_sample_us_ = 0;
    float capacity_previous_current_ma_ = 0.0f;
    int64_t capacity_last_save_us_ = 0;
    double capacity_fractional_uah_ = 0.0;
    int64_t capacity_fractional_time_us_ = 0;
    int64_t capacity_low_voltage_started_us_ = 0;

    std::atomic_bool valid_{false};
    std::atomic_int percent_{-1};
    std::atomic<float> voltage_v_{0.0f};
    std::atomic<float> current_ma_{0.0f};
    std::atomic<float> power_mw_{0.0f};
    std::atomic<float> signed_current_ma_{0.0f};
    std::atomic<float> shunt_voltage_mv_{0.0f};
    std::atomic<float> bus_voltage_v_{0.0f};
    std::atomic<float> remaining_mah_{0.0f};
    std::atomic_bool conversion_ready_{false};
    std::atomic_bool math_overflow_{false};
    std::atomic_bool soc_tracking_degraded_{false};
    std::atomic_bool soc_quasi_resting_{false};
    std::atomic<float> soc_voltage_reference_percent_{0.0f};
    std::atomic<float> soc_voltage_correction_mah_{0.0f};
    std::atomic_bool soc_full_anchored_{false};
    std::atomic_bool soc_bootstrap_voltage_rebased_{false};
    std::atomic_bool soc_empty_anchored_{false};
    std::atomic_bool charging_{false};
    std::atomic_bool discharging_{false};
    std::atomic_bool capacity_test_active_{false};
    std::atomic_bool capacity_test_measuring_{false};
    std::atomic<uint32_t> capacity_test_uah_{0};
    std::atomic<uint32_t> capacity_test_seconds_{0};
};
