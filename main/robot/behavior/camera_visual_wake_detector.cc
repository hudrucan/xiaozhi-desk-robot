#include "camera_visual_wake_detector.h"

#include <cmath>

void CameraVisualWakeDetector::ResetSequence() {
    stage_ = Stage::kArmed;
    have_anchor_ = false;
    gesture_started_ms_ = 0;
    stage_started_ms_ = 0;
    last_sample_ms_ = 0;
}

bool CameraVisualWakeDetector::Observe(float x, float y, int64_t sample_ms) {
    if (stage_ == Stage::kTriggered) {
        ResetSequence();
    }
    if (last_sample_ms_ > 0 && sample_ms - last_sample_ms_ > kStaleSampleMs) {
        ResetSequence();
    }
    if (gesture_started_ms_ > 0 && sample_ms - gesture_started_ms_ > kGestureTimeoutMs) {
        ResetSequence();
    }
    last_sample_ms_ = sample_ms;
    if (!have_anchor_) {
        have_anchor_ = true;
        anchor_x_ = x;
        anchor_y_ = y;
        turning_y_ = y;
        gesture_started_ms_ = sample_ms;
        stage_started_ms_ = sample_ms;
        return false;
    }
    if (std::fabs(x - anchor_x_) > kMaximumXDrift) {
        ResetSequence();
        have_anchor_ = true;
        anchor_x_ = x;
        anchor_y_ = y;
        turning_y_ = y;
        gesture_started_ms_ = sample_ms;
        stage_started_ms_ = sample_ms;
        return false;
    }

    switch (stage_) {
        case Stage::kArmed:
            turning_y_ = y < turning_y_ ? y : turning_y_;
            if (anchor_y_ - turning_y_ >= kLegExcursion) {
                stage_ = Stage::kSawUp;
                stage_started_ms_ = sample_ms;
            }
            break;
        case Stage::kSawUp:
            turning_y_ = y < turning_y_ ? y : turning_y_;
            if (y - turning_y_ >= kLegExcursion) {
                turning_y_ = y;
                stage_ = Stage::kSawDown;
                stage_started_ms_ = sample_ms;
            }
            break;
        case Stage::kSawDown:
            turning_y_ = y > turning_y_ ? y : turning_y_;
            if (turning_y_ - y >= kLegExcursion) {
                stage_ = Stage::kTriggered;
                stage_started_ms_ = sample_ms;
                ++wake_count_;
                return true;
            }
            break;
        case Stage::kTriggered:
            break;
    }
    return false;
}

void CameraVisualWakeDetector::Advance(int64_t now_ms) {
    if ((last_sample_ms_ > 0 && now_ms - last_sample_ms_ > kStaleSampleMs) ||
        (gesture_started_ms_ > 0 && now_ms - gesture_started_ms_ > kGestureTimeoutMs)) {
        ResetSequence();
    }
}

const char* CameraVisualWakeDetector::StageName(Stage stage) {
    switch (stage) {
        case Stage::kSawUp: return "saw_up";
        case Stage::kSawDown: return "saw_down";
        case Stage::kTriggered: return "triggered";
        case Stage::kArmed:
        default: return "armed";
    }
}
