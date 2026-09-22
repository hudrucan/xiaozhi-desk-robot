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
    cJSON_AddBoolToObject(root, "auto_brightness_enabled", status.auto_brightness_enabled);
    cJSON_AddNumberToObject(root, "auto_brightness_minimum", status.auto_brightness_minimum);
    cJSON_AddNumberToObject(root, "auto_brightness_maximum", status.auto_brightness_maximum);
    cJSON_AddNumberToObject(root, "status_light_brightness", status.status_light_brightness);
#ifdef SECONDARY_OLED_I2C_ADDRESS
    cJSON_AddBoolToObject(root, "oled_available", status.oled_available);
    cJSON_AddBoolToObject(root, "oled_flipped", status.oled_config.flip_180);
    cJSON_AddNumberToObject(root, "oled_contrast", status.oled_config.contrast);
    cJSON_AddBoolToObject(root, "oled_auto_contrast_enabled",
                          status.oled_config.auto_contrast_enabled);
    cJSON_AddNumberToObject(root, "oled_auto_contrast_minimum",
                            status.oled_config.auto_contrast_minimum);
    cJSON_AddNumberToObject(root, "oled_auto_contrast_maximum",
                            status.oled_config.auto_contrast_maximum);
    cJSON_AddNumberToObject(root, "oled_effective_contrast",
                            status.oled_effective_contrast);
    cJSON_AddBoolToObject(root, "oled_auto_contrast_available",
                          status.oled_auto_contrast_available);
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
    cJSON_AddBoolToObject(root, "web_camera_live", status.web_camera_live);
    cJSON_AddStringToObject(root, "camera_sensor", status.camera_sensor.c_str());
    cJSON_AddStringToObject(root, "camera_mode", status.camera_mode.c_str());
    cJSON_AddStringToObject(root, "camera_profile", status.camera_profile.c_str());
    cJSON_AddStringToObject(root, "camera_effective_profile",
                            status.camera_effective_profile.c_str());
    cJSON_AddBoolToObject(root, "camera_auto_profile_available",
                          status.camera_auto_profile_available);
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
    cJSON_AddBoolToObject(root, "microphone_muted", status.microphone_muted);
    cJSON_AddNumberToObject(root, "microphone_level", status.microphone_level);
    cJSON_AddBoolToObject(root, "microphone_clipping", status.microphone_clipping);
    return root;
}
