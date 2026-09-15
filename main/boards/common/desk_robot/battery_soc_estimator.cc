#include "battery_soc_estimator.h"

#include <algorithm>
#include <array>
#include <cmath>

BatterySocEstimator::BatterySocEstimator(const Config& config)
    : usable_capacity_mah_(config.usable_capacity_mah),
      maximum_integration_gap_us_(config.maximum_integration_gap_us),
      quasi_rest_max_current_ma_(config.quasi_rest_max_current_ma),
      quasi_rest_current_stddev_ma_(config.quasi_rest_current_stddev_ma),
      quasi_rest_voltage_stddev_v_(config.quasi_rest_voltage_stddev_v),
      quasi_rest_current_transition_ma_(config.quasi_rest_current_transition_ma),
      quasi_rest_voltage_transition_v_(config.quasi_rest_voltage_transition_v),
      quasi_rest_qualification_us_(config.quasi_rest_qualification_us),
      quasi_rest_correction_interval_us_(config.quasi_rest_correction_interval_us),
      quasi_rest_correction_time_constant_us_(config.quasi_rest_correction_time_constant_us),
      full_anchor_min_voltage_v_(config.full_anchor_min_voltage_v),
      full_anchor_taper_current_ma_(config.full_anchor_taper_current_ma),
      full_anchor_qualification_us_(config.full_anchor_qualification_us),
      empty_anchor_max_voltage_v_(config.empty_anchor_max_voltage_v),
      empty_anchor_qualification_us_(config.empty_anchor_qualification_us) {}

bool BatterySocEstimator::Restore(const PersistedState& state) {
    const int32_t configured_capacity_uah =
        static_cast<int32_t>(std::lround(usable_capacity_mah_ * 1000.0f));
    if (state.version != kPersistenceVersion ||
        state.usable_capacity_uah != configured_capacity_uah || state.remaining_uah < 0 ||
        state.remaining_uah > state.usable_capacity_uah || state.soc_basis_points < 0 ||
        state.soc_basis_points > 10000 || state.last_voltage_mv < 0 ||
        state.last_voltage_mv > 6000) {
        return false;
    }

    const float remaining_mah = state.remaining_uah / 1000.0f;
    const float calculated_soc = remaining_mah * 100.0f / usable_capacity_mah_;
    if (std::fabs(calculated_soc * 100.0f - state.soc_basis_points) > 100.0f) {
        return false;
    }

    remaining_mah_ = remaining_mah;
    last_voltage_v_ = state.last_voltage_mv / 1000.0f;
    tracking_degraded_ = state.tracking_degraded;
    initialized_ = true;
    ResetIntegrationBaseline();
    ResetQuasiRestQualification();
    ResetFullAnchorCandidate();
    full_anchor_charge_seen_ = false;
    full_anchored_ = false;
    ResetEmptyAnchorCandidate();
    empty_anchored_ = false;
    return true;
}

void BatterySocEstimator::SeedFromVoltage(float battery_voltage_v) {
    const float percent = EstimatePercentFromVoltage(battery_voltage_v);
    remaining_mah_ = usable_capacity_mah_ * percent / 100.0f;
    last_voltage_v_ = battery_voltage_v;
    tracking_degraded_ = false;
    initialized_ = true;
    ResetIntegrationBaseline();
    ResetQuasiRestQualification();
    ResetFullAnchorCandidate();
    full_anchor_charge_seen_ = false;
    full_anchored_ = false;
    ResetEmptyAnchorCandidate();
    empty_anchored_ = false;
}

void BatterySocEstimator::Update(float battery_voltage_v, float current_ma, bool motors_idle,
                                 bool charging, int64_t now_us) {
    if (!initialized_ || !std::isfinite(battery_voltage_v) || !std::isfinite(current_ma) ||
        now_us <= 0) {
        return;
    }

    last_voltage_v_ = battery_voltage_v;
    const bool discharging = !charging && current_ma > 20.0f;
    if (!have_previous_current_) {
        previous_current_ma_ = current_ma;
        last_update_us_ = now_us;
        have_previous_current_ = true;
        UpdateFullAnchor(battery_voltage_v, current_ma, motors_idle, charging, now_us);
        UpdateEmptyAnchor(battery_voltage_v, current_ma, motors_idle, discharging, now_us);
        UpdateQuasiRest(battery_voltage_v, current_ma, motors_idle, charging, now_us, 0);
        return;
    }

    const int64_t elapsed_us = now_us - last_update_us_;
    if (elapsed_us <= 0 || elapsed_us > maximum_integration_gap_us_) {
        previous_current_ma_ = current_ma;
        last_update_us_ = now_us;
        tracking_degraded_ = true;
        ResetQuasiRestQualification();
        ResetFullAnchorCandidate();
        UpdateFullAnchor(battery_voltage_v, current_ma, motors_idle, charging, now_us);
        ResetEmptyAnchorCandidate();
        UpdateEmptyAnchor(battery_voltage_v, current_ma, motors_idle, discharging, now_us);
        UpdateQuasiRest(battery_voltage_v, current_ma, motors_idle, charging, now_us, 0);
        return;
    }

    const double average_current_ma =
        0.5 * (static_cast<double>(previous_current_ma_) + current_ma);
    const double delta_mah = average_current_ma * elapsed_us / 3600000000.0;
    remaining_mah_ = static_cast<float>(std::clamp(static_cast<double>(remaining_mah_) - delta_mah,
                                                   0.0, static_cast<double>(usable_capacity_mah_)));
    previous_current_ma_ = current_ma;
    last_update_us_ = now_us;
    UpdateFullAnchor(battery_voltage_v, current_ma, motors_idle, charging, now_us);
    UpdateEmptyAnchor(battery_voltage_v, current_ma, motors_idle, discharging, now_us);
    UpdateQuasiRest(battery_voltage_v, current_ma, motors_idle, charging, now_us, elapsed_us);
}

