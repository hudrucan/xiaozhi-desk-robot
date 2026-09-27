#pragma once

#include "camera_visual_wake_detector.h"

#include <cstdint>
#include <mutex>

class CameraAttentionController {
public:
    enum class State : uint8_t {
        kDisabled,
        kIdle,
        kDeskWakeArmed,
        kTracking,
        kLossGrace,
        kReturning,
    };
    enum class Origin : uint8_t { kNone, kAmbient, kDeskMode };

    struct Config {
        int motion_loss_timeout_ms = 750;
        int tracking_gain_percent = 180;
    };

    struct Input {
        bool enabled = false;
        bool vision_enabled = false;
        bool idle = false;
        bool observer_usable = false;
        bool desk_mode_active = false;
        bool visual_eligible = false;
        bool visual_owned = false;
        bool visual_centered = false;
        bool self_motion_suppressed = false;
        uint32_t normal_sample_count = 0;
        int64_t now_ms = 0;
        int64_t sample_ms = 0;
        bool spatial_valid = false;
        bool activity_active = false;
        float centroid_x = 0.5f;
        float centroid_y = 0.5f;
        float attention_centroid_x = 0.5f;
    };

    struct Action {
        bool start = false;
        bool update_target = false;
        bool release = false;
        bool return_to_desk = false;
        float target_x = 0.0f;
        float target_y = 0.0f;
    };

    struct Status {
        bool enabled = false;
        State state = State::kDisabled;
        Origin origin = Origin::kNone;
        float input_x = 0.5f;
        float input_y = 0.5f;
        float target_x = 0.0f;
        float target_y = 0.0f;
        float applied_x = 0.0f;
        float applied_y = 0.0f;
        int applied_px_x = 0;
        int applied_px_y = 0;
        int motion_loss_timeout_ms = 750;
        int tracking_gain_percent = 180;
        int loss_grace_remaining_ms = 0;
        CameraVisualWakeDetector::Stage wake_stage = CameraVisualWakeDetector::Stage::kArmed;
        int64_t wake_stage_started_ms = 0;
        uint32_t wake_count = 0;
    };

    void Configure(const Config& config);
    void SetMotionLossTimeoutMs(int timeout_ms);
    void SetTrackingGainPercent(int gain_percent);
    Action Tick(const Input& input);
    void ResetContinuity(uint32_t current_sample_count);
    Status GetStatus() const;

    static const char* StateName(State state);
    static const char* OriginName(Origin origin);

private:
    void TerminateLocked(uint32_t current_sample_count);
    void SetRestingStateLocked(bool enabled, bool vision_enabled, bool idle,
                               bool desk_mode_active);

    mutable std::mutex mutex_;
    CameraVisualWakeDetector wake_detector_;
    State state_ = State::kDisabled;
    Origin origin_ = Origin::kNone;
    uint32_t last_sample_count_ = 0;
    bool sample_discriminator_initialized_ = false;
    Config config_;
    int64_t loss_started_ms_ = 0;
    int64_t last_now_ms_ = 0;
    float target_x_ = 0.0f;
    float target_y_ = 0.0f;
};
