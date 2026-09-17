#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

class AutoBrightnessPolicy {
public:
    struct Config {
        bool enabled = false;
        int minimum_percent = 15;
        int maximum_percent = 85;
    };

    void Configure(const Config& config, int current_brightness, int64_t now_us);
    Config GetConfig() const;
    std::optional<int> Update(float illuminance_lux, int64_t now_us);
    void ResetReading();

private:
    static int TargetForLux(float illuminance_lux);

    mutable std::mutex mutex_;
    Config config_;
    bool filtered_lux_valid_ = false;
    float filtered_lux_ = 0.0f;
    int last_output_percent_ = 75;
    int64_t last_update_us_ = 0;
};