void BatterySocEstimator::MarkMeasurementGap() {
    if (!initialized_) {
        return;
    }
    tracking_degraded_ = true;
    ResetIntegrationBaseline();
    ResetQuasiRestQualification();
    ResetFullAnchorCandidate();
    ResetEmptyAnchorCandidate();
}

float BatterySocEstimator::GetSocPercent() const {
    if (!initialized_ || usable_capacity_mah_ <= 0.0f) {
        return 0.0f;
    }
    return std::clamp(remaining_mah_ * 100.0f / usable_capacity_mah_, 0.0f, 100.0f);
}

BatterySocEstimator::PersistedState BatterySocEstimator::GetPersistedState() const {
    PersistedState state;
    state.version = kPersistenceVersion;
    state.remaining_uah = static_cast<int32_t>(std::lround(remaining_mah_ * 1000.0f));
    state.usable_capacity_uah = static_cast<int32_t>(std::lround(usable_capacity_mah_ * 1000.0f));
    state.soc_basis_points = static_cast<int32_t>(std::lround(GetSocPercent() * 100.0f));
    state.last_voltage_mv = static_cast<int32_t>(std::lround(last_voltage_v_ * 1000.0f));
    state.tracking_degraded = tracking_degraded_;
    return state;
}

float BatterySocEstimator::EstimatePercentFromVoltage(float voltage_v) {
    struct Point {
        float voltage;
        int percent;
    };
    constexpr std::array<Point, 10> curve = {{{3.20f, 0},
                                              {3.45f, 5},
                                              {3.55f, 10},
                                              {3.65f, 20},
                                              {3.75f, 40},
                                              {3.82f, 60},
                                              {3.90f, 75},
                                              {4.00f, 90},
                                              {4.10f, 97},
                                              {4.20f, 100}}};
    if (!std::isfinite(voltage_v) || voltage_v <= curve.front().voltage) {
        return 0;
    }
    if (voltage_v >= curve.back().voltage) {
        return 100;
    }
    for (size_t i = 1; i < curve.size(); ++i) {
        if (voltage_v <= curve[i].voltage) {
            const float ratio =
                (voltage_v - curve[i - 1].voltage) / (curve[i].voltage - curve[i - 1].voltage);
            return std::clamp(
                curve[i - 1].percent + ratio * (curve[i].percent - curve[i - 1].percent), 0.0f,
                100.0f);
        }
    }
    return 100;
}

void BatterySocEstimator::ResetIntegrationBaseline() {
    previous_current_ma_ = 0.0f;
    last_update_us_ = 0;
    have_previous_current_ = false;
}

void BatterySocEstimator::ResetQuasiRestQualification() {
    quasi_rest_started_us_ = 0;
    quasi_rest_sample_count_ = 0;
    quasi_rest_current_mean_ma_ = 0.0;
    quasi_rest_current_m2_ = 0.0;
    quasi_rest_voltage_mean_v_ = 0.0;
    quasi_rest_voltage_m2_ = 0.0;
    quasi_rest_correction_elapsed_us_ = 0;
    have_quasi_rest_previous_sample_ = false;
    quasi_resting_ = false;
    quasi_rest_voltage_soc_percent_ = 0.0f;
}

void BatterySocEstimator::ResetFullAnchorCandidate() {
    full_anchor_candidate_started_us_ = 0;
}

void BatterySocEstimator::UpdateFullAnchor(float battery_voltage_v, float current_ma,
                                           bool motors_idle, bool charging, int64_t now_us) {
    if (charging) {
        full_anchor_charge_seen_ = true;
    }
    if (current_ma > 20.0f || battery_voltage_v < full_anchor_min_voltage_v_ - 0.10f) {
        full_anchor_charge_seen_ = false;
        full_anchored_ = false;
    }

    const bool tapering =
        motors_idle && full_anchor_charge_seen_ &&
        battery_voltage_v >= full_anchor_min_voltage_v_ &&
        current_ma >= -full_anchor_taper_current_ma_ && current_ma <= 20.0f;
    if (!tapering) {
        ResetFullAnchorCandidate();
        return;
    }
    if (full_anchor_candidate_started_us_ == 0) {
        full_anchor_candidate_started_us_ = now_us;
        return;
    }
    if (!full_anchored_ &&
        now_us - full_anchor_candidate_started_us_ >= full_anchor_qualification_us_) {
        remaining_mah_ = usable_capacity_mah_;
        tracking_degraded_ = false;
        full_anchored_ = true;
        ResetQuasiRestQualification();
    }
}

