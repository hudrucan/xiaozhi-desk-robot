#pragma once

#include <cstdint>

enum class SensorState : uint8_t {
    kMissing,
    kInitializing,
    kReady,
    kDegraded,
};

enum class LightLevel : uint8_t {
    kUnavailable,
    kDark,
    kDim,
    kNormal,
    kBright,
    kVeryBright,
};

enum class ComfortLevel : uint8_t {
    kUnavailable,
    kDry,
    kComfortable,
    kHumid,
    kHot,
    kCold,
};

enum class PressureTrend : uint8_t {
    kUnavailable,
    kFalling,
    kStable,
    kRising,
};

struct SensorHealth {
    SensorState state = SensorState::kMissing;
    bool sample_valid = false;
    uint32_t consecutive_failures = 0;
    uint32_t total_failures = 0;
    int64_t last_good_us = 0;
    int64_t last_failure_us = 0;
    int64_t next_probe_us = 0;
};

struct EnvironmentStatus {
    SensorHealth aht20_health;
    SensorHealth bmp280_health;
    SensorHealth bh1750_health;

    bool aht20_available = false;
    bool bmp280_available = false;
    bool bh1750_available = false;

    bool temperature_valid = false;
    bool humidity_valid = false;
    bool pressure_valid = false;
    bool illuminance_valid = false;

    float temperature_c = 0.0f;
    float humidity_percent = 0.0f;
    float pressure_hpa = 0.0f;
    float illuminance_lux = 0.0f;

    LightLevel light_level = LightLevel::kUnavailable;
    ComfortLevel comfort_level = ComfortLevel::kUnavailable;
    PressureTrend pressure_trend = PressureTrend::kUnavailable;

    int64_t temperature_last_good_us = 0;
    int64_t humidity_last_good_us = 0;
    int64_t pressure_last_good_us = 0;
    int64_t illuminance_last_good_us = 0;
};
