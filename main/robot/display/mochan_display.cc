#include "mochan_display.h"

#include "display/lvgl_display/lvgl_theme.h"
#include "src/misc/cache/instance/lv_image_cache.h"

#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <material_symbols.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

namespace {
const lv_color_t kFaceBackground = LV_COLOR_MAKE(0x00, 0x00, 0x00);
const lv_color_t kBrass = LV_COLOR_MAKE(0xc6, 0xa1, 0x5b);
const lv_color_t kBrassHighlight = LV_COLOR_MAKE(0xe3, 0xc2, 0x7b);
const lv_color_t kEyelidShadow = LV_COLOR_MAKE(0x72, 0x55, 0x2b);
const lv_color_t kSpinnerTrack = LV_COLOR_MAKE(0x4b, 0x3b, 0x25);
constexpr int kPreviewDurationMs = 5000;
constexpr int kResponseTextScale = 210;
// Keep full-face motion and idle cadence tunables together for hardware iteration.
constexpr int kFaceAnimationPeriodMs = 33;
constexpr int kFaceLayoutTransitionMs = 450;
constexpr int kEmotionLayoutTransitionMs = 200;
constexpr int kResponseBoxFadeMs = 200;
constexpr int kResponseFadeInStartFaceProgress = 144;
constexpr int kFaceReturnStartResponseProgress = 96;
constexpr int64_t kPerformanceLogIntervalUs = 5000000;
constexpr int kIdleStartDelayMs = 15000;
constexpr int kIdleEarlyStageMs = 45000;
constexpr int kIdleSleepyStageMs = 120000;
constexpr int kIdleEmotionHoldMinMs = 9000;
constexpr int kIdleEmotionHoldMaxMs = 16000;
constexpr int kIdleHappyHoldMinMs = 4500;
constexpr int kIdleHappyHoldMaxMs = 7000;
constexpr int kIdleSurprisedHoldMs = 1800;
constexpr int kIdleSleepyHoldMinMs = 14000;
constexpr int kIdleSleepyHoldMaxMs = 24000;
constexpr int kYawnMinimumIdleMs = 120000;
constexpr int kYawnCooldownMs = 180000;
constexpr int kYawnOpenMs = 500;
constexpr int kYawnHoldMs = 350;
constexpr int kYawnCloseMs = 500;
constexpr int kYawnSettleMs = 250;
constexpr int kMouthMotionIntervalMinMs = 8000;
constexpr int kMouthMotionIntervalMaxMs = 14000;
constexpr int kMouthMotionOpenMs = 350;
constexpr int kMouthMotionHoldMs = 400;
constexpr int kMouthMotionCloseMs = 500;
constexpr int kMouthMotionClosedHoldMs = 350;
constexpr int kMouthMotionSettleMs = 300;
constexpr int kMouthMotionClosedAmount = -144;
constexpr char kTag[] = "MochanDisplay";

uint16_t TimedProgress(uint16_t from, uint16_t target, int64_t started_us, int duration_ms,
                       int64_t now_us) {
    const int distance = std::abs(static_cast<int>(target) - static_cast<int>(from));
    if (distance == 0) {
        return target;
    }
    const int64_t duration_us =
        std::max<int64_t>(1, static_cast<int64_t>(duration_ms) * 1000 * distance / 256);
    const int64_t elapsed_us = std::clamp<int64_t>(now_us - started_us, 0, duration_us);
    const int progress =
        static_cast<int>(from) +
        (static_cast<int>(target) - static_cast<int>(from)) * elapsed_us / duration_us;
    return static_cast<uint16_t>(std::clamp(progress, 0, 256));
}

int TimedValue(int from, int target, int64_t started_us, int duration_ms, int64_t now_us) {
    if (from == target) {
        return target;
    }
    const int64_t duration_us = static_cast<int64_t>(duration_ms) * 1000;
    const int64_t elapsed_us = std::clamp<int64_t>(now_us - started_us, 0, duration_us);
    return from + (target - from) * elapsed_us / duration_us;
}

}  // namespace

MochanDisplay::MochanDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y, bool mirror_x,
                             bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                    swap_xy) {
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) { static_cast<MochanDisplay*>(arg)->HidePreview(); },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "mochan_preview",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &preview_timer_));
}

