#include "environment_derived.h"

#include "config/tuning.h"

#include <algorithm>

namespace {

constexpr int64_t MillisecondsToMicroseconds(int64_t milliseconds) {
    return milliseconds * 1000;
}

}  // namespace

const char* LightLevelName(LightLevel level) {
    switch (level) {
        case LightLevel::kUnavailable:
            return "unavailable";
        case LightLevel::kDark:
            return "dark";
        case LightLevel::kDim:
            return "dim";
        case LightLevel::kNormal:
            return "normal";
        case LightLevel::kBright:
            return "bright";
        case LightLevel::kVeryBright:
            return "very_bright";
    }
    return "unavailable";
}

const char* ComfortLevelName(ComfortLevel level) {
    switch (level) {
        case ComfortLevel::kUnavailable:
            return "unavailable";
        case ComfortLevel::kDry:
            return "dry";
        case ComfortLevel::kComfortable:
            return "comfortable";
        case ComfortLevel::kHumid:
            return "humid";
        case ComfortLevel::kHot:
            return "hot";
        case ComfortLevel::kCold:
            return "cold";
    }
    return "unavailable";
}

const char* PressureTrendName(PressureTrend trend) {
    switch (trend) {
        case PressureTrend::kUnavailable:
            return "unavailable";
        case PressureTrend::kFalling:
            return "falling";
        case PressureTrend::kStable:
            return "stable";
        case PressureTrend::kRising:
            return "rising";
    }
    return "unavailable";
}

void EnvironmentDerived::UpdateClimate(EnvironmentStatus& status) const {
    if (!status.temperature_valid || !status.humidity_valid) {
        status.comfort_level = ComfortLevel::kUnavailable;
    } else if (status.temperature_c <= ENVIRONMENT_COMFORT_COLD_MAX_C) {
        status.comfort_level = ComfortLevel::kCold;
    } else if (status.temperature_c >= ENVIRONMENT_COMFORT_HOT_MIN_C) {
        status.comfort_level = ComfortLevel::kHot;
    } else if (status.humidity_percent <= ENVIRONMENT_COMFORT_DRY_MAX_PERCENT) {
        status.comfort_level = ComfortLevel::kDry;
    } else if (status.humidity_percent >= ENVIRONMENT_COMFORT_HUMID_MIN_PERCENT) {
        status.comfort_level = ComfortLevel::kHumid;
    } else {
        status.comfort_level = ComfortLevel::kComfortable;
    }
}

void EnvironmentDerived::UpdateLight(EnvironmentStatus& status) const {
    if (!status.illuminance_valid) {
        status.light_level = LightLevel::kUnavailable;
    } else if (status.illuminance_lux < ENVIRONMENT_LIGHT_DARK_MAX_LUX) {
        status.light_level = LightLevel::kDark;
    } else if (status.illuminance_lux < ENVIRONMENT_LIGHT_DIM_MAX_LUX) {
        status.light_level = LightLevel::kDim;
    } else if (status.illuminance_lux < ENVIRONMENT_LIGHT_NORMAL_MAX_LUX) {
        status.light_level = LightLevel::kNormal;
    } else if (status.illuminance_lux < ENVIRONMENT_LIGHT_BRIGHT_MAX_LUX) {
        status.light_level = LightLevel::kBright;
    } else {
        status.light_level = LightLevel::kVeryBright;
    }
}

void EnvironmentDerived::UpdatePressure(EnvironmentStatus& status, int64_t now_us) {
    if (!status.pressure_valid) {
        status.pressure_trend = PressureTrend::kUnavailable;
        return;
    }

    const int64_t interval_us =
        MillisecondsToMicroseconds(ENVIRONMENT_PRESSURE_HISTORY_INTERVAL_MS);
    if (pressure_history_count_ > 0 &&
        now_us - pressure_history_[pressure_history_count_ - 1].timestamp_us < interval_us) {
        return;
    }

    const int64_t oldest_allowed_us =
        now_us - MillisecondsToMicroseconds(ENVIRONMENT_PRESSURE_HISTORY_WINDOW_MS);
    size_t retained = 0;
    for (size_t index = 0; index < pressure_history_count_; ++index) {
        if (pressure_history_[index].timestamp_us >= oldest_allowed_us) {
            pressure_history_[retained++] = pressure_history_[index];
        }
    }
    pressure_history_count_ = retained;
    if (pressure_history_count_ == pressure_history_.size()) {
        std::move(pressure_history_.begin() + 1, pressure_history_.end(),
                  pressure_history_.begin());
        --pressure_history_count_;
    }
    pressure_history_[pressure_history_count_++] = {now_us, status.pressure_hpa};

    constexpr size_t kMinimumSamples = 5;
    const int64_t minimum_span_us =
        MillisecondsToMicroseconds(ENVIRONMENT_PRESSURE_TREND_MIN_SPAN_MS);
    if (pressure_history_count_ < kMinimumSamples ||
        now_us - pressure_history_[0].timestamp_us < minimum_span_us) {
        status.pressure_trend = PressureTrend::kUnavailable;
        return;
    }

    const float oldest_mean =
        (pressure_history_[0].pressure_hpa + pressure_history_[1].pressure_hpa) / 2.0f;
    const float newest_mean =
        (pressure_history_[pressure_history_count_ - 2].pressure_hpa +
         pressure_history_[pressure_history_count_ - 1].pressure_hpa) /
        2.0f;
    const float delta_hpa = newest_mean - oldest_mean;
    if (delta_hpa > ENVIRONMENT_PRESSURE_TREND_THRESHOLD_HPA) {
        status.pressure_trend = PressureTrend::kRising;
    } else if (delta_hpa < -ENVIRONMENT_PRESSURE_TREND_THRESHOLD_HPA) {
        status.pressure_trend = PressureTrend::kFalling;
    } else {
        status.pressure_trend = PressureTrend::kStable;
    }
}

void EnvironmentDerived::ResetPressure() {
    pressure_history_ = {};
    pressure_history_count_ = 0;
}
