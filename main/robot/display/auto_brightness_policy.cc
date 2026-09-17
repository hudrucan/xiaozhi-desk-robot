#include "auto_brightness_policy.h"

#include "config/tuning.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr int64_t MillisecondsToMicroseconds(int64_t milliseconds) {
    return milliseconds * 1000;
}

}  // namespace

void AutoBrightnessPolicy::Configure(const Config& config, int current_brightness,
                                     int64_t now_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    config_.minimum_percent = std::clamp(config_.minimum_percent, 10, 100);
    config_.maximum_percent =
        std::clamp(config_.maximum_percent, config_.minimum_percent, 100);
    filtered_lux_valid_ = false;
    last_output_percent_ = std::clamp(current_brightness, 0, 100);
    last_update_us_ = now_us;
}

AutoBrightnessPolicy::Config AutoBrightnessPolicy::GetConfig() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

std::optional<int> AutoBrightnessPolicy::Update(float illuminance_lux, int64_t now_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.enabled || !std::isfinite(illuminance_lux) || illuminance_lux < 0.0f) {
        return std::nullopt;
    }

    if (!filtered_lux_valid_) {
        filtered_lux_ = illuminance_lux;
        filtered_lux_valid_ = true;
    } else {
        filtered_lux_ += AUTO_BRIGHTNESS_FILTER_ALPHA * (illuminance_lux - filtered_lux_);
    }

    const int target = std::clamp(TargetForLux(filtered_lux_), config_.minimum_percent,
                                  config_.maximum_percent);
    if (std::abs(target - last_output_percent_) < AUTO_BRIGHTNESS_HYSTERESIS_PERCENT) {
        return std::nullopt;
    }
    if (last_update_us_ > 0 &&
        now_us - last_update_us_ <
            MillisecondsToMicroseconds(AUTO_BRIGHTNESS_MIN_UPDATE_INTERVAL_MS)) {
        return std::nullopt;
    }
    last_output_percent_ = target;
    last_update_us_ = now_us;
    return target;
}

void AutoBrightnessPolicy::ResetReading() {
    std::lock_guard<std::mutex> lock(mutex_);
    filtered_lux_valid_ = false;
}

int AutoBrightnessPolicy::TargetForLux(float illuminance_lux) {
    if (illuminance_lux < 15.0f) {
        return 15;
    }
    if (illuminance_lux < 60.0f) {
        return 25;
    }
    if (illuminance_lux < 150.0f) {
        return 40;
    }
    if (illuminance_lux < 350.0f) {
        return 55;
    }
    if (illuminance_lux < 700.0f) {
        return 70;
    }
    return 85;
}
