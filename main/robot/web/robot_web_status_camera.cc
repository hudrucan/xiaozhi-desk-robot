#include "robot_web_status.h"

#include "control/robot_controller.h"

#include <cJSON.h>
#include <esp_timer.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
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

cJSON* RobotWebStatus::CreateCamera() {
    const RobotStatus status = controller_.GetStatus();
    const CameraDiagnostics diagnostics = controller_.GetCameraDiagnostics();
    const CameraObserverStatus observer = controller_.GetCameraObserverStatus();
    const CameraVisionEventFrameStatus event_frame =
        controller_.GetCameraVisionEventFrameStatus();
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

    const int64_t observer_age_ms = observer.last_sample_ms > 0
                                        ? std::max<int64_t>(
                                              0, now_ms - observer.last_sample_ms)
                                        : 0;
    const int64_t observer_event_age_ms = observer.last_motion_event_ms > 0
                                              ? std::max<int64_t>(
                                                    0, now_ms - observer.last_motion_event_ms)
                                              : 0;
    cJSON_AddBoolToObject(root, "camera_vision_observer_enabled", observer.enabled);
    cJSON_AddStringToObject(root, "camera_vision_observer_state",
                            CameraObserver::StateName(observer.state));
    cJSON_AddStringToObject(root, "camera_vision_observer_suspend_reason",
                            CameraObserver::SuspendReasonName(observer.suspend_reason));
    cJSON_AddNumberToObject(root, "camera_vision_observer_interval_ms",
                            observer.interval_ms);
    cJSON_AddStringToObject(
        root, "camera_vision_observer_sampling_state",
        CameraObserver::SamplingStateName(observer.sampling_state));
    cJSON_AddNumberToObject(root, "camera_vision_observer_current_interval_ms",
                            observer.current_interval_ms);
    cJSON_AddNumberToObject(root, "camera_vision_observer_sample_age_ms",
                            observer_age_ms);
    cJSON_AddNumberToObject(root, "camera_vision_observer_luma", observer.global_luma);
    cJSON_AddNumberToObject(root, "camera_vision_observer_motion_score",
                            observer.motion_score);
    cJSON_AddNumberToObject(root, "camera_vision_observer_center_score",
                            observer.center_activity_score);
    cJSON_AddNumberToObject(root, "camera_vision_observer_changed_ratio",
                            observer.changed_pixel_ratio);
    cJSON_AddBoolToObject(root, "camera_vision_observer_motion_active",
                          observer.motion_active);
    cJSON_AddNumberToObject(root, "camera_vision_observer_event_age_ms",
                            observer_event_age_ms);
    cJSON_AddNumberToObject(root, "camera_vision_observer_capture_ms",
                            observer.capture_ms);
    cJSON_AddNumberToObject(root, "camera_vision_observer_decode_ms", observer.decode_ms);
    cJSON_AddNumberToObject(root, "camera_vision_observer_analyze_ms",
                            observer.analyze_ms);
    cJSON_AddNumberToObject(root, "camera_vision_observer_total_ms", observer.total_ms);
    cJSON_AddNumberToObject(root, "camera_vision_observer_sample_count",
                            observer.sample_count);
    cJSON_AddNumberToObject(root, "camera_vision_observer_skipped_count",
                            observer.skipped_count);
    cJSON_AddNumberToObject(root, "camera_vision_observer_failure_count",
                            observer.failure_count);
    cJSON_AddNumberToObject(root, "camera_vision_observer_event_count",
                            observer.motion_event_count);
    cJSON_AddNumberToObject(root, "camera_vision_observer_scratch_psram_bytes",
                            observer.scratch_psram_bytes);

    cJSON_AddBoolToObject(root, "camera_motion_spatial_valid",
                          observer.spatial.valid);
    cJSON_AddNumberToObject(root, "camera_motion_active_cells",
                            observer.spatial.active_cells);
    cJSON_AddNumberToObject(root, "camera_motion_centroid_x",
                            observer.spatial.centroid_x);
    cJSON_AddNumberToObject(root, "camera_motion_centroid_y",
                            observer.spatial.centroid_y);
    cJSON_AddNumberToObject(root, "camera_motion_bbox_left",
                            observer.spatial.bbox_left);
    cJSON_AddNumberToObject(root, "camera_motion_bbox_top",
                            observer.spatial.bbox_top);
    cJSON_AddNumberToObject(root, "camera_motion_bbox_right",
                            observer.spatial.bbox_right);
    cJSON_AddNumberToObject(root, "camera_motion_bbox_bottom",
                            observer.spatial.bbox_bottom);
    cJSON_AddNumberToObject(root, "camera_motion_bbox_area_ratio",
                            observer.spatial.bbox_area_ratio);
    cJSON_AddStringToObject(
        root, "camera_motion_horizontal_region",
        CameraMotionTracker::HorizontalRegionName(
            observer.spatial.horizontal_region));
    cJSON_AddStringToObject(
        root, "camera_motion_vertical_region",
        CameraMotionTracker::VerticalRegionName(observer.spatial.vertical_region));

    const int64_t event_frame_age_ms = event_frame.frame_ms > 0
                                           ? std::max<int64_t>(
                                                 0, now_ms - event_frame.frame_ms)
                                           : 0;
    cJSON_AddBoolToObject(root, "camera_vision_event_frame_enabled",
                          event_frame.enabled);
    cJSON_AddBoolToObject(root, "camera_vision_event_frame_available",
                          event_frame.available);
    cJSON_AddBoolToObject(root, "camera_vision_event_frame_capture_pending",
                          event_frame.capture_pending);
    cJSON_AddNumberToObject(root, "camera_vision_event_frame_age_ms",
                            event_frame_age_ms);
    cJSON_AddNumberToObject(root, "camera_vision_event_frame_psram_bytes",
                            event_frame.psram_bytes);
    cJSON_AddStringToObject(root, "camera_vision_event_frame_source",
                            CameraVisionEventFrame::SourceName(
                                event_frame.source));
    return root;
}
