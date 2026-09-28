#include "robot_web_status.h"

#include "control/robot_controller.h"
#include "protocol.h"

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
    cJSON_AddBoolToObject(root, "reaction_active", status.reaction_active);
    cJSON_AddStringToObject(root, "reaction", status.reaction.c_str());
    cJSON_AddNumberToObject(root, "reaction_priority", status.reaction_priority);
    cJSON_AddNumberToObject(root, "reaction_generation", status.reaction_generation);
    cJSON_AddNumberToObject(root, "reaction_remaining_ms", status.reaction_remaining_ms);
    cJSON_AddStringToObject(root, "reaction_motion_state",
                            status.reaction_motion_state.c_str());
    cJSON_AddBoolToObject(root, "reaction_oled_active", status.reaction_oled_active);
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
    cJSON_AddStringToObject(root, "reset_reason", status.reset_reason.c_str());
    cJSON_AddStringToObject(root, "server_transport", status.server_transport.c_str());
    cJSON_AddBoolToObject(root, "server_connected", status.server_connected);
    cJSON_AddBoolToObject(root, "server_persistent", status.server_persistent);
    const char* connection_state = "disconnected";
    switch (static_cast<ProtocolConnectionState>(status.server_connection_state)) {
        case ProtocolConnectionState::kLegacy:
            connection_state = "legacy";
            break;
        case ProtocolConnectionState::kConnected:
            connection_state = "connected";
            break;
        case ProtocolConnectionState::kReconnecting:
            connection_state = "reconnecting";
            break;
        case ProtocolConnectionState::kDisconnected:
        default:
            break;
    }
    cJSON_AddStringToObject(root, "server_connection_state", connection_state);
    cJSON_AddNumberToObject(root, "server_connection_age_sec",
                            status.server_connection_age_sec);
    cJSON_AddNumberToObject(root, "server_last_rx_age_sec",
                            status.server_last_rx_age_sec);
    cJSON_AddNumberToObject(root, "server_reconnect_count",
                            status.server_reconnect_count);
    cJSON_AddNumberToObject(root, "server_reconnect_delay_ms",
                            status.server_reconnect_delay_ms);
    cJSON_AddNumberToObject(root, "free_internal_bytes", status.free_internal_bytes);
    cJSON_AddNumberToObject(root, "total_internal_bytes", status.total_internal_bytes);
    cJSON_AddNumberToObject(root, "minimum_free_internal_bytes",
                            status.minimum_free_internal_bytes);
    cJSON_AddNumberToObject(root, "free_psram_bytes", status.free_psram_bytes);
    cJSON_AddNumberToObject(root, "total_psram_bytes", status.total_psram_bytes);
    return root;
}
