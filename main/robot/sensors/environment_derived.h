#pragma once

#include "environment_types.h"

#include <array>
#include <cstddef>
#include <cstdint>

const char* LightLevelName(LightLevel level);
const char* ComfortLevelName(ComfortLevel level);
const char* PressureTrendName(PressureTrend trend);

class EnvironmentDerived {
public:
    void UpdateClimate(EnvironmentStatus& status) const;
    void UpdateLight(EnvironmentStatus& status) const;
    void UpdatePressure(EnvironmentStatus& status, int64_t now_us);
    void ResetPressure();

private:
    struct PressureSample {
        int64_t timestamp_us = 0;
        float pressure_hpa = 0.0f;
    };

    static constexpr size_t kPressureHistoryCapacity = 12;
    std::array<PressureSample, kPressureHistoryCapacity> pressure_history_ = {};
    size_t pressure_history_count_ = 0;
};
