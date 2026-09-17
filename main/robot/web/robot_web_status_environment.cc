#include "robot_web_status.h"

#include "control/robot_controller.h"
#include "sensors/environment_derived.h"

#include <cJSON.h>

namespace {

const char* StateName(SensorState state) {
    switch (state) {
        case SensorState::kReady:
            return "online";
        case SensorState::kDegraded:
            return "degraded";
        case SensorState::kMissing:
        case SensorState::kInitializing:
            return "unavailable";
    }
    return "unavailable";
}

bool AddAht20(cJSON* root, const EnvironmentStatus& environment) {
    cJSON* sensor = cJSON_AddObjectToObject(root, "aht20");
    if (sensor == nullptr) {
        return false;
    }
    cJSON_AddStringToObject(sensor, "state", StateName(environment.aht20_health.state));
    cJSON_AddBoolToObject(sensor, "temperature_valid", environment.temperature_valid);
    cJSON_AddNumberToObject(sensor, "temperature_c", environment.temperature_c);
    cJSON_AddBoolToObject(sensor, "humidity_valid", environment.humidity_valid);
    cJSON_AddNumberToObject(sensor, "humidity_percent", environment.humidity_percent);
    return true;
}

bool AddBmp280(cJSON* root, const EnvironmentStatus& environment) {
    cJSON* sensor = cJSON_AddObjectToObject(root, "bmp280");
    if (sensor == nullptr) {
        return false;
    }
    cJSON_AddStringToObject(sensor, "state", StateName(environment.bmp280_health.state));
    cJSON_AddBoolToObject(sensor, "pressure_valid", environment.pressure_valid);
    cJSON_AddNumberToObject(sensor, "pressure_hpa", environment.pressure_hpa);
    return true;
}

bool AddBh1750(cJSON* root, const EnvironmentStatus& environment) {
    cJSON* sensor = cJSON_AddObjectToObject(root, "bh1750");
    if (sensor == nullptr) {
        return false;
    }
    cJSON_AddStringToObject(sensor, "state", StateName(environment.bh1750_health.state));
    cJSON_AddBoolToObject(sensor, "illuminance_valid", environment.illuminance_valid);
    cJSON_AddNumberToObject(sensor, "illuminance_lux", environment.illuminance_lux);
    return true;
}

}  // namespace

cJSON* RobotWebStatus::CreateEnvironment() {
    const EnvironmentStatus environment = controller_.GetStatus().environment;
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return nullptr;
    }
    if (!AddAht20(root, environment) || !AddBmp280(root, environment) ||
        !AddBh1750(root, environment)) {
        cJSON_Delete(root);
        return nullptr;
    }
    cJSON* summary = cJSON_AddObjectToObject(root, "summary");
    if (summary == nullptr) {
        cJSON_Delete(root);
        return nullptr;
    }
    cJSON_AddStringToObject(summary, "light_level", LightLevelName(environment.light_level));
    cJSON_AddStringToObject(summary, "comfort_level",
                            ComfortLevelName(environment.comfort_level));
    cJSON_AddStringToObject(summary, "pressure_trend",
                            PressureTrendName(environment.pressure_trend));
    return root;
}
