#include "motion_reactions.h"

#include "config/tuning.h"

#include <cmath>

MotionReactions::Gesture MotionReactions::Classify(const Mpu6050MotionSensor::Sample& sample,
                                                   float roll_deg, float pitch_deg) {
    const bool press_impulse = sample.acceleration_magnitude_g > MPU6050_PRESS_THRESHOLD_G;
    if (sample.acceleration_magnitude_g < MPU6050_FREEFALL_THRESHOLD_G ||
        sample.acceleration_magnitude_g > MPU6050_IMPACT_THRESHOLD_G || press_impulse) {
        return Gesture::kSurprised;
    }
    if (sample.rotation_magnitude_dps > MPU6050_SHAKE_THRESHOLD_DPS) {
        return Gesture::kShake;
    }
    if (std::fabs(pitch_deg) > MPU6050_TILT_THRESHOLD_DEG) {
        const bool diagonal = std::fabs(roll_deg) > MPU6050_TILT_THRESHOLD_DEG &&
                              std::fabs(roll_deg) < 75.0f;
        if (pitch_deg > 0.0f) {
            return !diagonal ? Gesture::kUp
                             : roll_deg < 0.0f ? Gesture::kUpLeft : Gesture::kUpRight;
        }
        return !diagonal ? Gesture::kDown
                         : roll_deg < 0.0f ? Gesture::kDownLeft : Gesture::kDownRight;
    }
    if (std::fabs(roll_deg) > 150.0f) {
        return Gesture::kSleepy;
    }
    if (std::fabs(roll_deg) > MPU6050_TILT_THRESHOLD_DEG) {
        return roll_deg < 0.0f ? Gesture::kLeft : Gesture::kRight;
    }
    return Gesture::kSteady;
}

MotionReactions::Decision MotionReactions::Evaluate(const Mpu6050MotionSensor::Sample& sample,
                                                    float roll_deg, float pitch_deg,
                                                    bool can_animate, bool face_busy,
                                                    int64_t now_us) {
    const Gesture gesture = Classify(sample, roll_deg, pitch_deg);
    gesture_.store(gesture);
    if (!can_animate || gesture == Gesture::kSteady) {
        candidate_gesture_ = Gesture::kCalibrating;
        candidate_samples_ = 0;
        return {};
    }
    if (gesture == candidate_gesture_) {
        ++candidate_samples_;
    } else {
        candidate_gesture_ = gesture;
        candidate_samples_ = 1;
    }
    if (sample.acceleration_magnitude_g > MPU6050_PRESS_THRESHOLD_G && !face_busy &&
        now_us - last_gesture_us_ >= MPU6050_GESTURE_COOLDOWN_MS * 1000LL) {
        return {.type = DecisionType::kPress,
                .emotion = candidate_samples_ >= 3 ? GestureName(gesture) : nullptr,
                .duration_ms = 1400};
    }
    if (candidate_samples_ < 3 || face_busy ||
        now_us - last_gesture_us_ < MPU6050_GESTURE_COOLDOWN_MS * 1000LL) {
        return {};
    }
    const int duration_ms =
        gesture == Gesture::kShake || gesture == Gesture::kSurprised ? 1400 : 1800;
    return {.type = DecisionType::kEmotion,
            .emotion = GestureName(gesture),
            .duration_ms = duration_ms};
}

void MotionReactions::CompleteDecision(bool accepted, int64_t now_us) {
    if (accepted) {
        last_gesture_us_ = now_us;
    }
    candidate_gesture_ = Gesture::kCalibrating;
    candidate_samples_ = 0;
}

const char* MotionReactions::GestureName(Gesture gesture) {
    switch (gesture) {
        case Gesture::kSteady:
            return "steady";
        case Gesture::kLeft:
            return "left";
        case Gesture::kRight:
            return "right";
        case Gesture::kUp:
            return "up";
        case Gesture::kDown:
            return "down";
        case Gesture::kUpLeft:
            return "up_left";
        case Gesture::kUpRight:
            return "up_right";
        case Gesture::kDownLeft:
            return "down_left";
        case Gesture::kDownRight:
            return "down_right";
        case Gesture::kShake:
            return "shake";
        case Gesture::kSurprised:
            return "surprised";
        case Gesture::kSleepy:
            return "sleepy";
        case Gesture::kCalibrating:
            return "calibrating";
    }
    return "calibrating";
}
