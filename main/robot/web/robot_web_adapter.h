#pragma once

#include <string>

class RobotController;

class RobotWebAdapter {
public:
    explicit RobotWebAdapter(RobotController& controller) : controller_(controller) {}

    bool ExecuteAction(const std::string& action, int value, const std::string& text,
                       std::string& message);
    bool ExecuteLiveDrive(int left_percent, int right_percent, std::string& message);
private:
    RobotController& controller_;
};