MochanDisplay::~MochanDisplay() {
    if (preview_timer_ != nullptr) {
        esp_timer_stop(preview_timer_);
        esp_timer_delete(preview_timer_);
    }
    if (eye_timer_ != nullptr) {
        lv_timer_delete(eye_timer_);
    }
    if (notification_timer_ != nullptr) {
        lv_timer_delete(notification_timer_);
    }
    for (auto* raster : {&left_raster_, &right_raster_}) {
        if (raster->pixels != nullptr) {
            lv_image_cache_drop(&raster->descriptor);
            heap_caps_free(raster->pixels);
        }
    }
    if (mouth_raster_.pixels != nullptr) {
        lv_image_cache_drop(&mouth_raster_.descriptor);
        heap_caps_free(mouth_raster_.pixels);
    }
}

void MochanDisplay::SetupUI() {
    if (setup_ui_called_) {
        return;
    }

    Display::SetupUI();
    DisplayLockGuard lock(this);
    auto* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, kFaceBackground, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(container_, kFaceBackground, 0);
    lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_scrollbar_mode(container_, LV_SCROLLBAR_MODE_OFF);

    face_ = lv_obj_create(container_);
    lv_obj_set_size(face_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(face_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(face_, 0, 0);
    lv_obj_set_style_pad_all(face_, 0, 0);
    lv_obj_set_scrollbar_mode(face_, LV_SCROLLBAR_MODE_OFF);

    const bool raster_eyes = InitializeEyeRasters();
    left_eye_ = raster_eyes ? lv_image_create(face_) : lv_obj_create(face_);
    right_eye_ = raster_eyes ? lv_image_create(face_) : lv_obj_create(face_);
    if (raster_eyes) {
        lv_image_set_src(left_eye_, &left_raster_.descriptor);
        lv_image_set_src(right_eye_, &right_raster_.descriptor);
    }
    for (auto* eye : {left_eye_, right_eye_}) {
        lv_obj_set_style_bg_color(eye, kBrass, 0);
        lv_obj_set_style_bg_opa(eye, raster_eyes ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(eye, 0, 0);
        lv_obj_set_style_shadow_width(eye, 0, 0);
        lv_obj_set_style_radius(eye, 22, 0);
    }

    left_eyelid_ = lv_obj_create(face_);
    right_eyelid_ = lv_obj_create(face_);
    for (auto* eyelid : {left_eyelid_, right_eyelid_}) {
        lv_obj_set_style_bg_color(eyelid, kEyelidShadow, 0);
        lv_obj_set_style_bg_opa(eyelid, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(eyelid, 0, 0);
        lv_obj_set_style_radius(eyelid, 22, 0);
        lv_obj_set_style_shadow_width(eyelid, 0, 0);
        lv_obj_move_to_index(eyelid, 0);
    }

    if (InitializeMouthRaster()) {
        mouth_ = lv_image_create(face_);
        lv_image_set_src(mouth_, &mouth_raster_.descriptor);
        lv_obj_set_style_transform_pivot_x(mouth_, MouthRaster::kWidth / 2, 0);
        lv_obj_set_style_transform_pivot_y(mouth_, MouthRaster::kHeight / 2, 0);
        lv_obj_align(mouth_, LV_ALIGN_CENTER, 0, 25);
        lv_obj_add_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
    }

    wifi_icon_ = lv_label_create(container_);
    lv_label_set_text(wifi_icon_, MATERIAL_SYMBOLS_WIFI_OFF);
    lv_obj_set_style_text_color(wifi_icon_, kSpinnerTrack, 0);
    auto* initial_theme = static_cast<LvglTheme*>(current_theme_);
    if (initial_theme != nullptr && initial_theme->icon_font() != nullptr) {
        lv_obj_set_style_text_font(wifi_icon_, initial_theme->icon_font()->font(), 0);
    }
    lv_obj_set_style_transform_scale(wifi_icon_, 192, 0);
    lv_obj_set_style_transform_pivot_x(wifi_icon_, 0, 0);
    lv_obj_set_style_transform_pivot_y(wifi_icon_, 0, 0);
    lv_obj_align(wifi_icon_, LV_ALIGN_TOP_LEFT, 8, 7);

    status_dot_ = lv_obj_create(container_);
    lv_obj_set_size(status_dot_, 7, 7);
    lv_obj_set_style_radius(status_dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(status_dot_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(status_dot_, kSpinnerTrack, 0);
    lv_obj_set_style_border_width(status_dot_, 0, 0);
    lv_obj_align(status_dot_, LV_ALIGN_TOP_RIGHT, -9, 9);

    battery_icon_ = lv_label_create(container_);
    lv_label_set_text(battery_icon_, MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_FULL);
    lv_obj_set_style_text_color(battery_icon_, kBrassHighlight, 0);
    if (initial_theme != nullptr && initial_theme->icon_font() != nullptr) {
        lv_obj_set_style_text_font(battery_icon_, initial_theme->icon_font()->font(), 0);
    }
    // The battery glyph has considerably more internal whitespace than the Wi-Fi
    // glyph, so it needs a larger transform to have the same apparent size.
    lv_obj_set_style_transform_scale(battery_icon_, 224, 0);
    lv_obj_set_style_transform_pivot_x(battery_icon_, 0, 0);
    lv_obj_set_style_transform_pivot_y(battery_icon_, 0, 0);
    // Wi-Fi, battery and percentage share the same visual center line. Their
    // offsets differ because each glyph/font is rendered at a different scale.
    lv_obj_align(battery_icon_, LV_ALIGN_TOP_LEFT, 31, 6);
    lv_obj_add_flag(battery_icon_, LV_OBJ_FLAG_HIDDEN);

    battery_status_ = lv_label_create(container_);
    lv_label_set_text(battery_status_, "");
    lv_obj_set_style_text_color(battery_status_, kBrassHighlight, 0);
    if (initial_theme != nullptr && initial_theme->text_font() != nullptr) {
        lv_obj_set_style_text_font(battery_status_, initial_theme->text_font()->font(), 0);
    }
    lv_obj_set_style_transform_scale(battery_status_, 160, 0);
    lv_obj_set_style_transform_pivot_x(battery_status_, 0, 0);
    lv_obj_set_style_transform_pivot_y(battery_status_, 0, 0);
    lv_obj_align(battery_status_, LV_ALIGN_TOP_LEFT, 54, 7);

    response_box_ = lv_obj_create(container_);
    lv_obj_set_size(response_box_, width_ - 20, 112);
    lv_obj_align(response_box_, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(response_box_, lv_color_hex(0x11100d), 0);
    lv_obj_set_style_bg_opa(response_box_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(response_box_, kSpinnerTrack, 0);
    lv_obj_set_style_border_width(response_box_, 1, 0);
    lv_obj_set_style_radius(response_box_, 8, 0);
    lv_obj_set_style_pad_left(response_box_, 9, 0);
    lv_obj_set_style_pad_right(response_box_, 9, 0);
    lv_obj_set_style_pad_top(response_box_, 11, 0);
    lv_obj_set_style_pad_bottom(response_box_, 7, 0);
    lv_obj_set_scroll_dir(response_box_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(response_box_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(response_box_, LV_OBJ_FLAG_HIDDEN);

    subtitle_ = lv_label_create(response_box_);
    // Transforms do not participate in LVGL layout. Compensate the logical
    // width so the visually scaled label still fills the response viewport.
    const int response_text_width =
        ((width_ - 38) * LV_SCALE_NONE + kResponseTextScale - 1) / kResponseTextScale;
    lv_obj_set_width(subtitle_, response_text_width);
    lv_obj_set_height(subtitle_, LV_SIZE_CONTENT);
    lv_obj_set_style_text_color(subtitle_, kBrassHighlight, 0);
    lv_obj_set_style_text_align(subtitle_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_transform_scale(subtitle_, kResponseTextScale, 0);
    lv_obj_set_style_transform_pivot_x(subtitle_, 0, 0);
    lv_obj_set_style_transform_pivot_y(subtitle_, 0, 0);
    lv_label_set_long_mode(subtitle_, LV_LABEL_LONG_WRAP);
    lv_obj_align(subtitle_, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(subtitle_, LV_OBJ_FLAG_HIDDEN);

    notification_ = lv_label_create(response_box_);
    lv_obj_set_width(notification_, response_text_width);
    lv_obj_set_style_text_color(notification_, kBrassHighlight, 0);
    lv_obj_set_style_text_align(notification_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_transform_scale(notification_, 210, 0);
    lv_obj_set_style_transform_pivot_x(notification_, 0, 0);
    lv_obj_set_style_transform_pivot_y(notification_, 0, 0);
    lv_label_set_long_mode(notification_, LV_LABEL_LONG_WRAP);
    lv_obj_align(notification_, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(notification_, LV_OBJ_FLAG_HIDDEN);

    camera_image_ = lv_image_create(response_box_);
    lv_obj_center(camera_image_);
    lv_obj_add_flag(camera_image_, LV_OBJ_FLAG_HIDDEN);

    SetFaceState(FaceState::kIdle);
    eye_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* display = static_cast<MochanDisplay*>(lv_timer_get_user_data(timer));
            display->AdvanceEyeAnimation();
        },
        kFaceAnimationPeriodMs, this);
}

void MochanDisplay::SetFaceState(FaceState state) {
    if (face_state_ == state) {
        UpdateStatusDot();
        return;
    }
    animation_phase_ = 0;
    blink_step_ = 0;
    blink_countdown_ = static_cast<uint16_t>(80 + esp_random() % 61);
    face_state_ = state;
    UpdateStatusDot();
    std::string emotion;
    {
        std::lock_guard<std::mutex> state_lock(emotion_mutex_);
        emotion = current_emotion_;
    }
    UpdateEyes(0, IsIdleEligible(emotion));
}

void MochanDisplay::UpdateStatusDot() {
    if (status_dot_ == nullptr) {
        return;
    }

    // Matches the original Mochan status language: green idle, yellow
    // listening, blue speaking. ASR preparation/processing uses a brighter
    // coral red so it is visibly busy rather than ready to hear speech.
    lv_color_t color = lv_color_hex(0x53c58b);
    if (status_dot_busy_) {
        color = lv_color_hex(0xff6257);
    } else if (activity_state_ == FaceState::kListening) {
        color = lv_color_hex(0xf0c94b);
    } else if (activity_state_ == FaceState::kSpeaking) {
        color = lv_color_hex(0x4aa3ff);
    } else if (activity_state_ == FaceState::kThinking) {
        color = lv_color_hex(0x8c7140);
    }
    lv_obj_set_style_bg_color(status_dot_, color, 0);
}

void MochanDisplay::ShowResponseBox() {
    FreezeMouthForExit();
    CancelIdleScheduler(true);
    response_box_requested_ = true;
    const int64_t now_us = esp_timer_get_time();
    SetFaceLayoutTarget(0, now_us);
    AdvanceResponseBoxTransition(now_us);
}

void MochanDisplay::HideResponseBox() {
    response_box_requested_ = false;
    AdvanceResponseBoxTransition(esp_timer_get_time());
}

bool MochanDisplay::AllowsNaturalBlink(FaceState state) {
    // Explicit expressions own their eyelids; an unrelated blink must not
    // overwrite a wink, squint or asymmetric pose.
    return state == FaceState::kIdle || state == FaceState::kListening ||
           state == FaceState::kSpeaking || state >= FaceState::kLookLeft;
}

bool MochanDisplay::CanShowFullFace(const std::string& emotion) const {
    if (mouth_ == nullptr || activity_state_ != FaceState::kIdle || response_box_ == nullptr ||
        response_box_requested_ || response_box_progress_ > kFaceReturnStartResponseProgress) {
        return false;
    }
    return HasMouthGeometry(emotion);
}

bool MochanDisplay::IsIdleEligible(const std::string& emotion) const {
    return CanShowFullFace(emotion) && response_box_progress_ == 0 && !emotion_active_ &&
           splash_ == nullptr &&
           (camera_image_ == nullptr || lv_obj_has_flag(camera_image_, LV_OBJ_FLAG_HIDDEN)) &&
           (notification_ == nullptr || lv_obj_has_flag(notification_, LV_OBJ_FLAG_HIDDEN));
}

void MochanDisplay::SetFaceLayoutTarget(uint16_t target, int64_t now_us) {
    if (target == face_layout_target_) {
        return;
    }
    face_layout_from_ = face_layout_progress_;
    face_layout_target_ = target;
    face_layout_transition_started_us_ = now_us;
}

void MochanDisplay::UpdateFaceLayoutTarget(const std::string& emotion, int64_t now_us) {
    const uint16_t target = CanShowFullFace(emotion) ? 256 : 0;
    const std::string& layout_emotion =
        target == 0 && !exiting_mouth_emotion_.empty() ? exiting_mouth_emotion_ : emotion;
    int idle_eye_offset_y = 0;
    if (GetMouthIdleEyeOffset(layout_emotion, idle_eye_offset_y)) {
        face_layout_full_offset_y_ = TimedValue(
            face_layout_full_offset_from_y_, face_layout_full_offset_target_y_,
            face_layout_emotion_transition_started_us_, kEmotionLayoutTransitionMs, now_us);
        if (idle_eye_offset_y != face_layout_full_offset_target_y_) {
            if (face_layout_progress_ == 0) {
                face_layout_full_offset_y_ = idle_eye_offset_y;
                face_layout_full_offset_from_y_ = idle_eye_offset_y;
                face_layout_full_offset_target_y_ = idle_eye_offset_y;
            } else {
                face_layout_full_offset_from_y_ = face_layout_full_offset_y_;
                face_layout_full_offset_target_y_ = idle_eye_offset_y;
            }
            face_layout_emotion_transition_started_us_ = now_us;
        }
    }
    SetFaceLayoutTarget(target, now_us);
    if (target != 0 && mouth_ != nullptr) {
        exiting_mouth_emotion_.clear();
        if (lv_obj_has_flag(mouth_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_remove_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void MochanDisplay::FreezeMouthForExit() {
    if (face_layout_progress_ == 0 || !exiting_mouth_emotion_.empty()) {
        return;
    }
    std::lock_guard<std::mutex> state_lock(emotion_mutex_);
    if (HasMouthGeometry(current_emotion_)) {
        exiting_mouth_emotion_ = current_emotion_;
    }
}

void MochanDisplay::AdvanceResponseBoxTransition(int64_t now_us) {
    if (response_box_ == nullptr) {
        return;
    }

    // Let the panel fade overlap the latter part of the face movement. On the
    // way back, the face starts returning while the last of the panel fades.
    const uint16_t target =
        response_box_requested_ && face_layout_progress_ <= kResponseFadeInStartFaceProgress ? 256
                                                                                             : 0;
    if (target != response_box_target_) {
        response_box_from_ = response_box_progress_;
        response_box_target_ = target;
        response_box_transition_started_us_ = now_us;
        if (target != 0 && response_box_progress_ == 0) {
            lv_obj_remove_flag(response_box_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    response_box_progress_ =
        TimedProgress(response_box_from_, response_box_target_, response_box_transition_started_us_,
                      kResponseBoxFadeMs, now_us);

    const uint32_t progress = response_box_progress_;
    const uint32_t eased = progress * progress * (3 * 256 - 2 * progress) / (256 * 256);
    const uint8_t opacity = eased * LV_OPA_COVER / 256;
    if (response_box_opacity_ != opacity) {
        response_box_opacity_ = opacity;
        lv_obj_set_style_opa(response_box_, opacity, 0);
    }
    if (preview_show_pending_ && response_box_progress_ == 256) {
        preview_show_pending_ = false;
        if (camera_image_cached_ != nullptr) {
            lv_obj_remove_flag(camera_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(camera_image_);
            lv_obj_invalidate(camera_image_);
        }
        esp_timer_stop(preview_timer_);
        ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, kPreviewDurationMs * 1000));
    }
    if (!response_box_requested_ && response_box_progress_ == 0) {
        if (!lv_obj_has_flag(response_box_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(response_box_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void MochanDisplay::AdvanceFaceLayout(int64_t now_us) {
    face_layout_full_offset_y_ =
        TimedValue(face_layout_full_offset_from_y_, face_layout_full_offset_target_y_,
                   face_layout_emotion_transition_started_us_, kEmotionLayoutTransitionMs, now_us);
    face_layout_progress_ =
        TimedProgress(face_layout_from_, face_layout_target_, face_layout_transition_started_us_,
                      kFaceLayoutTransitionMs, now_us);
    const uint32_t progress = face_layout_progress_;
    const uint32_t eased = progress * progress * (3 * 256 - 2 * progress) / (256 * 256);
    face_layout_offset_y_ = face_layout_full_offset_y_ * static_cast<int>(eased) / 256;
}

void MochanDisplay::CancelIdleScheduler(bool restart_session) {
    const bool restore_activity = idle_override_active_;
    idle_override_active_ = false;
    yawn_active_ = false;
    yawn_amount_ = 0;
    yawn_started_ms_ = 0;
    mouth_motion_active_ = false;
    mouth_motion_amount_ = 0;
    mouth_motion_started_ms_ = 0;
    next_mouth_motion_ms_ = 0;
    next_idle_emotion_ms_ = 0;
    if (restart_session) {
        idle_session_started_ms_ = 0;
        idle_motion_phase_ = 0;
        last_idle_emotion_.clear();
        idle_repeat_count_ = 0;
    }
    if (restore_activity) {
        const char* activity_emotion = "neutral";
        if (activity_state_ == FaceState::kListening) {
            activity_emotion = "listening";
        } else if (activity_state_ == FaceState::kSpeaking) {
            activity_emotion = "speaking";
        } else if (activity_state_ == FaceState::kThinking) {
            activity_emotion = "thinking";
        }
        {
            std::lock_guard<std::mutex> state_lock(emotion_mutex_);
            current_emotion_ = activity_emotion;
        }
        SetFaceState(activity_state_);
    }
}

void MochanDisplay::AdvanceIdleMouthAnimation(bool idle_eligible) {
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (!idle_eligible || yawn_active_) {
        mouth_motion_active_ = false;
        mouth_motion_amount_ = 0;
        mouth_motion_started_ms_ = 0;
        if (!idle_eligible) {
            next_mouth_motion_ms_ = 0;
        }
        return;
    }

    if (!mouth_motion_active_) {
        if (next_mouth_motion_ms_ == 0) {
            const int interval =
                kMouthMotionIntervalMinMs +
                esp_random() % (kMouthMotionIntervalMaxMs - kMouthMotionIntervalMinMs + 1);
            next_mouth_motion_ms_ = now_ms + interval;
        }
        if (now_ms < next_mouth_motion_ms_) {
            return;
        }
        mouth_motion_active_ = true;
        mouth_motion_started_ms_ = now_ms;
    }

    const int64_t elapsed = now_ms - mouth_motion_started_ms_;
    if (elapsed < kMouthMotionOpenMs) {
        mouth_motion_amount_ = static_cast<int16_t>(elapsed * 256 / kMouthMotionOpenMs);
    } else if (elapsed < kMouthMotionOpenMs + kMouthMotionHoldMs) {
        mouth_motion_amount_ = 256;
    } else if (elapsed < kMouthMotionOpenMs + kMouthMotionHoldMs + kMouthMotionCloseMs) {
        const int64_t close_elapsed = elapsed - kMouthMotionOpenMs - kMouthMotionHoldMs;
        mouth_motion_amount_ = static_cast<int16_t>(256 + (kMouthMotionClosedAmount - 256) *
                                                              close_elapsed / kMouthMotionCloseMs);
    } else if (elapsed < kMouthMotionOpenMs + kMouthMotionHoldMs + kMouthMotionCloseMs +
                             kMouthMotionClosedHoldMs) {
        mouth_motion_amount_ = kMouthMotionClosedAmount;
    } else if (elapsed < kMouthMotionOpenMs + kMouthMotionHoldMs + kMouthMotionCloseMs +
                             kMouthMotionClosedHoldMs + kMouthMotionSettleMs) {
        const int64_t settle_elapsed = elapsed - kMouthMotionOpenMs - kMouthMotionHoldMs -
                                       kMouthMotionCloseMs - kMouthMotionClosedHoldMs;
        mouth_motion_amount_ =
            static_cast<int16_t>(kMouthMotionClosedAmount *
                                 (kMouthMotionSettleMs - settle_elapsed) / kMouthMotionSettleMs);
    } else {
        mouth_motion_active_ = false;
        mouth_motion_amount_ = 0;
        mouth_motion_started_ms_ = 0;
        const int interval =
            kMouthMotionIntervalMinMs +
            esp_random() % (kMouthMotionIntervalMaxMs - kMouthMotionIntervalMinMs + 1);
        next_mouth_motion_ms_ = now_ms + interval;
    }
}

void MochanDisplay::ApplyIdleEmotion(const char* emotion) {
    FaceState state = FaceState::kIdle;
    if (std::strcmp(emotion, "happy") == 0) {
        state = FaceState::kHappy;
    } else if (std::strcmp(emotion, "bored") == 0) {
        state = FaceState::kCool;
    } else if (std::strcmp(emotion, "sleepy") == 0) {
        state = FaceState::kSleepy;
    } else if (std::strcmp(emotion, "surprised") == 0) {
        state = FaceState::kSurprised;
    }
    {
        std::lock_guard<std::mutex> state_lock(emotion_mutex_);
        current_emotion_ = emotion;
    }
    idle_override_active_ = std::strcmp(emotion, "neutral") != 0;
    SetFaceState(state);
}

void MochanDisplay::AdvanceIdleScheduler(std::string& emotion) {
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (!IsIdleEligible(emotion)) {
        const bool restores_activity_emotion = idle_override_active_;
        CancelIdleScheduler(true);
        if (restores_activity_emotion) {
            std::lock_guard<std::mutex> state_lock(emotion_mutex_);
            emotion = current_emotion_;
        }
        return;
    }
    if (idle_session_started_ms_ == 0) {
        idle_session_started_ms_ = now_ms;
        next_idle_emotion_ms_ = now_ms + kIdleStartDelayMs;
        return;
    }

    if (yawn_active_) {
        const int64_t elapsed = now_ms - yawn_started_ms_;
        if (elapsed < kYawnOpenMs) {
            yawn_amount_ = static_cast<uint16_t>(elapsed * 256 / kYawnOpenMs);
        } else if (elapsed < kYawnOpenMs + kYawnHoldMs) {
            yawn_amount_ = 256;
        } else if (elapsed < kYawnOpenMs + kYawnHoldMs + kYawnCloseMs) {
            yawn_amount_ = static_cast<uint16_t>(
                (kYawnOpenMs + kYawnHoldMs + kYawnCloseMs - elapsed) * 256 / kYawnCloseMs);
        } else if (elapsed < kYawnOpenMs + kYawnHoldMs + kYawnCloseMs + kYawnSettleMs) {
            yawn_amount_ = 0;
        } else {
            yawn_active_ = false;
            yawn_amount_ = 0;
            next_idle_emotion_ms_ = now_ms + kIdleSleepyHoldMinMs;
        }
        return;
    }
    if (now_ms < next_idle_emotion_ms_) {
        return;
    }

    struct WeightedEmotion {
        const char* name;
        uint8_t weight;
    };
    constexpr std::array<WeightedEmotion, 5> early = {
        {{"neutral", 56}, {"bored", 25}, {"sleepy", 0}, {"happy", 16}, {"surprised", 3}}};
    constexpr std::array<WeightedEmotion, 5> settled = {
        {{"neutral", 31}, {"bored", 36}, {"sleepy", 23}, {"happy", 8}, {"surprised", 2}}};
    constexpr std::array<WeightedEmotion, 5> long_idle = {
        {{"neutral", 15}, {"bored", 30}, {"sleepy", 50}, {"happy", 4}, {"surprised", 1}}};
    const int64_t idle_elapsed = now_ms - idle_session_started_ms_;
    const auto& choices = idle_elapsed < kIdleEarlyStageMs
                              ? early
                              : (idle_elapsed < kIdleSleepyStageMs ? settled : long_idle);
    const char* selected = "neutral";
    for (int attempt = 0; attempt < 3; ++attempt) {
        int pick = esp_random() % 100;
        for (const auto& choice : choices) {
            if (pick < choice.weight) {
                selected = choice.name;
                break;
            }
            pick -= choice.weight;
        }
        if (last_idle_emotion_ != selected || idle_repeat_count_ < 2) {
            break;
        }
    }
    if (last_idle_emotion_ == selected && idle_repeat_count_ >= 2) {
        selected = last_idle_emotion_ == "bored" ? "neutral" : "bored";
    }
    if (last_idle_emotion_ == selected) {
        ++idle_repeat_count_;
    } else {
        last_idle_emotion_ = selected;
        idle_repeat_count_ = 1;
    }

    ApplyIdleEmotion(selected);
    emotion = selected;
    int hold_min = kIdleEmotionHoldMinMs;
    int hold_max = kIdleEmotionHoldMaxMs;
    if (std::strcmp(selected, "happy") == 0) {
        hold_min = kIdleHappyHoldMinMs;
        hold_max = kIdleHappyHoldMaxMs;
    } else if (std::strcmp(selected, "surprised") == 0) {
        hold_min = hold_max = kIdleSurprisedHoldMs;
    } else if (std::strcmp(selected, "sleepy") == 0) {
        hold_min = kIdleSleepyHoldMinMs;
        hold_max = kIdleSleepyHoldMaxMs;
        const bool yawn_ready = idle_elapsed >= kYawnMinimumIdleMs &&
                                (last_yawn_ms_ == 0 || now_ms - last_yawn_ms_ >= kYawnCooldownMs);
        if (yawn_ready && esp_random() % 5 == 0) {
            yawn_active_ = true;
            yawn_started_ms_ = now_ms;
            last_yawn_ms_ = now_ms;
        }
    }
    const int hold_range = std::max(1, hold_max - hold_min + 1);
    next_idle_emotion_ms_ = now_ms + hold_min + esp_random() % hold_range;
}

void MochanDisplay::AdvanceEyeAnimation() {
    const int64_t callback_started_us = esp_timer_get_time();
    const int64_t frame_interval_us =
        last_animation_callback_us_ == 0 ? 0 : callback_started_us - last_animation_callback_us_;
    last_animation_callback_us_ = callback_started_us;
    max_frame_interval_us_ = std::max(max_frame_interval_us_, frame_interval_us);
    if (splash_ != nullptr) {
        RecordAnimationTiming(callback_started_us, frame_interval_us);
        return;
    }
    std::string emotion;
    {
        std::lock_guard<std::mutex> state_lock(emotion_mutex_);
        emotion = current_emotion_;
    }
    ++animation_phase_;
    AdvanceIdleScheduler(emotion);
    const bool idle_eligible = IsIdleEligible(emotion);
    if (idle_eligible) {
        ++idle_motion_phase_;
    }
    AdvanceIdleMouthAnimation(idle_eligible);
    UpdateFaceLayoutTarget(emotion, callback_started_us);
    AdvanceFaceLayout(callback_started_us);
    AdvanceResponseBoxTransition(callback_started_us);
    // If the response fade crossed the return threshold in this callback,
    // start the face transition timestamp now rather than one frame later.
    UpdateFaceLayoutTarget(emotion, callback_started_us);
    uint8_t blink_amount = 0;
    if (AllowsNaturalBlink(face_state_) || idle_eligible) {
        static constexpr uint8_t kBlinkCurve[] = {35, 75, 100, 70, 30};
        if (blink_step_ != 0) {
            blink_amount = kBlinkCurve[blink_step_ - 1];
            ++blink_step_;
            if (blink_step_ > sizeof(kBlinkCurve)) {
                blink_step_ = 0;
                blink_countdown_ = static_cast<uint16_t>(80 + esp_random() % 61);
            }
        } else if (blink_countdown_ > 0) {
            --blink_countdown_;
        } else {
            blink_step_ = 1;
        }
    }
    if (yawn_active_) {
        blink_amount = std::max<uint8_t>(blink_amount, yawn_amount_ * 72 / 256);
    }
    const int64_t face_work_started_us = esp_timer_get_time();
    UpdateEyes(blink_amount, idle_eligible);
    UpdateMouth(blink_amount, emotion);
    max_face_work_us_ = std::max(max_face_work_us_, esp_timer_get_time() - face_work_started_us);
    // Keep typewriter work on the face frame clock. Time-based glyph credit
    // preserves a steady reveal when an occasional display frame arrives late.
    if (typing_active_) {
        const int64_t text_work_started_us = esp_timer_get_time();
        UpdateTyping(callback_started_us);
        max_text_work_us_ = std::max(max_text_work_us_, esp_timer_get_time() - text_work_started_us);
    }
    RecordAnimationTiming(callback_started_us, frame_interval_us);
}

void MochanDisplay::RecordAnimationTiming(int64_t callback_started_us, int64_t frame_interval_us) {
    const int64_t callback_duration_us = esp_timer_get_time() - callback_started_us;
    max_callback_duration_us_ = std::max(max_callback_duration_us_, callback_duration_us);
    if (last_performance_log_us_ == 0) {
        last_performance_log_us_ = callback_started_us;
        return;
    }
    if (callback_started_us - last_performance_log_us_ < kPerformanceLogIntervalUs) {
        return;
    }
    ESP_LOGI(kTag, "Face perf: frame=%lld us (max %lld), callback=%lld us (max %lld), "
                  "face_max=%lld us, text_max=%lld us",
             static_cast<long long>(frame_interval_us),
             static_cast<long long>(max_frame_interval_us_),
             static_cast<long long>(callback_duration_us),
             static_cast<long long>(max_callback_duration_us_),
             static_cast<long long>(max_face_work_us_),
             static_cast<long long>(max_text_work_us_));
    last_performance_log_us_ = callback_started_us;
    max_frame_interval_us_ = 0;
    max_callback_duration_us_ = 0;
    max_face_work_us_ = 0;
    max_text_work_us_ = 0;
}
