#pragma once

#include <cstdint>

enum class SensorState : uint8_t {
    kMissing,
    kInitializing,
    kReady,
    kDegraded,
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

    int64_t temperature_last_good_us = 0;
    int64_t humidity_last_good_us = 0;
    int64_t pressure_last_good_us = 0;
    int64_t illuminance_last_good_us = 0;
};
