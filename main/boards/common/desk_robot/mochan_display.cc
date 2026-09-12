#include "mochan_display.h"

#include "assets/lang_config.h"
#include "display/lvgl_display/lvgl_theme.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_random.h>
#include <material_symbols.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace {
const lv_color_t kFaceBackground = LV_COLOR_MAKE(0x00, 0x00, 0x00);
const lv_color_t kBrass = LV_COLOR_MAKE(0xc6, 0xa1, 0x5b);
const lv_color_t kBrassHighlight = LV_COLOR_MAKE(0xe3, 0xc2, 0x7b);
const lv_color_t kEyelidShadow = LV_COLOR_MAKE(0x72, 0x55, 0x2b);
const lv_color_t kSpinnerTrack = LV_COLOR_MAKE(0x4b, 0x3b, 0x25);
constexpr int kPreviewDurationMs = 5000;
constexpr int kTypingPeriodMs = 42;
constexpr int kTypingFinishPeriodMs = 20;
constexpr int kResponseTextScale = 210;
constexpr char kTag[] = "MochanDisplay";

constexpr std::array<const char*, 33> kSupportedEmotions = {
    "neutral",    "happy",       "laughing",  "funny",     "sad",        "angry",   "crying",
    "loving",     "embarrassed", "surprised", "shocked",   "thinking",   "winking", "cool",
    "relaxed",    "delicious",   "kissy",     "confident", "sleepy",     "silly",   "confused",
    "suspicious", "shake",       "speaking",  "listening", "left",       "right",   "up",
    "down",       "up_left",     "up_right",  "down_left", "down_right",
};

