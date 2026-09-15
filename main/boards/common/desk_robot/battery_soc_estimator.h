#pragma once

#include <cstdint>

class BatterySocEstimator {
public:
    // Version 1 may contain charge integrated with the pre-rewire current polarity.
    static constexpr int kPersistenceVersion = 2;

    struct Config {
        float usable_capacity_mah;
        int64_t maximum_integration_gap_us;
        float quasi_rest_max_current_ma;
        float quasi_rest_current_stddev_ma;
        float quasi_rest_voltage_stddev_v;
        float quasi_rest_current_transition_ma;
        float quasi_rest_voltage_transition_v;
        int64_t quasi_rest_qualification_us;
        int64_t quasi_rest_correction_interval_us;
        int64_t quasi_rest_correction_time_constant_us;
        float full_anchor_min_voltage_v;
        float full_anchor_taper_current_ma;
        int64_t full_anchor_qualification_us;
        float empty_anchor_max_voltage_v;
        int64_t empty_anchor_qualification_us;
    };

    struct PersistedState {
        int version = 0;
        int32_t remaining_uah = 0;
        int32_t usable_capacity_uah = 0;
        int32_t soc_basis_points = 0;
        int32_t last_voltage_mv = 0;
        bool tracking_degraded = false;
    };

    explicit BatterySocEstimator(const Config& config);

    bool Restore(const PersistedState& state);
    void SeedFromVoltage(float battery_voltage_v);
    void Update(float battery_voltage_v, float current_ma, bool motors_idle, bool charging,
                int64_t now_us);
    void MarkMeasurementGap();

    bool IsInitialized() const { return initialized_; }
    float GetSocPercent() const;
    float GetRemainingMah() const { return remaining_mah_; }
    float GetCapacityMah() const { return usable_capacity_mah_; }
    float GetLastVoltageV() const { return last_voltage_v_; }
    bool IsTrackingDegraded() const { return tracking_degraded_; }
    bool IsQuasiResting() const { return quasi_resting_; }
    float GetQuasiRestVoltageSocPercent() const { return quasi_rest_voltage_soc_percent_; }
    float GetCumulativeVoltageCorrectionMah() const { return cumulative_voltage_correction_mah_; }
    bool IsFullAnchored() const { return full_anchored_; }
    bool IsEmptyAnchored() const { return empty_anchored_; }
    PersistedState GetPersistedState() const;

    static float EstimatePercentFromVoltage(float voltage_v);

private:
    void ResetIntegrationBaseline();
    void ResetQuasiRestQualification();
    void UpdateQuasiRest(float battery_voltage_v, float current_ma, bool motors_idle, bool charging,
                         int64_t now_us, int64_t elapsed_us);
    void ResetFullAnchorCandidate();
    void UpdateFullAnchor(float battery_voltage_v, float current_ma, bool motors_idle,
                          bool charging, int64_t now_us);
    void ResetEmptyAnchorCandidate();
    void UpdateEmptyAnchor(float battery_voltage_v, float current_ma, bool motors_idle,
                           bool discharging, int64_t now_us);

    float usable_capacity_mah_;
    float remaining_mah_ = 0.0f;
    float previous_current_ma_ = 0.0f;
    float last_voltage_v_ = 0.0f;
    int64_t maximum_integration_gap_us_;
    int64_t last_update_us_ = 0;
    bool initialized_ = false;
    bool have_previous_current_ = false;
    bool tracking_degraded_ = false;
    float quasi_rest_max_current_ma_;
    float quasi_rest_current_stddev_ma_;
    float quasi_rest_voltage_stddev_v_;
    float quasi_rest_current_transition_ma_;
    float quasi_rest_voltage_transition_v_;
    int64_t quasi_rest_qualification_us_;
    int64_t quasi_rest_correction_interval_us_;
    int64_t quasi_rest_correction_time_constant_us_;
    float full_anchor_min_voltage_v_;
    float full_anchor_taper_current_ma_;
    int64_t full_anchor_qualification_us_;
    float empty_anchor_max_voltage_v_;
    int64_t empty_anchor_qualification_us_;
    int64_t quasi_rest_correction_elapsed_us_ = 0;
    int64_t quasi_rest_started_us_ = 0;
    uint32_t quasi_rest_sample_count_ = 0;
    double quasi_rest_current_mean_ma_ = 0.0;
    double quasi_rest_current_m2_ = 0.0;
    double quasi_rest_voltage_mean_v_ = 0.0;
    double quasi_rest_voltage_m2_ = 0.0;
    float quasi_rest_previous_current_ma_ = 0.0f;
    float quasi_rest_previous_voltage_v_ = 0.0f;
    bool have_quasi_rest_previous_sample_ = false;
    bool quasi_resting_ = false;
    float quasi_rest_voltage_soc_percent_ = 0.0f;
    float cumulative_voltage_correction_mah_ = 0.0f;
    int64_t full_anchor_candidate_started_us_ = 0;
    bool full_anchor_charge_seen_ = false;
    bool full_anchored_ = false;
    int64_t empty_anchor_candidate_started_us_ = 0;
    bool empty_anchored_ = false;
};
