#pragma once

#include "motor_controller.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class ExpressiveMotionPlanner {
public:
    std::vector<MotorController::Movement> BuildEmotionMovement(const std::string& emotion);
    std::vector<MotorController::Movement> BuildReactionMovement(
        const std::string& profile) const;
    std::vector<MotorController::Movement> BuildDance() const;
    bool GetEmotionTurn(const std::string& emotion, float& target_deg,
                        uint8_t& intensity_percent) const;

private:
    std::array<uint8_t, 8> last_emotion_variants_{};
};
