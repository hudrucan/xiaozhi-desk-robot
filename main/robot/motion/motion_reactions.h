#pragma once

#include "sensors/mpu6050_motion_sensor.h"

#include <atomic>
#include <cstdint>
#include <string>

class MotionReactions {
public:
    enum class Gesture : uint8_t {
        kCalibrating,
        kSteady,
        kLeft,
        kRight,
        kUp,
        kDown,
        kUpLeft,
        kUpRight,
        kDownLeft,
        kDownRight,
        kShake,
        kSurprised,
        kSleepy,
    };

    enum class DecisionType : uint8_t { kNone, kPress, kEmotion };

    struct Decision {
        DecisionType type = DecisionType::kNone;
        const char* emotion = nullptr;
        int duration_ms = 0;
    };

    void SetCalibrated() { gesture_.store(Gesture::kSteady); }
    Decision Evaluate(const Mpu6050MotionSensor::Sample& sample, float roll_deg, float pitch_deg,
                      bool can_animate, bool face_busy, int64_t now_us);
    void CompleteDecision(bool accepted, int64_t now_us);
    Gesture GetGesture() const { return gesture_.load(); }
    bool GetEmotionTurn(const std::string& emotion, float& target_deg,
                        uint8_t& intensity_percent) const;

    static const char* GestureName(Gesture gesture);

private:
    static Gesture Classify(const Mpu6050MotionSensor::Sample& sample, float roll_deg,
                            float pitch_deg);

    std::atomic<Gesture> gesture_{Gesture::kCalibrating};
    Gesture candidate_gesture_ = Gesture::kCalibrating;
    int candidate_samples_ = 0;
    int64_t last_gesture_us_ = 0;
};