void BatterySocEstimator::ResetEmptyAnchorCandidate() {
    empty_anchor_candidate_started_us_ = 0;
}

void BatterySocEstimator::UpdateEmptyAnchor(float battery_voltage_v, float current_ma,
                                            bool motors_idle, bool discharging, int64_t now_us) {
    if (current_ma < -20.0f || battery_voltage_v > empty_anchor_max_voltage_v_ + 0.10f) {
        empty_anchored_ = false;
    }

    const bool depleted =
        motors_idle && discharging && battery_voltage_v <= empty_anchor_max_voltage_v_;
    if (!depleted) {
        ResetEmptyAnchorCandidate();
        return;
    }
    if (empty_anchor_candidate_started_us_ == 0) {
        empty_anchor_candidate_started_us_ = now_us;
        return;
    }
    if (!empty_anchored_ &&
        now_us - empty_anchor_candidate_started_us_ >= empty_anchor_qualification_us_) {
        remaining_mah_ = 0.0f;
        tracking_degraded_ = false;
        empty_anchored_ = true;
        ResetQuasiRestQualification();
    }
}

void BatterySocEstimator::UpdateQuasiRest(float battery_voltage_v, float current_ma,
                                          bool motors_idle, bool charging, int64_t now_us,
                                          int64_t elapsed_us) {
    const bool candidate =
        motors_idle && !charging && std::fabs(current_ma) <= quasi_rest_max_current_ma_;
    if (!candidate) {
        ResetQuasiRestQualification();
        return;
    }

    if (have_quasi_rest_previous_sample_ &&
        (std::fabs(current_ma - quasi_rest_previous_current_ma_) >
             quasi_rest_current_transition_ma_ ||
         std::fabs(battery_voltage_v - quasi_rest_previous_voltage_v_) >
             quasi_rest_voltage_transition_v_)) {
        ResetQuasiRestQualification();
    }
    quasi_rest_previous_current_ma_ = current_ma;
    quasi_rest_previous_voltage_v_ = battery_voltage_v;
    have_quasi_rest_previous_sample_ = true;

    if (quasi_rest_started_us_ == 0) {
        quasi_rest_started_us_ = now_us;
    }
    ++quasi_rest_sample_count_;
    const double current_delta = current_ma - quasi_rest_current_mean_ma_;
    quasi_rest_current_mean_ma_ += current_delta / quasi_rest_sample_count_;
    quasi_rest_current_m2_ +=
        current_delta * (static_cast<double>(current_ma) - quasi_rest_current_mean_ma_);
    const double voltage_delta = battery_voltage_v - quasi_rest_voltage_mean_v_;
    quasi_rest_voltage_mean_v_ += voltage_delta / quasi_rest_sample_count_;
    quasi_rest_voltage_m2_ +=
        voltage_delta * (static_cast<double>(battery_voltage_v) - quasi_rest_voltage_mean_v_);

    if (now_us - quasi_rest_started_us_ < quasi_rest_qualification_us_ ||
        quasi_rest_sample_count_ < 2) {
        return;
    }
    const double current_stddev =
        std::sqrt(std::max(0.0, quasi_rest_current_m2_ / (quasi_rest_sample_count_ - 1)));
    const double voltage_stddev =
        std::sqrt(std::max(0.0, quasi_rest_voltage_m2_ / (quasi_rest_sample_count_ - 1)));
    if (current_stddev > quasi_rest_current_stddev_ma_ ||
        voltage_stddev > quasi_rest_voltage_stddev_v_) {
        ResetQuasiRestQualification();
        return;
    }

    quasi_resting_ = true;
    quasi_rest_voltage_soc_percent_ =
        EstimatePercentFromVoltage(static_cast<float>(quasi_rest_voltage_mean_v_));
    if (elapsed_us <= 0 || quasi_rest_correction_interval_us_ <= 0 ||
        quasi_rest_correction_time_constant_us_ <= 0) {
        return;
    }
    quasi_rest_correction_elapsed_us_ += elapsed_us;
    if (quasi_rest_correction_elapsed_us_ < quasi_rest_correction_interval_us_) {
        return;
    }
    const double target_remaining_mah =
        usable_capacity_mah_ * quasi_rest_voltage_soc_percent_ / 100.0;
    const double correction_fraction =
        -std::expm1(-static_cast<double>(quasi_rest_correction_elapsed_us_) /
                    quasi_rest_correction_time_constant_us_);
    quasi_rest_correction_elapsed_us_ = 0;
    const float previous_remaining_mah = remaining_mah_;
    remaining_mah_ = static_cast<float>(
        std::clamp(static_cast<double>(remaining_mah_) +
                       (target_remaining_mah - remaining_mah_) * correction_fraction,
                   0.0, static_cast<double>(usable_capacity_mah_)));
    cumulative_voltage_correction_mah_ += remaining_mah_ - previous_remaining_mah;
}
