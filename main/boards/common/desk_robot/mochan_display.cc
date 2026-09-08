#include "mochan_display.h"

#include "assets/lang_config.h"
#include "display/lvgl_display/lvgl_theme.h"

#include <esp_err.h>
#include <material_symbols.h>
#include <cctype>
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

    response_box_ = lv_obj_create(container_);
    lv_obj_set_size(response_box_, width_ - 20, 112);
    lv_obj_align(response_box_, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(response_box_, lv_color_hex(0x11100d), 0);
    lv_obj_set_style_bg_opa(response_box_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(response_box_, kSpinnerTrack, 0);
    lv_obj_set_style_border_width(response_box_, 1, 0);
    lv_obj_set_style_radius(response_box_, 8, 0);
    lv_obj_set_style_pad_all(response_box_, 9, 0);
    lv_obj_set_scrollbar_mode(response_box_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(response_box_, LV_OBJ_FLAG_HIDDEN);

    subtitle_ = lv_label_create(response_box_);
    lv_obj_set_width(subtitle_, width_ - 38);
    lv_obj_set_height(subtitle_, 94);
    lv_obj_set_style_text_color(subtitle_, kBrassHighlight, 0);
    lv_obj_set_style_text_align(subtitle_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_transform_scale(subtitle_, 210, 0);
    lv_obj_set_style_transform_pivot_x(subtitle_, 0, 0);
    lv_obj_set_style_transform_pivot_y(subtitle_, 0, 0);
    lv_label_set_long_mode(subtitle_, LV_LABEL_LONG_WRAP);
    lv_obj_align(subtitle_, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(subtitle_, LV_OBJ_FLAG_HIDDEN);

    notification_ = lv_label_create(response_box_);
    lv_obj_set_width(notification_, width_ - 38);
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
            display->blink_phase_ = static_cast<uint8_t>((display->blink_phase_ + 1) % 60);
            const bool blink = display->blink_phase_ < 7;
            display->UpdateEyes(blink);
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
    face_state_ = state;
    UpdateStatusDot();
    UpdateEyes(false);
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

void MochanDisplay::UpdateEyes(bool blink) {
    if (left_eye_ == nullptr || right_eye_ == nullptr) {
        return;
    }

    blink_closed_ = blink;
    int eye_width = 74;
    int eye_height = 54;
    int gap = 22;
    int left_rotation = 0;
    int right_rotation = 0;
    if (face_state_ == FaceState::kListening) {
        eye_width = 78;
        eye_height = 62;
        gap = 18;
    } else if (face_state_ == FaceState::kSpeaking) {
        eye_width = 66;
        eye_height = 48;
        gap = 28;
    } else if (face_state_ == FaceState::kThinking) {
        eye_width = 70;
        eye_height = 58;
        gap = 30;
    } else if (face_state_ == FaceState::kHappy) {
        eye_width = 68;
        eye_height = 28;
        gap = 20;
    } else if (face_state_ == FaceState::kAngry) {
        eye_width = 72;
        eye_height = 48;
        gap = 18;
        left_rotation = 120;
        right_rotation = -120;
    } else if (face_state_ == FaceState::kSad) {
        eye_width = 62;
        eye_height = 42;
        gap = 30;
        left_rotation = -80;
        right_rotation = 80;
    } else if (face_state_ == FaceState::kSuspicious) {
        eye_width = 64;
        eye_height = 50;
        gap = 26;
        left_rotation = -55;
        right_rotation = -15;
    }
    if (blink_closed_) {
        eye_height = 7;
    } else if (face_state_ == FaceState::kSpeaking) {
        // A restrained 600 ms "breathing" beat makes speech feel alive
        // without tying animation timing to the audio task.
        eye_height += (blink_phase_ / 6) % 2 == 0 ? -4 : 4;
    }

    int gaze_x = 0;
    int gaze_y = -59;
    if (face_state_ == FaceState::kIdle) {
        if (blink_phase_ >= 12 && blink_phase_ <= 26) {
            gaze_x = -9;
            gaze_y = -56;
        } else if (blink_phase_ >= 35 && blink_phase_ <= 50) {
            gaze_x = 9;
            gaze_y = -52;
        }
    } else if (face_state_ == FaceState::kHappy) {
        gaze_y = blink_phase_ < 30 ? -62 : -57;
    } else if (face_state_ == FaceState::kAngry) {
        gaze_y = -53;
    } else if (face_state_ == FaceState::kSad) {
        gaze_y = -49;
    } else if (face_state_ == FaceState::kSuspicious) {
        gaze_x = blink_phase_ < 30 ? -5 : 5;
        gaze_y = -54;
    } else if (face_state_ == FaceState::kThinking) {
        gaze_x = blink_phase_ < 30 ? -12 : 12;
        gaze_y = -50;
    } else if (face_state_ == FaceState::kShake) {
        gaze_x = (blink_phase_ / 3) % 2 == 0 ? -14 : 14;
    } else if (face_state_ == FaceState::kLookUpLeft) {
        gaze_x = -14;
        gaze_y = -56;
    } else if (face_state_ == FaceState::kLookUpRight) {
        gaze_x = 14;
        gaze_y = -56;
    } else if (face_state_ == FaceState::kLookDownLeft) {
        gaze_x = -14;
        gaze_y = -28;
    } else if (face_state_ == FaceState::kLookDownRight) {
        gaze_x = 14;
        gaze_y = -28;
    }

    auto smooth = [](int current, int target) {
        const int delta = target - current;
        if (delta >= -1 && delta <= 1) {
            return target;
        }
        const int step = delta / 3;
        return current + (step != 0 ? step : (delta > 0 ? 1 : -1));
    };
    if (!eye_geometry_initialized_) {
        eye_width_ = eye_width;
        eye_height_ = eye_height;
        eye_gap_ = gap;
        eye_gaze_x_ = gaze_x;
        eye_gaze_y_ = gaze_y;
        left_eye_rotation_ = left_rotation;
        right_eye_rotation_ = right_rotation;
        eye_geometry_initialized_ = true;
    } else {
        eye_width_ = smooth(eye_width_, eye_width);
        eye_height_ = smooth(eye_height_, eye_height);
        eye_gap_ = smooth(eye_gap_, gap);
        eye_gaze_x_ = smooth(eye_gaze_x_, gaze_x);
        eye_gaze_y_ = smooth(eye_gaze_y_, gaze_y);
        left_eye_rotation_ = smooth(left_eye_rotation_, left_rotation);
        right_eye_rotation_ = smooth(right_eye_rotation_, right_rotation);
    }

    lv_obj_set_size(left_eye_, eye_width_, eye_height_);
    lv_obj_set_size(right_eye_, eye_width_, eye_height_);
    lv_obj_set_style_transform_pivot_x(left_eye_, eye_width_ / 2, 0);
    lv_obj_set_style_transform_pivot_y(left_eye_, eye_height_ / 2, 0);
    lv_obj_set_style_transform_pivot_x(right_eye_, eye_width_ / 2, 0);
    lv_obj_set_style_transform_pivot_y(right_eye_, eye_height_ / 2, 0);
    lv_obj_set_style_transform_rotation(left_eye_, left_eye_rotation_, 0);
    lv_obj_set_style_transform_rotation(right_eye_, right_eye_rotation_, 0);
    lv_obj_align(left_eye_, LV_ALIGN_CENTER, eye_gaze_x_ - (eye_width_ / 2 + eye_gap_ / 2),
                 eye_gaze_y_);
    lv_obj_align(right_eye_, LV_ALIGN_CENTER, eye_gaze_x_ + eye_width_ / 2 + eye_gap_ / 2,
                 eye_gaze_y_);

    const int eyelid_width = eye_width_;
    const int eyelid_height = eye_height_;
    const bool show_eyelids = !blink_closed_;
    for (auto* eyelid : {left_eyelid_, right_eyelid_}) {
        lv_obj_set_style_bg_opa(eyelid, show_eyelids ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_size(eyelid, eyelid_width, eyelid_height);
    }
    lv_obj_align(left_eyelid_, LV_ALIGN_CENTER, eye_gaze_x_ - (eye_width_ / 2 + eye_gap_ / 2) + 4,
                 eye_gaze_y_ + 6);
    lv_obj_align(right_eyelid_, LV_ALIGN_CENTER, eye_gaze_x_ + eye_width_ / 2 + eye_gap_ / 2 + 4,
                 eye_gaze_y_ + 6);
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
    DisplayLockGuard lock(this);
    if (std::strstr(emotion, "happy") != nullptr) {
        emotion_active_ = true;
        SetFaceState(FaceState::kHappy);
    } else if (std::strstr(emotion, "angry") != nullptr) {
        emotion_active_ = true;
        SetFaceState(FaceState::kAngry);
    } else if (std::strstr(emotion, "sad") != nullptr ||
               std::strstr(emotion, "sleepy") != nullptr) {
        emotion_active_ = true;
        SetFaceState(FaceState::kSad);
    } else if (std::strstr(emotion, "suspicious") != nullptr) {
        emotion_active_ = true;
        SetFaceState(FaceState::kSuspicious);
    } else if (std::strstr(emotion, "shake") != nullptr) {
        emotion_active_ = true;
        SetFaceState(FaceState::kShake);
    } else if (std::strstr(emotion, "up_left") != nullptr) {
        emotion_active_ = true;
        SetFaceState(FaceState::kLookUpLeft);
    } else if (std::strstr(emotion, "up_right") != nullptr) {
        emotion_active_ = true;
        SetFaceState(FaceState::kLookUpRight);
    } else if (std::strstr(emotion, "down_left") != nullptr) {
        emotion_active_ = true;
        SetFaceState(FaceState::kLookDownLeft);
    } else if (std::strstr(emotion, "down_right") != nullptr) {
        emotion_active_ = true;
        SetFaceState(FaceState::kLookDownRight);
    } else if (std::strstr(emotion, "thinking") != nullptr) {
        emotion_active_ = true;
        SetFaceState(FaceState::kThinking);
    } else if (std::strstr(emotion, "speaking") != nullptr) {
        emotion_active_ = true;
        SetFaceState(FaceState::kSpeaking);
    } else if (std::strstr(emotion, "neutral") != nullptr) {
        emotion_active_ = false;
        SetFaceState(activity_state_);
    }
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
        // Fill the wide response panel and let its bounds crop the top and bottom.
        // This preserves the original UI's cinematic preview rather than shrinking
        // a 4:3 frame into a tiny letterboxed thumbnail.
        const auto scale_x = 256 * (width_ - 38) / descriptor->header.w;
        lv_image_set_scale(camera_image_, scale_x);
    }
    lv_obj_add_flag(subtitle_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(notification_, LV_OBJ_FLAG_HIDDEN);
    ShowResponseBox();
    lv_obj_remove_flag(camera_image_, LV_OBJ_FLAG_HIDDEN);
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
