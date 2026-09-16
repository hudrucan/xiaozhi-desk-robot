#include "expressive_motion_planner.h"

#include <esp_random.h>

#include <cstddef>

std::vector<MotorController::Movement> ExpressiveMotionPlanner::BuildEmotionMovement(
    const std::string& emotion) {
    enum Group : size_t {
        kJoy,
        kAffection,
        kSad,
        kBold,
        kSurprise,
        kCurious,
        kAwkward,
        kShake,
    };

    size_t group = 0;
    if (emotion == "happy" || emotion == "laughing" || emotion == "funny" ||
        emotion == "delicious") {
        group = kJoy;
    } else if (emotion == "loving" || emotion == "kissy" || emotion == "winking") {
        group = kAffection;
    } else if (emotion == "sad" || emotion == "crying" || emotion == "sleepy") {
        group = kSad;
    } else if (emotion == "angry" || emotion == "confident" || emotion == "cool") {
        group = kBold;
    } else if (emotion == "surprised" || emotion == "shocked") {
        group = kSurprise;
    } else if (emotion == "thinking" || emotion == "confused" || emotion == "suspicious") {
        group = kCurious;
    } else if (emotion == "embarrassed" || emotion == "silly") {
        group = kAwkward;
    } else if (emotion == "shake") {
        group = kShake;
    } else {
        // Neutral, relaxed, activity states, and directional looks are face-only.
        return {};
    }

    constexpr uint8_t kVariantCount = 3;
    uint8_t variant = static_cast<uint8_t>(esp_random() % kVariantCount);
    if (variant == last_emotion_variants_[group]) {
        variant = static_cast<uint8_t>((variant + 1 + esp_random() % (kVariantCount - 1)) %
                                       kVariantCount);
    }
    last_emotion_variants_[group] = variant;

    using Direction = MotorController::Direction;
    switch (group) {
        case kJoy:
            if (variant == 0) {
                return {{Direction::kLeft, 100, 85},
                        {Direction::kRight, 160, 85},
                        {Direction::kLeft, 100, 80}};
            }
            if (variant == 1) {
                return {{Direction::kForward, 100, 75},
                        {Direction::kBackward, 100, 70},
                        {Direction::kRight, 120, 80}};
            }
            return {{Direction::kRight, 110, 85},
                    {Direction::kLeft, 170, 85},
                    {Direction::kRight, 110, 80}};
        case kAffection:
            if (variant == 0) {
                return {{Direction::kLeft, 120, 65}, {Direction::kRight, 120, 65}};
            }
            if (variant == 1) {
                return {{Direction::kForward, 90, 60}, {Direction::kBackward, 90, 60}};
            }
            return {{Direction::kRight, 130, 65}, {Direction::kLeft, 100, 60}};
        case kSad:
            if (variant == 0) {
                return {{Direction::kBackward, 100, 60}, {Direction::kLeft, 110, 60}};
            }
            if (variant == 1) {
                return {{Direction::kRight, 120, 60}, {Direction::kLeft, 120, 60}};
            }
            return {{Direction::kLeft, 140, 60}};
        case kBold:
            if (variant == 0) {
                return {{Direction::kForward, 130, 90}, {Direction::kBackward, 80, 75}};
            }
            if (variant == 1) {
                return {{Direction::kLeft, 150, 90}, {Direction::kRight, 80, 75}};
            }
            return {{Direction::kRight, 150, 90}, {Direction::kLeft, 80, 75}};
        case kSurprise:
            if (variant == 0) {
                return {{Direction::kBackward, 80, 90}};
            }
            if (variant == 1) {
                return {{Direction::kLeft, 80, 90}, {Direction::kRight, 90, 85}};
            }
            return {{Direction::kRight, 80, 90}, {Direction::kLeft, 90, 85}};
        case kCurious:
            if (variant == 0) {
                return {{Direction::kLeft, 110, 65}};
            }
            if (variant == 1) {
                return {{Direction::kRight, 110, 65}};
            }
            return {{Direction::kLeft, 90, 65}, {Direction::kRight, 140, 65}};
        case kAwkward:
            if (variant == 0) {
                return {{Direction::kLeft, 90, 70},
                        {Direction::kRight, 130, 70},
                        {Direction::kLeft, 80, 65}};
            }
            if (variant == 1) {
                return {{Direction::kBackward, 80, 65}, {Direction::kRight, 100, 70}};
            }
            return {{Direction::kRight, 90, 70},
                    {Direction::kLeft, 130, 70},
                    {Direction::kRight, 80, 65}};
        case kShake:
            if (variant == 0) {
                return {{Direction::kLeft, 100, 85},
                        {Direction::kRight, 160, 85},
                        {Direction::kLeft, 160, 85},
                        {Direction::kRight, 100, 80}};
            }
            if (variant == 1) {
                return {{Direction::kRight, 100, 85},
                        {Direction::kLeft, 160, 85},
                        {Direction::kRight, 160, 85},
                        {Direction::kLeft, 100, 80}};
            }
            return {{Direction::kLeft, 120, 80},
                    {Direction::kRight, 120, 80},
                    {Direction::kLeft, 120, 80},
                    {Direction::kRight, 120, 80}};
    }
    return {};
}

std::vector<MotorController::Movement> ExpressiveMotionPlanner::BuildDance() const {
    constexpr MotorController::Direction kDirections[] = {
        MotorController::Direction::kForward,
        MotorController::Direction::kBackward,
        MotorController::Direction::kLeft,
        MotorController::Direction::kRight,
    };
    constexpr size_t kDirectionCount = sizeof(kDirections) / sizeof(kDirections[0]);
    const size_t step_count = 30 + esp_random() % 21;
    std::vector<MotorController::Movement> movements;
    movements.reserve(step_count);
    size_t previous_direction = kDirectionCount;
    for (size_t step = 0; step < step_count; ++step) {
        size_t direction_index = esp_random() % kDirectionCount;
        if (direction_index == previous_direction) {
            direction_index =
                (direction_index + 1 + esp_random() % (kDirectionCount - 1)) % kDirectionCount;
        }
        previous_direction = direction_index;
        const auto direction = kDirections[direction_index];
        const bool turning = direction == MotorController::Direction::kLeft ||
                             direction == MotorController::Direction::kRight;
        const uint32_t duration_ms =
            turning ? 110 + esp_random() % 341 : 180 + esp_random() % 371;
        movements.push_back({direction, duration_ms});
    }
    return movements;
}

bool ExpressiveMotionPlanner::GetEmotionTurn(const std::string& emotion, float& target_deg,
                                             uint8_t& intensity_percent) const {
    if (emotion == "thinking" || emotion == "suspicious") {
        target_deg = 8.0f + static_cast<float>(esp_random() % 5);
        intensity_percent = 68;
    } else if (emotion == "confused") {
        target_deg = 10.0f + static_cast<float>(esp_random() % 5);
        intensity_percent = 72;
    } else if (emotion == "surprised" || emotion == "shocked") {
        target_deg = 7.0f + static_cast<float>(esp_random() % 4);
        intensity_percent = 85;
    } else {
        return false;
    }
    if ((esp_random() & 1U) == 0) {
        target_deg = -target_deg;
    }
    return true;
}
