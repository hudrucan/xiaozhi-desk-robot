#pragma once

#include "camera/camera_settings.h"

#include <cstddef>
#include <string>

class RobotController;

class RobotWebCameraSettings {
public:
    explicit RobotWebCameraSettings(RobotController& controller) : controller_(controller) {}

    std::string Encode(bool ok = true, const char* message = nullptr) const;
    bool Decode(const char* body, size_t length, CameraSettingsConfig& settings,
                std::string& error) const;

private:
    RobotController& controller_;
};
