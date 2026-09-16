#pragma once

#include <string>

struct cJSON;
class RobotController;

class RobotWebAdapter {
public:
    explicit RobotWebAdapter(RobotController& controller) : controller_(controller) {}

    bool ExecuteAction(const std::string& action, int value, const std::string& text,
                       std::string& message);
    cJSON* CreateStatus();

private:
    RobotController& controller_;
};
