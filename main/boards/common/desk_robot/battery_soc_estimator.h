#pragma once

#include <cstdint>

class BatterySocEstimator {
public:
    // Version 1 may contain charge integrated with the pre-rewire current polarity.
    static constexpr int kPersistenceVersion = 2;

    struct PersistedState {
        int version = 0;
        int32_t remaining_uah = 0;
        int32_t usable_capacity_uah = 0;
        int32_t soc_basis_points = 0;
        int32_t last_voltage_mv = 0;
        bool tracking_degraded = false;
    };

    BatterySocEstimator(float usable_capacity_mah, int64_t maximum_integration_gap_us);

    bool Restore(const PersistedState& state);
    void SeedFromVoltage(float battery_voltage_v);
    void Update(float battery_voltage_v, float current_ma, int64_t now_us);
    void MarkMeasurementGap();

    bool IsInitialized() const { return initialized_; }
    float GetSocPercent() const;
    float GetRemainingMah() const { return remaining_mah_; }
    float GetCapacityMah() const { return usable_capacity_mah_; }
    float GetLastVoltageV() const { return last_voltage_v_; }
    bool IsTrackingDegraded() const { return tracking_degraded_; }
    PersistedState GetPersistedState() const;

    static int EstimatePercentFromVoltage(float voltage_v);

private:
    void ResetIntegrationBaseline();

    float usable_capacity_mah_;
    float remaining_mah_ = 0.0f;
    float previous_current_ma_ = 0.0f;
    float last_voltage_v_ = 0.0f;
    int64_t maximum_integration_gap_us_;
    int64_t last_update_us_ = 0;
    bool initialized_ = false;
    bool have_previous_current_ = false;
    bool tracking_degraded_ = false;
};
