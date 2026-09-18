#pragma once

#include "camera_settings.h"

#include <cstdint>
#include <mutex>

class CameraImagePolicy {
public:
    struct Status {
        bool ambient_light_available = false;
        CameraImageProfile effective_profile = CameraImageProfile::kNormal;
    };

    void UpdateAmbientLight(bool valid, float illuminance_lux, int64_t timestamp_us);
    Status GetStatus(int64_t now_us) const;
    CameraImageProfile EffectiveProfile(int64_t now_us) const;

private:
    Status GetStatusLocked(int64_t now_us) const;

    mutable std::mutex mutex_;
    bool sample_valid_ = false;
    int64_t last_sample_us_ = 0;
    CameraImageProfile effective_profile_ = CameraImageProfile::kNormal;
};
