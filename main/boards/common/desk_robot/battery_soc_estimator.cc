#include "battery_soc_estimator.h"

#include <algorithm>
#include <array>
#include <cmath>

BatterySocEstimator::BatterySocEstimator(float usable_capacity_mah,
                                         int64_t maximum_integration_gap_us)
    : usable_capacity_mah_(usable_capacity_mah),
      maximum_integration_gap_us_(maximum_integration_gap_us) {}

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
    return true;
}

void BatterySocEstimator::SeedFromVoltage(float battery_voltage_v) {
    const int percent = EstimatePercentFromVoltage(battery_voltage_v);
    remaining_mah_ = usable_capacity_mah_ * percent / 100.0f;
    last_voltage_v_ = battery_voltage_v;
    tracking_degraded_ = false;
    initialized_ = true;
    ResetIntegrationBaseline();
}

void BatterySocEstimator::Update(float battery_voltage_v, float current_ma, int64_t now_us) {
    if (!initialized_ || !std::isfinite(battery_voltage_v) || !std::isfinite(current_ma) ||
        now_us <= 0) {
        return;
    }

    last_voltage_v_ = battery_voltage_v;
    if (!have_previous_current_) {
        previous_current_ma_ = current_ma;
        last_update_us_ = now_us;
        have_previous_current_ = true;
        return;
    }

    const int64_t elapsed_us = now_us - last_update_us_;
    if (elapsed_us <= 0 || elapsed_us > maximum_integration_gap_us_) {
        previous_current_ma_ = current_ma;
        last_update_us_ = now_us;
        tracking_degraded_ = true;
        return;
    }

    const double average_current_ma =
        0.5 * (static_cast<double>(previous_current_ma_) + current_ma);
    const double delta_mah = average_current_ma * elapsed_us / 3600000000.0;
    remaining_mah_ = static_cast<float>(std::clamp(static_cast<double>(remaining_mah_) - delta_mah,
                                                0.0, static_cast<double>(usable_capacity_mah_)));
    previous_current_ma_ = current_ma;
    last_update_us_ = now_us;
}

void BatterySocEstimator::MarkMeasurementGap() {
    if (!initialized_) {
        return;
    }
    tracking_degraded_ = true;
    ResetIntegrationBaseline();
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

int BatterySocEstimator::EstimatePercentFromVoltage(float voltage_v) {
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
                static_cast<int>(std::lround(curve[i - 1].percent +
                                             ratio * (curve[i].percent - curve[i - 1].percent))),
                0, 100);
        }
    }
    return 100;
}

void BatterySocEstimator::ResetIntegrationBaseline() {
    previous_current_ma_ = 0.0f;
    last_update_us_ = 0;
    have_previous_current_ = false;
}
