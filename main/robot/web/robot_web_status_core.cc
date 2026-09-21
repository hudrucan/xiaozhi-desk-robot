#include "robot_web_status.h"

#include "control/robot_controller.h"

#include <cJSON.h>

cJSON* RobotWebStatus::CreateCore() {
    const RobotStatus status = controller_.GetStatus();
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return nullptr;
    }
    cJSON_AddStringToObject(root, "state", status.state.c_str());
    cJSON_AddStringToObject(root, "server_status_phase", status.server_status_phase.c_str());
    cJSON_AddBoolToObject(root, "asr_ready", status.asr_ready);
    cJSON_AddBoolToObject(root, "asr_preparing", status.asr_preparing);
    cJSON_AddStringToObject(root, "emotion", status.emotion.c_str());
    cJSON_AddBoolToObject(root, "emotion_movement_enabled", status.emotion_movement_enabled);
    cJSON_AddBoolToObject(root, "emotion_movement_active", status.emotion_movement_active);
    return root;
}

cJSON* RobotWebStatus::CreateSystem() {
    const RobotStatus status = controller_.GetStatus();
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return nullptr;
    }
    cJSON_AddStringToObject(root, "version", status.version.c_str());
    cJSON_AddStringToObject(root, "ip", status.ip.c_str());
    cJSON_AddStringToObject(root, "ssid", status.ssid.c_str());
    cJSON_AddNumberToObject(root, "rssi", status.rssi);
    cJSON_AddNumberToObject(root, "uptime_sec", status.uptime_sec);
    cJSON_AddNumberToObject(root, "free_internal_bytes", status.free_internal_bytes);
    cJSON_AddNumberToObject(root, "free_psram_bytes", status.free_psram_bytes);
    return root;
}
