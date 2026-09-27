#include "camera_attention_controller.h"

#include <algorithm>

void CameraAttentionController::Configure(const Config& requested) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.motion_loss_timeout_ms =
        std::clamp(requested.motion_loss_timeout_ms, 0, 3000);
    config_.tracking_gain_percent =
        std::clamp(requested.tracking_gain_percent, 50, 300);
}

void CameraAttentionController::SetMotionLossTimeoutMs(int timeout_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.motion_loss_timeout_ms = std::clamp(timeout_ms, 0, 3000);
}

void CameraAttentionController::SetTrackingGainPercent(int gain_percent) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.tracking_gain_percent = std::clamp(gain_percent, 50, 300);
}

void CameraAttentionController::SetRestingStateLocked(bool enabled, bool vision_enabled,
                                                       bool idle, bool desk_mode_active) {
    state_ = !enabled || !vision_enabled || !idle
                 ? State::kDisabled
                 : desk_mode_active ? State::kDeskWakeArmed : State::kIdle;
}

void CameraAttentionController::TerminateLocked(uint32_t current_sample_count) {
    origin_ = Origin::kNone;
    target_x_ = 0.0f;
    target_y_ = 0.0f;
    loss_started_ms_ = 0;
    last_sample_count_ = current_sample_count;
    sample_discriminator_initialized_ = true;
    wake_detector_.ResetSequence();
}

CameraAttentionController::Action CameraAttentionController::Tick(const Input& input) {
    std::lock_guard<std::mutex> lock(mutex_);
    Action action;
    last_now_ms_ = input.now_ms;
    wake_detector_.Advance(input.now_ms);
    const bool active = state_ == State::kTracking || state_ == State::kLossGrace ||
                        state_ == State::kReturning;
    const bool unavailable = !input.enabled || !input.vision_enabled || !input.idle ||
                             !input.observer_usable || input.self_motion_suppressed;
    if (unavailable) {
        if (active && input.visual_owned) {
            action.release = true;
        }
        TerminateLocked(input.normal_sample_count);
        state_ = State::kDisabled;
        return action;
    }
    if (active && (!input.visual_owned || !input.visual_eligible)) {
        if (input.visual_owned) {
            action.release = true;
        }
        TerminateLocked(input.normal_sample_count);
        SetRestingStateLocked(input.enabled, input.vision_enabled, input.idle,
                              input.desk_mode_active);
        return action;
    }

    if (!active) {
        const State resting_state = input.desk_mode_active ? State::kDeskWakeArmed
                                                          : State::kIdle;
        if (state_ != resting_state) {
            state_ = resting_state;
            origin_ = Origin::kNone;
            target_x_ = 0.0f;
            target_y_ = 0.0f;
            wake_detector_.ResetSequence();
            last_sample_count_ = input.normal_sample_count;
            sample_discriminator_initialized_ = true;
            return action;
        }
    }

    if (!sample_discriminator_initialized_) {
        last_sample_count_ = input.normal_sample_count;
        sample_discriminator_initialized_ = true;
        return action;
    }
    const bool fresh = input.normal_sample_count != last_sample_count_;
    const float gain = config_.tracking_gain_percent / 100.0f;
    const float x = input.spatial_valid
                        ? std::clamp((input.attention_centroid_x - 0.5f) * 2.0f * gain,
                                     -1.0f, 1.0f)
                        : 0.0f;
    const float y = input.spatial_valid
                        ? std::clamp((input.centroid_y - 0.5f) * 2.0f * gain,
                                     -1.0f, 1.0f)
                        : 0.0f;
    if (state_ == State::kLossGrace) {
        if (fresh && input.spatial_valid && input.activity_active) {
            last_sample_count_ = input.normal_sample_count;
            state_ = State::kTracking;
            loss_started_ms_ = 0;
            target_x_ = x;
            target_y_ = y;
            action.update_target = true;
            action.target_x = x;
            action.target_y = y;
            return action;
        }
        if (input.now_ms - loss_started_ms_ >= config_.motion_loss_timeout_ms) {
            state_ = State::kReturning;
            target_x_ = 0.0f;
            target_y_ = 0.0f;
            loss_started_ms_ = 0;
            action.update_target = true;
            return action;
        }
        if (fresh) {
            last_sample_count_ = input.normal_sample_count;
        }
        return action;
    }
    if (!fresh) {
        if (state_ == State::kReturning && input.visual_centered) {
            action.release = true;
            action.return_to_desk = origin_ == Origin::kDeskMode;
            TerminateLocked(input.normal_sample_count);
            SetRestingStateLocked(true, true, true, input.desk_mode_active);
        }
        return action;
    }
    last_sample_count_ = input.normal_sample_count;
    if (state_ == State::kDeskWakeArmed) {
        if (input.spatial_valid && input.activity_active &&
            wake_detector_.Observe(input.centroid_x, input.centroid_y, input.sample_ms) &&
            input.visual_eligible) {
            state_ = State::kTracking;
            origin_ = Origin::kDeskMode;
            target_x_ = x;
            target_y_ = y;
            action.start = true;
            action.update_target = true;
            action.target_x = x;
            action.target_y = y;
        }
        return action;
    }
    if (state_ == State::kIdle) {
        if (input.spatial_valid && input.activity_active && input.visual_eligible) {
            state_ = State::kTracking;
            origin_ = Origin::kAmbient;
            target_x_ = x;
            target_y_ = y;
            action.start = true;
            action.update_target = true;
            action.target_x = x;
            action.target_y = y;
        }
        return action;
    }
    if (input.spatial_valid && input.activity_active) {
        state_ = State::kTracking;
        loss_started_ms_ = 0;
        target_x_ = x;
        target_y_ = y;
        action.update_target = true;
        action.target_x = x;
        action.target_y = y;
    } else if (state_ == State::kTracking) {
        if (config_.motion_loss_timeout_ms > 0) {
            state_ = State::kLossGrace;
            loss_started_ms_ = input.now_ms;
            return action;
        }
        state_ = State::kReturning;
        target_x_ = 0.0f;
        target_y_ = 0.0f;
        action.update_target = true;
    }
    return action;
}

