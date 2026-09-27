#pragma once

#include "display/face_geometry.h"

#include <cstddef>
#include <string>

class RobotController;

class RobotWebFaceGeometry {
public:
    explicit RobotWebFaceGeometry(RobotController& controller) : controller_(controller) {}

    std::string Encode(const std::string& emotion, bool ok = true,
                       const char* message = nullptr) const;
    bool Decode(const char* body, size_t length, std::string& emotion,
                FaceGeometry& geometry, std::string& error) const;

private:
    RobotController& controller_;
};
