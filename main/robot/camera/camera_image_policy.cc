#include "camera_image_policy.h"

#include "config/tuning.h"

#include <cmath>

void CameraImagePolicy::UpdateAmbientLight(bool valid, float illuminance_lux,
                                           int64_t timestamp_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!valid || !std::isfinite(illuminance_lux) || illuminance_lux < 0.0f) {
        sample_valid_ = false;
        effective_profile_ = CameraImageProfile::kNormal;
        return;
    }

    const bool previous_sample_fresh =
        sample_valid_ && last_sample_us_ > 0 && timestamp_us >= last_sample_us_ &&
        timestamp_us - last_sample_us_ <=
            static_cast<int64_t>(CAMERA_AUTO_LIGHT_STALE_MS) * 1000;
    if (!previous_sample_fresh) {
        effective_profile_ = CameraImageProfile::kNormal;
    }
    sample_valid_ = true;
    last_sample_us_ = timestamp_us;
    if (effective_profile_ == CameraImageProfile::kLowLight) {
        if (illuminance_lux >= CAMERA_AUTO_LOW_LIGHT_EXIT_LUX) {
            effective_profile_ = CameraImageProfile::kNormal;
        }
    } else if (illuminance_lux < CAMERA_AUTO_LOW_LIGHT_ENTER_LUX) {
        effective_profile_ = CameraImageProfile::kLowLight;
    }
}

CameraImagePolicy::Status CameraImagePolicy::GetStatus(int64_t now_us) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return GetStatusLocked(now_us);
}

CameraImageProfile CameraImagePolicy::EffectiveProfile(int64_t now_us) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return GetStatusLocked(now_us).effective_profile;
}

CameraImagePolicy::Status CameraImagePolicy::GetStatusLocked(int64_t now_us) const {
    const bool fresh = sample_valid_ && last_sample_us_ > 0 && now_us >= last_sample_us_ &&
                       now_us - last_sample_us_ <=
                           static_cast<int64_t>(CAMERA_AUTO_LIGHT_STALE_MS) * 1000;
    if (!fresh) {
        return {
            .ambient_light_available = false,
            .effective_profile = CameraImageProfile::kNormal,
        };
    }
    return {
        .ambient_light_available = true,
        .effective_profile = effective_profile_,
    };
}
