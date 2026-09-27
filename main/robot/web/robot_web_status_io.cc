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
    cJSON_AddBoolToObject(root, "desk_mode_enabled", status.desk_mode_enabled);
    cJSON_AddNumberToObject(root, "desk_mode_delay_seconds",
                            status.desk_mode_delay_seconds);
    cJSON_AddBoolToObject(root, "desk_mode_use_24_hour",
                          status.desk_mode_use_24_hour);
    cJSON_AddBoolToObject(root, "desk_mode_active", status.desk_mode_active);
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

cJSON* RobotWebStatus::CreateAudio() {
    const RobotStatus status = controller_.GetStatus();
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return nullptr;
    }
    cJSON_AddNumberToObject(root, "speaker_volume", status.speaker_volume);
    cJSON_AddBoolToObject(root, "ambient_sound_enabled", status.ambient_sound_enabled);
    cJSON_AddBoolToObject(root, "microphone_muted", status.microphone_muted);
    cJSON_AddNumberToObject(root, "microphone_level", status.microphone_level);
    cJSON_AddBoolToObject(root, "microphone_clipping", status.microphone_clipping);
    const VoiceInputStatus& voice = status.voice_input;
    cJSON_AddStringToObject(root, "voice_profile", VoiceInputProfileName(voice.config.profile));
    cJSON_AddNumberToObject(root, "capture_trim_db", voice.config.capture_trim_db);
    cJSON_AddNumberToObject(root, "voice_gain_db", voice.config.voice_gain_db);
    cJSON_AddBoolToObject(root, "ns_available", voice.ns_available);
    cJSON_AddBoolToObject(root, "ns_requested", voice.config.ns_requested);
    cJSON_AddBoolToObject(root, "ns_active", voice.ns_active);
    cJSON_AddBoolToObject(root, "agc_available", voice.agc_available);
    cJSON_AddBoolToObject(root, "agc_requested", voice.config.agc_requested);
    cJSON_AddBoolToObject(root, "agc_active", voice.agc_active);
    if (voice.voice_level.valid) {
        cJSON_AddNumberToObject(root, "voice_rms_dbfs", voice.voice_level.rms_dbfs);
        cJSON_AddNumberToObject(root, "voice_peak_dbfs", voice.voice_level.peak_dbfs);
    } else {
        cJSON_AddNullToObject(root, "voice_rms_dbfs");
        cJSON_AddNullToObject(root, "voice_peak_dbfs");
    }
    if (voice.afe_output_level.valid) {
        cJSON_AddNumberToObject(root, "afe_output_rms_dbfs",
                                voice.afe_output_level.rms_dbfs);
        cJSON_AddNumberToObject(root, "afe_output_peak_dbfs",
                                voice.afe_output_level.peak_dbfs);
    } else {
        cJSON_AddNullToObject(root, "afe_output_rms_dbfs");
        cJSON_AddNullToObject(root, "afe_output_peak_dbfs");
    }
    cJSON_AddBoolToObject(root, "voice_processing_restart_required", voice.restart_required);
    return root;
}
