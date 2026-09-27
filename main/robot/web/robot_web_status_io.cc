#include "robot_web_status.h"

#include "config/hardware_config.h"
#include "control/robot_controller.h"
#ifdef SECONDARY_OLED_I2C_ADDRESS
#include "display/secondary_display_controller.h"
#endif

#include <cJSON.h>
#include <esp_timer.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>

namespace {

template <size_t Size>
std::string HexBytes(const std::array<uint8_t, Size>& bytes) {
    std::string result;
    result.reserve(Size * 3 - 1);
    char value[3] = {};
    for (size_t index = 0; index < Size; ++index) {
        if (index > 0) {
            result.push_back(' ');
        }
        std::snprintf(value, sizeof(value), "%02X", bytes[index]);
        result.append(value);
    }
    return result;
}

std::string Hex32(uint32_t value) {
    char output[11] = {};
    std::snprintf(output, sizeof(output), "0x%08lX",
                  static_cast<unsigned long>(value));
    return output;
}

}  // namespace

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

cJSON* RobotWebStatus::CreateCamera() {
    const RobotStatus status = controller_.GetStatus();
    const CameraDiagnostics diagnostics = controller_.GetCameraDiagnostics();
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return nullptr;
    }
    cJSON_AddBoolToObject(root, "camera_available", status.camera_available);
    cJSON_AddBoolToObject(root, "camera_mirrored", status.camera_mirrored);
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
    cJSON_AddStringToObject(root, "camera_request_state",
                            status.camera_request_state.c_str());
    cJSON_AddNumberToObject(root, "camera_request_age_sec", status.camera_request_age_sec);
    cJSON_AddNumberToObject(root, "camera_capture_ms", status.camera_capture_ms);
    cJSON_AddNumberToObject(root, "camera_vision_ms", status.camera_vision_ms);
    cJSON_AddNumberToObject(root, "camera_image_bytes", status.camera_image_bytes);
    cJSON_AddNumberToObject(root, "camera_response_bytes", status.camera_response_bytes);
    const int64_t now_ms = esp_timer_get_time() / 1000;
    const int64_t age_ms = diagnostics.last_read_ms > 0
                               ? std::max<int64_t>(0, now_ms - diagnostics.last_read_ms)
                               : 0;
    cJSON_AddBoolToObject(root, "camera_diag_available", diagnostics.available);
    cJSON_AddBoolToObject(root, "camera_diag_valid", diagnostics.valid);
    cJSON_AddBoolToObject(root, "camera_diag_stale",
                          !diagnostics.valid || age_ms > 4000);
    cJSON_AddNumberToObject(root, "camera_diag_age_ms", age_ms);
    cJSON_AddNumberToObject(root, "camera_diag_last_read_ms",
                            diagnostics.last_read_ms);
    cJSON_AddNumberToObject(root, "camera_diag_read_ms",
                            diagnostics.register_read_ms);

    cJSON_AddBoolToObject(root, "camera_awb_enabled", diagnostics.awb_enabled);
    cJSON_AddBoolToObject(root, "camera_awb_gain_enabled",
                          diagnostics.awb_gain_enabled);
    cJSON_AddBoolToObject(root, "camera_advanced_awb_enabled",
                          diagnostics.advanced_awb_enabled);
    cJSON_AddNumberToObject(root, "camera_wb_mode", diagnostics.wb_mode);
    cJSON_AddNumberToObject(root, "camera_wb_control_raw",
                            diagnostics.wb_control_raw);
    cJSON_AddNumberToObject(root, "camera_awb_r_gain_raw",
                            diagnostics.awb_r_gain_raw);
    cJSON_AddNumberToObject(root, "camera_awb_g_gain_raw",
                            diagnostics.awb_g_gain_raw);
    cJSON_AddNumberToObject(root, "camera_awb_b_gain_raw",
                            diagnostics.awb_b_gain_raw);

    cJSON_AddNumberToObject(root, "camera_isp_control_00_raw",
                            diagnostics.isp_control_00_raw);
    cJSON_AddNumberToObject(root, "camera_isp_control_01_raw",
                            diagnostics.isp_control_01_raw);
    cJSON_AddBoolToObject(root, "camera_ccm_native_valid",
                          diagnostics.ccm_native_valid);
    cJSON_AddBoolToObject(root, "camera_ccm_matches_native",
                          diagnostics.ccm_matches_native);
    const std::string ccm_current = HexBytes(diagnostics.ccm_current);
    const std::string ccm_native = HexBytes(diagnostics.ccm_native);
    cJSON_AddStringToObject(root, "camera_ccm_current", ccm_current.c_str());
    cJSON_AddStringToObject(root, "camera_ccm_native", ccm_native.c_str());
    cJSON_AddBoolToObject(root, "camera_awb_table_native_valid",
                          diagnostics.awb_table_native_valid);
    cJSON_AddBoolToObject(root, "camera_awb_table_matches_native",
                          diagnostics.awb_table_matches_native);
    const std::string awb_current_hash =
        Hex32(diagnostics.awb_table_current_hash);
    const std::string awb_native_hash =
        Hex32(diagnostics.awb_table_native_hash);
    cJSON_AddStringToObject(root, "camera_awb_table_current_hash",
                            awb_current_hash.c_str());
    cJSON_AddStringToObject(root, "camera_awb_table_native_hash",
                            awb_native_hash.c_str());

    cJSON_AddBoolToObject(root, "camera_aec_enabled", diagnostics.aec_enabled);
    cJSON_AddBoolToObject(root, "camera_aec2_enabled", diagnostics.aec2_enabled);
    cJSON_AddBoolToObject(root, "camera_agc_enabled", diagnostics.agc_enabled);
    cJSON_AddNumberToObject(root, "camera_exposure_raw", diagnostics.exposure_raw);
    cJSON_AddNumberToObject(root, "camera_gain_raw", diagnostics.gain_raw);
    cJSON_AddNumberToObject(root, "camera_gain_ceiling_raw",
                            diagnostics.gain_ceiling_raw);
    cJSON_AddNumberToObject(root, "camera_ae_target_high",
                            diagnostics.ae_target_high);
    cJSON_AddNumberToObject(root, "camera_ae_target_low",
                            diagnostics.ae_target_low);
    cJSON_AddNumberToObject(root, "camera_ae_target_high_2",
                            diagnostics.ae_target_high_2);
    cJSON_AddNumberToObject(root, "camera_ae_target_low_2",
                            diagnostics.ae_target_low_2);
    cJSON_AddNumberToObject(root, "camera_ae_fast_high",
                            diagnostics.ae_fast_high);
    cJSON_AddNumberToObject(root, "camera_ae_fast_low",
                            diagnostics.ae_fast_low);

    cJSON_AddBoolToObject(root, "camera_bpc_enabled", diagnostics.bpc_enabled);
    cJSON_AddBoolToObject(root, "camera_wpc_enabled", diagnostics.wpc_enabled);
    cJSON_AddBoolToObject(root, "camera_gamma_enabled", diagnostics.gamma_enabled);
    cJSON_AddBoolToObject(root, "camera_lens_correction_enabled",
                          diagnostics.lens_correction_enabled);
    cJSON_AddBoolToObject(root, "camera_mirror_enabled",
                          diagnostics.mirror_enabled);
    cJSON_AddBoolToObject(root, "camera_flip_enabled", diagnostics.flip_enabled);
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
