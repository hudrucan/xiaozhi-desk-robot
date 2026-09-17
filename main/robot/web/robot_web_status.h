#pragma once

struct cJSON;
class RobotController;

class RobotWebStatus {
public:
    explicit RobotWebStatus(RobotController& controller) : controller_(controller) {}

    cJSON* CreateCore();
    cJSON* CreateMotors();
    cJSON* CreateSensors();
    cJSON* CreateBattery();
    cJSON* CreateDisplay();
    cJSON* CreateCamera();
    cJSON* CreateAudio();
    cJSON* CreateSystem();

private:
    RobotController& controller_;
};
