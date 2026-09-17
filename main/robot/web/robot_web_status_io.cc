#include "robot_web_status.h"

#include "config/hardware_config.h"
#include "control/robot_controller.h"
#ifdef SECONDARY_OLED_I2C_ADDRESS
#include "display/secondary_display_controller.h"
#endif

#include <cJSON.h>

cJSON* RobotWebStatus::CreateDisplay() {
    const RobotStatus status = controller_.GetStatus();
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return nullptr;
    }
    cJSON_AddBoolToObject(root, "display_flipped", status.display_flipped);
    cJSON_AddNumberToObject(root, "screen_brightness", status.screen_brightness);
    cJSON_AddNumberToObject(root, "status_light_brightness", status.status_light_brightness);
#ifdef SECONDARY_OLED_I2C_ADDRESS
    cJSON_AddBoolToObject(root, "oled_available", status.oled_available);
    cJSON_AddBoolToObject(root, "oled_flipped", status.oled_config.flip_180);
    cJSON_AddNumberToObject(root, "oled_contrast", status.oled_config.contrast);
    cJSON_AddNumberToObject(root, "oled_page_count", status.oled_page_count);
    cJSON_AddStringToObject(root, "oled_brand", status.oled_config.brand.c_str());
    cJSON_AddStringToObject(root, "oled_distance_prefix",
                            status.oled_config.distance_prefix.c_str());
    cJSON* widgets = cJSON_AddArrayToObject(root, "oled_widgets");
    if (widgets != nullptr) {
        for (const auto& widget : status.oled_config.widgets) {
            cJSON* item = cJSON_CreateObject();
            if (item == nullptr) {
                break;
            }
            cJSON_AddStringToObject(item, "type",
                                    SecondaryDisplayController::WidgetTypeName(widget.type));
            cJSON_AddBoolToObject(item, "enabled", widget.enabled);
            cJSON_AddNumberToObject(item, "size", static_cast<int>(widget.size));
            cJSON_AddNumberToObject(item, "mode", widget.mode);
            cJSON_AddItemToArray(widgets, item);
        }
    }
#endif
    return root;
}

cJSON* RobotWebStatus::CreateCamera() {
    const RobotStatus status = controller_.GetStatus();
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return nullptr;
    }
    cJSON_AddBoolToObject(root, "camera_available", status.camera_available);
    cJSON_AddBoolToObject(root, "camera_flipped", status.camera_flipped);
    cJSON_AddBoolToObject(root, "live_camera_available", status.live_camera_available);
    cJSON_AddBoolToObject(root, "live_camera", status.live_camera);
    return root;
}

cJSON* RobotWebStatus::CreateAudio() {
    const RobotStatus status = controller_.GetStatus();
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return nullptr;
    }
    cJSON_AddNumberToObject(root, "speaker_volume", status.speaker_volume);
    cJSON_AddNumberToObject(root, "microphone_gain", status.microphone_gain);
    cJSON_AddNumberToObject(root, "microphone_level", status.microphone_level);
    cJSON_AddBoolToObject(root, "microphone_clipping", status.microphone_clipping);
    return root;
}