void CameraAttentionController::ResetContinuity(uint32_t current_sample_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    TerminateLocked(current_sample_count);
    state_ = State::kDisabled;
}

CameraAttentionController::Status CameraAttentionController::GetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {
        .enabled = state_ != State::kDisabled,
        .state = state_,
        .origin = origin_,
        .input_x = 0.5f,
        .input_y = 0.5f,
        .target_x = target_x_,
        .target_y = target_y_,
        .applied_x = 0.0f,
        .applied_y = 0.0f,
        .applied_px_x = 0,
        .applied_px_y = 0,
        .motion_loss_timeout_ms = config_.motion_loss_timeout_ms,
        .tracking_gain_percent = config_.tracking_gain_percent,
        .loss_grace_remaining_ms = state_ == State::kLossGrace
            ? static_cast<int>(std::max<int64_t>(
                  0, config_.motion_loss_timeout_ms - (last_now_ms_ - loss_started_ms_)))
            : 0,
        .wake_stage = wake_detector_.stage(),
        .wake_stage_started_ms = wake_detector_.stage_started_ms(),
        .wake_count = wake_detector_.wake_count(),
    };
}

const char* CameraAttentionController::StateName(State state) {
    switch (state) {
        case State::kIdle: return "idle";
        case State::kDeskWakeArmed: return "desk_wake_armed";
        case State::kTracking: return "tracking";
        case State::kLossGrace: return "loss_grace";
        case State::kReturning: return "returning";
        case State::kDisabled:
        default: return "disabled";
    }
}

const char* CameraAttentionController::OriginName(Origin origin) {
    switch (origin) {
        case Origin::kAmbient: return "ambient";
        case Origin::kDeskMode: return "desk_mode";
        case Origin::kNone:
        default: return "none";
    }
}
