#pragma once

#include <cstdint>

struct AcousticEnvironmentStatus {
    bool valid = false;
    bool noise_floor_valid = false;
    float rms_dbfs = -90.0f;
    float peak_dbfs = -90.0f;
    float noise_floor_dbfs = -90.0f;
    float signal_over_floor_db = 0.0f;
    bool self_noise = false;
    int64_t last_update_us = 0;
};