bool EndsWithSentencePunctuation(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    const char last = text.back();
    return last == '.' || last == '!' || last == '?' || last == ':' || last == ';';
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
    if (typing_timer_ != nullptr) {
        lv_timer_delete(typing_timer_);
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

    left_eye_ = lv_obj_create(face_);
    right_eye_ = lv_obj_create(face_);
    for (auto* eye : {left_eye_, right_eye_}) {
        lv_obj_set_style_bg_color(eye, kBrass, 0);
        lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
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

    left_eye_line_shadow_ = lv_line_create(face_);
    right_eye_line_shadow_ = lv_line_create(face_);
    left_eye_line_ = lv_line_create(face_);
    right_eye_line_ = lv_line_create(face_);
    lv_line_set_points_mutable(left_eye_line_shadow_, left_eye_line_points_, kEyeLinePointCount);
    lv_line_set_points_mutable(left_eye_line_, left_eye_line_points_, kEyeLinePointCount);
    lv_line_set_points_mutable(right_eye_line_shadow_, right_eye_line_points_, kEyeLinePointCount);
    lv_line_set_points_mutable(right_eye_line_, right_eye_line_points_, kEyeLinePointCount);
    for (auto* line : {left_eye_line_shadow_, right_eye_line_shadow_}) {
        lv_obj_set_style_line_color(line, kEyelidShadow, 0);
        lv_obj_set_style_line_width(line, 11, 0);
        lv_obj_set_style_line_rounded(line, true, 0);
        lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
    }

    left_eye_accent_ = lv_obj_create(face_);
    right_eye_accent_ = lv_obj_create(face_);
    for (auto* accent : {left_eye_accent_, right_eye_accent_}) {
        lv_obj_set_style_bg_color(accent, kBrassHighlight, 0);
        lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(accent, 0, 0);
        lv_obj_set_style_radius(accent, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_shadow_width(accent, 0, 0);
        lv_obj_add_flag(accent, LV_OBJ_FLAG_HIDDEN);
    }
    for (auto* line : {left_eye_line_, right_eye_line_}) {
        lv_obj_set_style_line_color(line, kBrass, 0);
        lv_obj_set_style_line_width(line, 8, 0);
        lv_obj_set_style_line_rounded(line, true, 0);
        lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
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
    lv_obj_set_style_transform_scale(battery_icon_, 176, 0);
    lv_obj_align(battery_icon_, LV_ALIGN_TOP_LEFT, 31, 7);
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
    lv_obj_align(battery_status_, LV_ALIGN_TOP_LEFT, 47, 7);

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
        50, this);

    typing_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            static_cast<MochanDisplay*>(lv_timer_get_user_data(timer))->UpdateTyping();
        },
        kTypingPeriodMs, this);
    lv_timer_pause(typing_timer_);
}

void MochanDisplay::SetFaceState(FaceState state) {
    if (face_state_ != state) {
        animation_phase_ = 0;
        blink_step_ = 0;
        blink_countdown_ = static_cast<uint16_t>(80 + esp_random() % 61);
    }
    face_state_ = state;
    UpdateStatusDot();
    UpdateEyes(0);
}

void MochanDisplay::UpdateStatusDot() {
    if (status_dot_ == nullptr) {
        return;
    }

    // Matches the original Mochan status language: green idle, yellow
    // listening, blue speaking. Thinking keeps the quieter brass indicator.
    lv_color_t color = lv_color_hex(0x53c58b);
    if (activity_state_ == FaceState::kListening) {
        color = lv_color_hex(0xf0c94b);
    } else if (activity_state_ == FaceState::kSpeaking) {
        color = lv_color_hex(0x4aa3ff);
    } else if (activity_state_ == FaceState::kThinking) {
        color = lv_color_hex(0x8c7140);
    }
    lv_obj_set_style_bg_color(status_dot_, color, 0);
}

void MochanDisplay::ShowResponseBox() {
    if (response_box_ == nullptr || !lv_obj_has_flag(response_box_, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    lv_obj_remove_flag(response_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(response_box_, LV_OPA_TRANSP, 0);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, response_box_);
    lv_anim_set_values(&animation, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&animation, 200);
    LV_ANIM_SET_EASE_OUT_CUBIC(&animation);
    lv_anim_set_exec_cb(&animation, [](void* object, int32_t opacity) {
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(object), opacity, 0);
    });
    lv_anim_start(&animation);
}

bool MochanDisplay::AllowsNaturalBlink(FaceState state) {
    switch (state) {
        case FaceState::kHappy:
        case FaceState::kLaughing:
        case FaceState::kCrying:
        case FaceState::kLoving:
        case FaceState::kSurprised:
        case FaceState::kShocked:
        case FaceState::kWinking:
        case FaceState::kRelaxed:
        case FaceState::kDelicious:
        case FaceState::kKissy:
        case FaceState::kSleepy:
        case FaceState::kShake:
            return false;
        default:
            return true;
    }
}

void MochanDisplay::AdvanceEyeAnimation() {
    ++animation_phase_;
    uint8_t blink_amount = 0;
    if (AllowsNaturalBlink(face_state_)) {
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
    UpdateEyes(blink_amount);
}

void MochanDisplay::ApplyRoundedEye(lv_obj_t* eye, lv_obj_t* shadow, const EyeGeometry& geometry,
                                    uint8_t blink_amount) {
    const int height = std::max(7, geometry.height - (geometry.height - 7) * blink_amount / 100);
    const int inset_x = std::min(4, std::max(2, geometry.width / 8));
    const int inset_y = std::min(6, std::max(2, height / 4));

    lv_obj_remove_flag(shadow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(eye, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(shadow, geometry.width, height);
    lv_obj_set_style_transform_pivot_x(shadow, geometry.width / 2, 0);
    lv_obj_set_style_transform_pivot_y(shadow, height / 2, 0);
    lv_obj_set_style_transform_rotation(shadow, geometry.rotation, 0);
    lv_obj_align(shadow, LV_ALIGN_CENTER, geometry.x, geometry.y);

    const int bright_width = std::max(8, geometry.width - inset_x);
    const int bright_height = std::max(4, height - inset_y);
    lv_obj_set_size(eye, bright_width, bright_height);
    lv_obj_set_style_transform_pivot_x(eye, bright_width / 2, 0);
    lv_obj_set_style_transform_pivot_y(eye, bright_height / 2, 0);
    lv_obj_set_style_transform_rotation(eye, geometry.rotation, 0);
    lv_obj_align(eye, LV_ALIGN_CENTER, geometry.x - inset_x / 2, geometry.y - inset_y / 2);
}

void MochanDisplay::ApplyLineEye(lv_obj_t* line, lv_obj_t* shadow, lv_point_precise_t* points,
                                 const EyeGeometry& geometry, EyeShape shape, int stroke_width) {
    const int width = std::max(20, geometry.width);
    const int height = std::max(12, geometry.height);
    if (shape == EyeShape::kHeart) {
        static constexpr int kHeartX[kEyeLinePointCount] = {
            500, 400, 270, 140, 40,  10,  70,  190, 350, 500,
            650, 810, 930, 990, 960, 860, 730, 600, 500,
        };
        static constexpr int kHeartY[kEyeLinePointCount] = {
            260, 90, 20, 80, 230, 410, 560, 690, 820, 980, 820, 690, 560, 410, 230, 80, 20, 90, 260,
        };
        for (size_t index = 0; index < kEyeLinePointCount; ++index) {
            points[index] = {width * kHeartX[index] / 1000, height * kHeartY[index] / 1000};
        }
    } else {
        constexpr int kSpan = static_cast<int>(kEyeLinePointCount - 1);
        for (int index = 0; index <= kSpan; ++index) {
            const int x = 2 + index * (width - 4) / kSpan;
            const int centered = index * 2 - kSpan;
            const int curve = (height - 6) * centered * centered / (kSpan * kSpan);
            int y = height / 2;
            if (shape == EyeShape::kSmileArc) {
                y = 3 + curve;
            } else if (shape == EyeShape::kDroopArc) {
                y = height - 3 - curve;
            } else {
                y += centered * centered / (kSpan * kSpan / 2) - 1;
            }
            points[index] = {x, y};
        }
    }

    lv_obj_set_style_line_width(shadow, stroke_width + 4, 0);
    lv_obj_set_style_line_width(line, stroke_width, 0);
    for (auto* object : {shadow, line}) {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(object, width, height);
        lv_obj_set_style_transform_pivot_x(object, width / 2, 0);
        lv_obj_set_style_transform_pivot_y(object, height / 2, 0);
        lv_obj_set_style_transform_rotation(object, geometry.rotation, 0);
        lv_obj_invalidate(object);
    }
    lv_obj_align(shadow, LV_ALIGN_CENTER, geometry.x + 3, geometry.y + 4);
    lv_obj_align(line, LV_ALIGN_CENTER, geometry.x, geometry.y);
}

void MochanDisplay::UpdateEyes(uint8_t blink_amount) {
    if (left_eye_ == nullptr || right_eye_ == nullptr) {
        return;
    }

    struct EyeTarget {
        EyeGeometry geometry;
        EyeShape shape;
        int stroke_width = 12;
    };
    EyeTarget left{{74, 54, -48, -59, 0}, EyeShape::kRounded};
    EyeTarget right{{74, 54, 48, -59, 0}, EyeShape::kRounded};

    const auto triangle = [](uint16_t phase, int period, int amplitude) {
        const int position = phase % period;
        const int half = period / 2;
        const int ramp = position < half ? position : period - position;
        return ramp * amplitude * 2 / half - amplitude;
    };
    const int gentle = triangle(animation_phase_, 32, 2);

    switch (face_state_) {
        case FaceState::kListening:
            left.geometry = {78, 58 + gentle, -48, -60, 0};
            right.geometry = {78, 58 + gentle, 48, -60, 0};
            break;
        case FaceState::kSpeaking:
            left.geometry = {68, 46 + triangle(animation_phase_, 20, 3), -48, -59, 0};
            right.geometry = {68, 46 + triangle(animation_phase_, 20, 3), 48, -59, 0};
            break;
        case FaceState::kThinking:
            left.geometry = {68, 48, -57, -66, -20};
            right.geometry = {68, 48, 39, -66, -20};
            break;
        case FaceState::kHappy:
            left = {{64, 32, -45, -58 + gentle, 0}, EyeShape::kSmileArc};
            right = {{64, 32, 45, -58 + gentle, 0}, EyeShape::kSmileArc};
            break;
        case FaceState::kLaughing:
            left = {
                {70, 38, -46, -57 + triangle(animation_phase_, 18, 4), 0}, EyeShape::kSmileArc, 14};
            right = {
                {70, 38, 46, -57 + triangle(animation_phase_, 18, 4), 0}, EyeShape::kSmileArc, 14};
            break;
        case FaceState::kFunny: {
            const int sway = triangle(animation_phase_, 36, 5);
            left = {{56, 48, -48 + sway, -59, -60}, EyeShape::kRounded};
            right = {{64, 30, 49 + sway, -55, 30}, EyeShape::kSmileArc};
            break;
        }
        case FaceState::kAngry:
            left.geometry = {70, 42, -45, -56, 130};
            right.geometry = {70, 42, 45, -56, -130};
            break;
        case FaceState::kSad:
            left.geometry = {62, 40, -47, -50, -100};
            right.geometry = {62, 40, 47, -50, 100};
            break;
        case FaceState::kCrying:
            left = {{62, 31, -46, -49 + gentle, -20}, EyeShape::kDroopArc, 11};
            right = {{62, 31, 46, -49 + gentle, 20}, EyeShape::kDroopArc, 11};
            break;
        case FaceState::kLoving: {
            const int pulse = triangle(animation_phase_, 24, 3);
            left = {{50 + pulse, 44 + pulse, -42, -58, 0}, EyeShape::kHeart, 8};
            right = {{50 + pulse, 44 + pulse, 42, -58, 0}, EyeShape::kHeart, 8};
            break;
        }
        case FaceState::kEmbarrassed:
            left.geometry = {56, 36, -55, -48, -30};
            right.geometry = {56, 36, 41, -48, 30};
            break;
        case FaceState::kSurprised:
            left.geometry = {50, 66, -44, -57, 0};
            right.geometry = {50, 66, 44, -57, 0};
            break;
        case FaceState::kShocked: {
            const int jitter = (animation_phase_ / 2) % 2 == 0 ? -2 : 2;
            left.geometry = {62, 70, -43 + jitter, -56, 0};
            right.geometry = {62, 70, 43 + jitter, -56, 0};
            break;
        }
        case FaceState::kWinking:
            left = {{62, 20, -47, -55, -40}, EyeShape::kFlat, 12};
            right.geometry = {66, 50, 47, -58, 0};
            break;
        case FaceState::kCool:
            left = {{74, 22, -45, -56, 45}, EyeShape::kFlat, 13};
            right = {{74, 22, 45, -56, -45}, EyeShape::kFlat, 13};
            break;
        case FaceState::kRelaxed:
            left = {{66, 22, -46, -51, 0}, EyeShape::kFlat};
            right = {{66, 22, 46, -51, 0}, EyeShape::kFlat};
            break;
        case FaceState::kDelicious: {
            const int sway = triangle(animation_phase_, 30, 3);
            left = {{64, 30, -46 + sway, -57, 0}, EyeShape::kSmileArc};
            right.geometry = {58, 44, 46 + sway, -55, 20};
            break;
        }
        case FaceState::kKissy:
            left = {{56, 20, -49, -54, -70}, EyeShape::kFlat};
            right = {{56, 20, 49, -54, 70}, EyeShape::kFlat};
            break;
        case FaceState::kConfident:
            left.geometry = {74, 34, -44, -56, 70};
            right.geometry = {74, 34, 44, -56, -70};
            break;
        case FaceState::kSleepy:
            left = {{66, 22, -46, -48 + gentle, 0}, EyeShape::kDroopArc, 11};
            right = {{66, 22, 46, -48 + gentle, 0}, EyeShape::kDroopArc, 11};
            break;
        case FaceState::kSilly: {
            const int sway = triangle(animation_phase_, 26, 4);
            left.geometry = {52, 58, -49 + sway, -59, 70};
            right.geometry = {64, 38, 49 + sway, -52, -30};
            break;
        }
        case FaceState::kConfused:
            left.geometry = {62, 32, -48, -47, -80};
            right.geometry = {68, 48, 48, -61, -20};
            break;
        case FaceState::kSuspicious:
            left.geometry = {66, 24, -43, -52, -40};
            right.geometry = {64, 46, 51, -59, -10};
            break;
        case FaceState::kShake: {
            const int shake = (animation_phase_ / 2) % 2 == 0 ? -12 : 12;
            left.geometry.x += shake;
            right.geometry.x += shake;
            break;
        }
        case FaceState::kLookLeft:
            left.geometry.x -= 18;
            right.geometry.x -= 18;
            break;
        case FaceState::kLookRight:
            left.geometry.x += 18;
            right.geometry.x += 18;
            break;
        case FaceState::kLookUp:
            left.geometry.y = -72;
            right.geometry.y = -72;
            break;
        case FaceState::kLookDown:
            left.geometry.y = -34;
            right.geometry.y = -34;
            break;
        case FaceState::kLookUpLeft:
            left.geometry.x -= 14;
            right.geometry.x -= 14;
            left.geometry.y = -70;
            right.geometry.y = -70;
            break;
        case FaceState::kLookUpRight:
            left.geometry.x += 14;
            right.geometry.x += 14;
            left.geometry.y = -70;
            right.geometry.y = -70;
            break;
        case FaceState::kLookDownLeft:
            left.geometry.x -= 14;
            right.geometry.x -= 14;
            left.geometry.y = -35;
            right.geometry.y = -35;
            break;
        case FaceState::kLookDownRight:
            left.geometry.x += 14;
            right.geometry.x += 14;
            left.geometry.y = -35;
            right.geometry.y = -35;
            break;
        case FaceState::kIdle: {
            const int idle_phase = animation_phase_ % 160;
            if (idle_phase >= 35 && idle_phase < 65) {
                left.geometry.x -= 8;
                right.geometry.x -= 8;
                left.geometry.y += 2;
                right.geometry.y += 2;
            } else if (idle_phase >= 95 && idle_phase < 125) {
                left.geometry.x += 8;
                right.geometry.x += 8;
                left.geometry.y += 5;
                right.geometry.y += 5;
            }
            break;
        }
    }

    const auto smooth = [](int current, int target) {
        const int delta = target - current;
        if (delta >= -1 && delta <= 1) {
            return target;
        }
        const int step = delta / 3;
        return current + (step != 0 ? step : (delta > 0 ? 1 : -1));
    };
    const auto approach = [&smooth](EyeGeometry& current, const EyeGeometry& target) {
        current.width = smooth(current.width, target.width);
        current.height = smooth(current.height, target.height);
        current.x = smooth(current.x, target.x);
        current.y = smooth(current.y, target.y);
        current.rotation = smooth(current.rotation, target.rotation);
    };
    if (!eye_geometry_initialized_) {
        left_eye_geometry_ = left.geometry;
        right_eye_geometry_ = right.geometry;
        eye_geometry_initialized_ = true;
    } else {
        approach(left_eye_geometry_, left.geometry);
        approach(right_eye_geometry_, right.geometry);
    }

    const auto apply_eye = [this, blink_amount](lv_obj_t* eye, lv_obj_t* eye_shadow, lv_obj_t* line,
                                                lv_obj_t* line_shadow, lv_point_precise_t* points,
                                                const EyeGeometry& geometry, EyeShape shape,
                                                int stroke_width) {
        if (shape == EyeShape::kRounded) {
            lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(line_shadow, LV_OBJ_FLAG_HIDDEN);
            ApplyRoundedEye(eye, eye_shadow, geometry, blink_amount);
        } else {
            lv_obj_add_flag(eye, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(eye_shadow, LV_OBJ_FLAG_HIDDEN);
            ApplyLineEye(line, line_shadow, points, geometry, shape, stroke_width);
        }
    };
    apply_eye(left_eye_, left_eyelid_, left_eye_line_, left_eye_line_shadow_, left_eye_line_points_,
              left_eye_geometry_, left.shape, left.stroke_width);
    apply_eye(right_eye_, right_eyelid_, right_eye_line_, right_eye_line_shadow_,
              right_eye_line_points_, right_eye_geometry_, right.shape, right.stroke_width);

    for (auto* accent : {left_eye_accent_, right_eye_accent_}) {
        lv_obj_add_flag(accent, LV_OBJ_FLAG_HIDDEN);
    }
    if (face_state_ == FaceState::kCrying) {
        const int tear_fall = animation_phase_ % 18 / 3;
        for (auto* accent : {left_eye_accent_, right_eye_accent_}) {
            lv_obj_remove_flag(accent, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(accent, kBrassHighlight, 0);
            lv_obj_set_size(accent, 8, 15);
        }
        lv_obj_align(left_eye_accent_, LV_ALIGN_CENTER, -47, -18 + tear_fall);
        lv_obj_align(right_eye_accent_, LV_ALIGN_CENTER, 47, -18 + tear_fall);
    } else if (face_state_ == FaceState::kEmbarrassed) {
        for (auto* accent : {left_eye_accent_, right_eye_accent_}) {
            lv_obj_remove_flag(accent, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(accent, kEyelidShadow, 0);
            lv_obj_set_size(accent, 22, 5);
        }
        lv_obj_align(left_eye_accent_, LV_ALIGN_CENTER, -52, -20);
        lv_obj_align(right_eye_accent_, LV_ALIGN_CENTER, 44, -20);
    }
}

void MochanDisplay::SetStatus(const char* status) {
    if (status == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    if (std::strcmp(status, Lang::Strings::LISTENING) == 0) {
        activity_state_ = FaceState::kListening;
        emotion_active_ = false;
    } else if (std::strcmp(status, Lang::Strings::SPEAKING) == 0) {
        activity_state_ = FaceState::kSpeaking;
    } else if (std::strcmp(status, Lang::Strings::CONNECTING) == 0 ||
               std::strcmp(status, Lang::Strings::REGISTERING_NETWORK) == 0) {
        activity_state_ = FaceState::kThinking;
    } else {
        activity_state_ = FaceState::kIdle;
        emotion_active_ = false;
    }
    if (activity_state_ != FaceState::kSpeaking) {
        FinishTyping();
    }
    UpdateStatusDot();
    if (!emotion_active_) {
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

void MochanDisplay::RenderTypingText() {
    if (subtitle_ == nullptr) {
        return;
    }

    std::string rendered = "> ";
    rendered.append(typing_text_, 0, typing_position_);
    if (typing_cursor_visible_) {
        rendered.push_back('|');
    }
    lv_label_set_text(subtitle_, rendered.c_str());
    lv_obj_update_layout(response_box_);

    // LVGL calculates scrolling from the label's unscaled height, while the
    // response font is rendered smaller with a transform. Scrolling to the
    // normal bottom therefore lifts the final visible line toward the middle
    // of the panel. Use the transformed visual height so the last line rests
    // on the bottom edge of the response viewport.
    const int32_t label_height = lv_obj_get_height(subtitle_);
    const int32_t visual_height =
        (label_height * kResponseTextScale + LV_SCALE_NONE - 1) / LV_SCALE_NONE;
    const int32_t viewport_height = lv_obj_get_content_height(response_box_);
    const int32_t scroll_target = std::max<int32_t>(0, visual_height - viewport_height);
    if (scroll_target != response_scroll_target_) {
        const bool moves_forward = scroll_target > response_scroll_target_;
        response_scroll_target_ = scroll_target;
        // A new wrapped line should glide into view. Backward/reset movement
        // remains immediate so stale content never flashes during a new turn.
        lv_obj_scroll_to_y(response_box_, scroll_target, moves_forward ? LV_ANIM_ON : LV_ANIM_OFF);
    }
}

void MochanDisplay::StartTyping(const char* content) {
    const std::string incoming(content);
    if (incoming.empty()) {
        return;
    }

    if (typing_text_.empty()) {
        typing_text_ = incoming;
        typing_position_ = 0;
    } else if (incoming == typing_text_) {
        // Ignore a duplicate transcript update.
    } else if (incoming.size() > typing_text_.size() &&
               incoming.compare(0, typing_text_.size(), typing_text_) == 0) {
        // Some servers send the complete accumulated transcript on every
        // sentence update. Keep the already-rendered position and only extend
        // the target text.
        typing_text_ = incoming;
    } else {
        // sentence_start normally contains one sentence at a time. Preserve
        // text that is still being typed and continue from the same cursor.
        if (!std::isspace(static_cast<unsigned char>(typing_text_.back())) &&
            !std::isspace(static_cast<unsigned char>(incoming.front()))) {
            typing_text_ += EndsWithSentencePunctuation(typing_text_) ? " " : ". ";
        }
        typing_text_ += incoming;
    }

    typing_cursor_phase_ = 0;
    typing_cursor_visible_ = true;
    typing_active_ = true;
    typing_finishing_ = false;
    RenderTypingText();
    if (typing_timer_ != nullptr) {
        lv_timer_set_period(typing_timer_, kTypingPeriodMs);
        lv_timer_reset(typing_timer_);
        lv_timer_resume(typing_timer_);
    }
}

void MochanDisplay::UpdateTyping() {
    if (typing_text_.empty()) {
        ResetTyping();
        return;
    }

    if (typing_position_ < typing_text_.size()) {
        // Advance one complete UTF-8 code point so Vietnamese glyphs never
        // appear as temporarily corrupted byte sequences. Catch up faster when
        // multiple TTS sentences arrive before the display has finished.
        const size_t remaining = typing_text_.size() - typing_position_;
        const int glyphs_per_tick =
            typing_finishing_ ? 4 : (remaining > 72 ? 3 : (remaining > 32 ? 2 : 1));
        for (int glyph = 0; glyph < glyphs_per_tick && typing_position_ < typing_text_.size();
             ++glyph) {
            ++typing_position_;
            while (typing_position_ < typing_text_.size() &&
                   (static_cast<uint8_t>(typing_text_[typing_position_]) & 0xc0) == 0x80) {
                ++typing_position_;
            }
        }
    } else {
        typing_active_ = false;
        typing_finishing_ = false;
        typing_cursor_visible_ = false;
        if (typing_timer_ != nullptr) {
            lv_timer_pause(typing_timer_);
        }
    }

    if (typing_active_) {
        typing_cursor_phase_ = static_cast<uint8_t>((typing_cursor_phase_ + 1) % 12);
        typing_cursor_visible_ = typing_cursor_phase_ < 6;
    } else {
        typing_cursor_visible_ = false;
    }
    RenderTypingText();
}

void MochanDisplay::FinishTyping() {
    if (typing_text_.empty()) {
        return;
    }
    if (typing_position_ < typing_text_.size()) {
        typing_finishing_ = true;
        typing_active_ = true;
        typing_cursor_visible_ = true;
        if (typing_timer_ != nullptr) {
            lv_timer_set_period(typing_timer_, kTypingFinishPeriodMs);
            lv_timer_reset(typing_timer_);
            lv_timer_resume(typing_timer_);
        }
    } else {
        typing_finishing_ = false;
        typing_cursor_visible_ = false;
        typing_active_ = false;
        if (typing_timer_ != nullptr) {
            lv_timer_pause(typing_timer_);
        }
    }
    RenderTypingText();
}

void MochanDisplay::ResetTyping() {
    typing_text_.clear();
    typing_position_ = 0;
    typing_cursor_phase_ = 0;
    typing_cursor_visible_ = false;
    typing_active_ = false;
    typing_finishing_ = false;
    response_scroll_target_ = 0;
    if (response_box_ != nullptr) {
        lv_obj_scroll_to_y(response_box_, 0, LV_ANIM_OFF);
    }
    if (typing_timer_ != nullptr) {
        lv_timer_set_period(typing_timer_, kTypingPeriodMs);
        lv_timer_pause(typing_timer_);
    }
}

void MochanDisplay::SetTheme(Theme* theme) {
    // Asset updates call this after Wi-Fi connects.  LcdDisplay::SetTheme()
    // expects the stock status-bar widgets, which Mochan deliberately does
    // not create; delegating to it dereferences those null pointers.
    if (theme == nullptr) {
        return;
    }

    DisplayLockGuard lock(this);
    current_theme_ = theme;
    auto* lvgl_theme = static_cast<LvglTheme*>(theme);
    auto text_font_asset = lvgl_theme->text_font();
    if (text_font_asset == nullptr) {
        return;
    }
    auto* text_font = text_font_asset->font();
    if (text_font == nullptr) {
        return;
    }

    // Apply the downloaded/asset font to this custom hierarchy instead of
    // LcdDisplay's stock widgets. This also enables Vietnamese glyph fallback.
    lv_obj_set_style_text_font(lv_screen_active(), text_font, 0);
    if (container_ != nullptr) {
        lv_obj_set_style_text_font(container_, text_font, 0);
    }
    if (subtitle_ != nullptr) {
        lv_obj_set_style_text_font(subtitle_, text_font, 0);
    }
    if (notification_ != nullptr) {
        lv_obj_set_style_text_font(notification_, text_font, 0);
    }
    if (wifi_icon_ != nullptr && lvgl_theme->icon_font() != nullptr) {
        lv_obj_set_style_text_font(wifi_icon_, lvgl_theme->icon_font()->font(), 0);
    }
    if (battery_icon_ != nullptr && lvgl_theme->icon_font() != nullptr) {
        lv_obj_set_style_text_font(battery_icon_, lvgl_theme->icon_font()->font(), 0);
    }
    if (battery_status_ != nullptr) {
        lv_obj_set_style_text_font(battery_status_, text_font, 0);
    }
}

void MochanDisplay::SetWifiConnected(bool connected) {
    if (wifi_icon_ == nullptr) {
        return;
    }

    DisplayLockGuard lock(this);
    wifi_connected_ = connected;
    lv_label_set_text(wifi_icon_, connected ? MATERIAL_SYMBOLS_WIFI : MATERIAL_SYMBOLS_WIFI_OFF);
    lv_obj_set_style_text_color(wifi_icon_, connected ? kBrassHighlight : kSpinnerTrack, 0);
}

void MochanDisplay::SetBatteryStatus(int percent, float voltage_v, bool charging) {
    (void)voltage_v;
    DisplayLockGuard lock(this);
    if (battery_icon_ == nullptr || battery_status_ == nullptr) {
        return;
    }
    if (percent < 0) {
        lv_obj_add_flag(battery_icon_, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(battery_status_, "");
        return;
    }
    static constexpr std::array<const char*, 8> kBatteryIcons = {
        MATERIAL_SYMBOLS_BATTERY_ANDROID_0,       MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_1,
        MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_2, MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_3,
        MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_4, MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_5,
        MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_6, MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_FULL,
    };
    const int safe_percent = std::clamp(percent, 0, 100);
    const int icon_index =
        safe_percent <= 0 ? 0 : (safe_percent >= 100 ? 7 : 1 + (safe_percent - 1) * 6 / 99);
    lv_label_set_text(battery_icon_, charging ? MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_BOLT
                                              : kBatteryIcons[icon_index]);
    lv_obj_remove_flag(battery_icon_, LV_OBJ_FLAG_HIDDEN);
    char text[16] = {};
    std::snprintf(text, sizeof(text), "%d%%", safe_percent);
    lv_label_set_text(battery_status_, text);
    lv_obj_align(battery_icon_, LV_ALIGN_TOP_LEFT, 31, 7);
    lv_obj_align(battery_status_, LV_ALIGN_TOP_LEFT, 47, 7);
}

bool MochanDisplay::SetPanelMirror(bool mirror_x, bool mirror_y) {
    DisplayLockGuard lock(this);
    const esp_err_t error = esp_lcd_panel_mirror(panel_, mirror_x, mirror_y);
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "Cannot update display mirror: %s", esp_err_to_name(error));
        return false;
    }
    lv_obj_invalidate(lv_screen_active());
    return true;
}

void MochanDisplay::ShowNotification(const char* notification, int duration_ms) {
    if (notification == nullptr || notification_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    lv_label_set_text(notification_, notification);
    lv_obj_add_flag(subtitle_, LV_OBJ_FLAG_HIDDEN);
    ShowResponseBox();
    lv_obj_remove_flag(notification_, LV_OBJ_FLAG_HIDDEN);
    if (notification_timer_ != nullptr) {
        lv_timer_delete(notification_timer_);
    }
    notification_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            static_cast<MochanDisplay*>(lv_timer_get_user_data(timer))->HideNotification();
            lv_timer_delete(timer);
        },
        duration_ms, this);
}

void MochanDisplay::SetEmotion(const char* emotion) {
    if (emotion == nullptr) {
        return;
    }
    const std::string requested(emotion);
    {
        std::lock_guard<std::mutex> state_lock(emotion_mutex_);
        current_emotion_ = requested;
    }
    DisplayLockGuard lock(this);
    if (requested == "happy") {
        emotion_active_ = true;
        SetFaceState(FaceState::kHappy);
    } else if (requested == "laughing") {
        emotion_active_ = true;
        SetFaceState(FaceState::kLaughing);
    } else if (requested == "funny") {
        emotion_active_ = true;
        SetFaceState(FaceState::kFunny);
    } else if (requested == "angry") {
        emotion_active_ = true;
        SetFaceState(FaceState::kAngry);
    } else if (requested == "sad") {
        emotion_active_ = true;
        SetFaceState(FaceState::kSad);
    } else if (requested == "crying") {
        emotion_active_ = true;
        SetFaceState(FaceState::kCrying);
    } else if (requested == "loving") {
        emotion_active_ = true;
        SetFaceState(FaceState::kLoving);
    } else if (requested == "embarrassed") {
        emotion_active_ = true;
        SetFaceState(FaceState::kEmbarrassed);
    } else if (requested == "surprised") {
        emotion_active_ = true;
        SetFaceState(FaceState::kSurprised);
    } else if (requested == "shocked") {
        emotion_active_ = true;
        SetFaceState(FaceState::kShocked);
    } else if (requested == "winking") {
        emotion_active_ = true;
        SetFaceState(FaceState::kWinking);
    } else if (requested == "cool") {
        emotion_active_ = true;
        SetFaceState(FaceState::kCool);
    } else if (requested == "relaxed") {
        emotion_active_ = true;
        SetFaceState(FaceState::kRelaxed);
    } else if (requested == "delicious") {
        emotion_active_ = true;
        SetFaceState(FaceState::kDelicious);
    } else if (requested == "kissy") {
        emotion_active_ = true;
        SetFaceState(FaceState::kKissy);
    } else if (requested == "confident") {
        emotion_active_ = true;
        SetFaceState(FaceState::kConfident);
    } else if (requested == "sleepy") {
        emotion_active_ = true;
        SetFaceState(FaceState::kSleepy);
    } else if (requested == "silly") {
        emotion_active_ = true;
        SetFaceState(FaceState::kSilly);
    } else if (requested == "confused") {
        emotion_active_ = true;
        SetFaceState(FaceState::kConfused);
    } else if (requested == "suspicious") {
        emotion_active_ = true;
        SetFaceState(FaceState::kSuspicious);
    } else if (requested == "shake") {
        emotion_active_ = true;
        SetFaceState(FaceState::kShake);
    } else if (requested == "left") {
        emotion_active_ = true;
        SetFaceState(FaceState::kLookLeft);
    } else if (requested == "right") {
        emotion_active_ = true;
        SetFaceState(FaceState::kLookRight);
    } else if (requested == "up") {
        emotion_active_ = true;
        SetFaceState(FaceState::kLookUp);
    } else if (requested == "down") {
        emotion_active_ = true;
        SetFaceState(FaceState::kLookDown);
    } else if (requested == "up_left") {
        emotion_active_ = true;
        SetFaceState(FaceState::kLookUpLeft);
    } else if (requested == "up_right") {
        emotion_active_ = true;
        SetFaceState(FaceState::kLookUpRight);
    } else if (requested == "down_left") {
        emotion_active_ = true;
        SetFaceState(FaceState::kLookDownLeft);
    } else if (requested == "down_right") {
        emotion_active_ = true;
        SetFaceState(FaceState::kLookDownRight);
    } else if (requested == "thinking") {
        emotion_active_ = true;
        SetFaceState(FaceState::kThinking);
    } else if (requested == "speaking") {
        emotion_active_ = true;
        SetFaceState(FaceState::kSpeaking);
    } else if (requested == "listening") {
        emotion_active_ = true;
        SetFaceState(FaceState::kListening);
    } else if (requested == "neutral" || requested == "robot_2") {
        {
            std::lock_guard<std::mutex> state_lock(emotion_mutex_);
            current_emotion_ = "neutral";
        }
        emotion_active_ = false;
        SetFaceState(activity_state_);
    } else {
        ESP_LOGW(kTag, "Unsupported emotion: %s", emotion);
        {
            std::lock_guard<std::mutex> state_lock(emotion_mutex_);
            current_emotion_ = "neutral";
        }
        emotion_active_ = false;
        SetFaceState(activity_state_);
    }
}

std::string MochanDisplay::GetCurrentEmotion() const {
    std::lock_guard<std::mutex> lock(emotion_mutex_);
    return current_emotion_;
}

bool MochanDisplay::IsSupportedEmotion(const std::string& emotion) {
    return std::find(kSupportedEmotions.begin(), kSupportedEmotions.end(), emotion) !=
           kSupportedEmotions.end();
}

void MochanDisplay::SetChatMessage(const char* role, const char* content) {
    if (subtitle_ == nullptr || content == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    const bool is_assistant = role != nullptr && std::strcmp(role, "assistant") == 0;
    lv_obj_set_style_text_align(subtitle_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_transform_pivot_x(subtitle_, 0, 0);
    if (content[0] == '\0') {
        ResetTyping();
        lv_label_set_text(subtitle_, "");
        lv_obj_add_flag(subtitle_, LV_OBJ_FLAG_HIDDEN);
        if (lv_obj_has_flag(notification_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(response_box_, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        if (is_assistant) {
            StartTyping(content);
        } else {
            ResetTyping();
            lv_label_set_text(subtitle_, content);
        }
        lv_obj_add_flag(notification_, LV_OBJ_FLAG_HIDDEN);
        ShowResponseBox();
        lv_obj_remove_flag(subtitle_, LV_OBJ_FLAG_HIDDEN);
    }
}

void MochanDisplay::ClearChatMessages() { SetChatMessage("system", ""); }

void MochanDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    if (camera_image_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    if (image == nullptr) {
        esp_timer_stop(preview_timer_);
        camera_image_cached_.reset();
        lv_obj_add_flag(camera_image_, LV_OBJ_FLAG_HIDDEN);
        if (lv_label_get_text(subtitle_)[0] != '\0') {
            RenderTypingText();
            ShowResponseBox();
            lv_obj_remove_flag(subtitle_, LV_OBJ_FLAG_HIDDEN);
        } else if (lv_obj_has_flag(notification_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(response_box_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    camera_image_cached_ = std::move(image);
    const auto* descriptor = camera_image_cached_->image_dsc();
    lv_image_set_src(camera_image_, descriptor);
    if (descriptor->header.w > 0 && descriptor->header.h > 0) {
        // Cover the response panel and let its rounded bounds crop the excess.
        const int content_width = width_ - 38;
        const int content_height = 94;
        const int scale_x = 256 * content_width / descriptor->header.w;
        const int scale_y = 256 * content_height / descriptor->header.h;
        lv_image_set_scale(camera_image_, std::max(scale_x, scale_y));
        ESP_LOGI(kTag, "Showing camera preview: %ux%u", descriptor->header.w, descriptor->header.h);
    }
    lv_obj_add_flag(subtitle_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(notification_, LV_OBJ_FLAG_HIDDEN);
    response_scroll_target_ = 0;
    lv_obj_scroll_to_y(response_box_, 0, LV_ANIM_OFF);
    ShowResponseBox();
    lv_obj_set_style_opa(response_box_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(camera_image_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(camera_image_);
    lv_obj_invalidate(response_box_);
    esp_timer_stop(preview_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, kPreviewDurationMs * 1000));
}

void MochanDisplay::HidePreview() { SetPreviewImage(nullptr); }

void MochanDisplay::HideNotification() {
    if (notification_ != nullptr) {
        lv_obj_add_flag(notification_, LV_OBJ_FLAG_HIDDEN);
    }
    if (response_box_ != nullptr && subtitle_ != nullptr &&
        lv_label_get_text(subtitle_)[0] == '\0') {
        lv_obj_add_flag(subtitle_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(response_box_, LV_OBJ_FLAG_HIDDEN);
    } else if (subtitle_ != nullptr) {
        lv_obj_remove_flag(subtitle_, LV_OBJ_FLAG_HIDDEN);
    }
    notification_timer_ = nullptr;
}

void MochanDisplay::ShowBootSplash() {
    if (splash_ != nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    splash_ = lv_obj_create(lv_layer_top());
    lv_obj_set_size(splash_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(splash_, kFaceBackground, 0);
    lv_obj_set_style_bg_opa(splash_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(splash_, 0, 0);
    lv_obj_set_style_border_width(splash_, 0, 0);
    lv_obj_set_style_outline_width(splash_, 0, 0);
    lv_obj_set_style_shadow_width(splash_, 0, 0);
    lv_obj_set_style_pad_all(splash_, 0, 0);

    auto* spinner = lv_arc_create(splash_);
    lv_obj_set_size(spinner, 74, 74);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -43);
    lv_arc_set_range(spinner, 0, 100);
    lv_arc_set_value(spinner, 25);
    lv_arc_set_bg_angles(spinner, 0, 360);
    lv_arc_set_rotation(spinner, 270);
    lv_obj_remove_style(spinner, nullptr, LV_PART_KNOB);
    lv_obj_set_style_arc_width(spinner, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 7, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spinner, kSpinnerTrack, LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, kBrassHighlight, LV_PART_INDICATOR);

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, spinner);
    lv_anim_set_values(&animation, 0, 360);
    lv_anim_set_duration(&animation, 900);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&animation, [](void* object, int32_t angle) {
        lv_arc_set_rotation(static_cast<lv_obj_t*>(object), angle);
    });
    lv_anim_start(&animation);

    auto* title = lv_label_create(splash_);
    lv_label_set_text(title, "Xiaozhi AI Desk Robot");
    lv_obj_set_style_text_color(title, kBrass, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 45);

    auto* label = lv_label_create(splash_);
    lv_label_set_text(label, "Loading...");
    lv_obj_set_style_text_color(label, kBrassHighlight, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 71);
}

void MochanDisplay::HideBootSplash() {
    if (splash_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    lv_obj_delete(splash_);
    splash_ = nullptr;
}
