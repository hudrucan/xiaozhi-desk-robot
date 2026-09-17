#include "mochan_display.h"

#include "application.h"
#include "assets/lang_config.h"
#include "display/lvgl_display/lvgl_theme.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
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
const lv_color_t kSpinnerTrack = LV_COLOR_MAKE(0x4b, 0x3b, 0x25);
constexpr int kResponseTextScale = 210;
// Sentence messages carry no word timestamps: use a conservative playback-
// paced reveal, with faster catch-up only after the voice has drained.
constexpr int kTypingGlyphsPerSecond = 28;
constexpr int kTypingFinishingGlyphsPerSecond = 48;
constexpr int64_t kTypingUpdateIntervalUs = 66000;
constexpr int64_t kTypingCreditScale = 1000000;
constexpr int64_t kTypingMaxElapsedUs = 100000;
constexpr char kTag[] = "MochanDisplay";

std::string ResponseDisplayText(const char* content) {
    std::string text(content);
    constexpr char kBridgeTool[] = "self.web_chat.consume_pending";
    size_t position = 0;
    while ((position = text.find(kBridgeTool, position)) != std::string::npos) {
        size_t begin = position;
        size_t end = position + sizeof(kBridgeTool) - 1;
        // Only remove the display annotation and common no-argument call
        // wrappers. This never changes the MCP payload or Web transcript.
        while (begin > 0 && text[begin - 1] == '`') {
            --begin;
        }
        if (text.compare(end, 4, "({})") == 0) {
            end += 4;
        } else if (text.compare(end, 2, "()") == 0) {
            end += 2;
        }
        while (end < text.size() && text[end] == '`') {
            ++end;
        }
        if (begin > 0 && text[begin - 1] == '[' && end < text.size() && text[end] == ']') {
            --begin;
            ++end;
        }
        text.erase(begin, end - begin);
        // A tool-only line must not open the box or spend typewriter time on
        // punctuation/icons. Preserve any real text on the same line.
        const size_t previous_newline = begin == 0 ? std::string::npos : text.rfind('\n', begin - 1);
        const size_t line_begin = previous_newline == std::string::npos ? 0 : previous_newline + 1;
        const size_t newline = text.find('\n', begin);
        const size_t line_end = newline == std::string::npos ? text.size() : newline;
        std::string remainder = text.substr(line_begin, line_end - line_begin);
        for (const char* icon : {"🔧", "🛠️", "🛠"}) {
            size_t icon_position;
            while ((icon_position = remainder.find(icon)) != std::string::npos) {
                remainder.erase(icon_position, std::strlen(icon));
            }
        }
        if (remainder.find_first_not_of(" \t\r`*[](){}:;,.->") == std::string::npos) {
            text.erase(line_begin, line_end - line_begin + (newline != std::string::npos ? 1 : 0));
            position = line_begin;
        } else {
            position = begin;
        }
    }
    return text;
}

constexpr std::array<const char*, 34> kSupportedEmotions = {
    "neutral",    "happy",       "laughing",  "funny",     "sad",        "angry",   "crying",
    "loving",     "embarrassed", "surprised", "shocked",   "thinking",   "winking", "cool",
    "relaxed",    "delicious",   "kissy",     "confident", "sleepy",     "silly",   "confused",
    "suspicious", "shake",       "speaking",  "listening", "left",       "right",   "up",
    "down",       "up_left",     "up_right",  "down_left", "down_right", "bored",
};

bool EndsWithSentencePunctuation(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    const char last = text.back();
    return last == '.' || last == '!' || last == '?' || last == ':' || last == ';';
}

}  // namespace

void MochanDisplay::SetStatus(const char* status) {
    if (status == nullptr) {
        return;
    }
    FaceState next_activity = FaceState::kIdle;
    bool clear_emotion = false;
    const bool status_dot_busy = std::strcmp(status, Lang::Strings::PREPARING_ASR) == 0 ||
                                 std::strcmp(status, Lang::Strings::PROCESSING) == 0;
    if (std::strcmp(status, Lang::Strings::LISTENING) == 0) {
        next_activity = FaceState::kListening;
        clear_emotion = true;
    } else if (std::strcmp(status, Lang::Strings::SPEAKING) == 0) {
        next_activity = FaceState::kSpeaking;
    } else if (std::strcmp(status, Lang::Strings::CONNECTING) == 0 ||
               std::strcmp(status, Lang::Strings::PREPARING_ASR) == 0 ||
               std::strcmp(status, Lang::Strings::PROCESSING) == 0 ||
               std::strcmp(status, Lang::Strings::REGISTERING_NETWORK) == 0) {
        next_activity = FaceState::kThinking;
    } else {
        clear_emotion = true;
    }

    DisplayLockGuard lock(this);
    FreezeMouthForExit();
    activity_state_ = next_activity;
    status_dot_busy_ = status_dot_busy;
    if (clear_emotion) {
        emotion_active_ = false;
    }
    CancelIdleScheduler(true);
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
    std::string emotion;
    {
        std::lock_guard<std::mutex> state_lock(emotion_mutex_);
        emotion = current_emotion_;
    }
    UpdateFaceLayoutTarget(emotion, esp_timer_get_time());
}
void MochanDisplay::RenderTypingText() {
    if (subtitle_ == nullptr) {
        return;
    }

    // Keep the label/layout workload bounded during long answers. Retain the
    // full transcript for cumulative server updates, but only render its tail.
    if (typing_position_ - typing_window_start_ > 1024) {
        typing_window_start_ = typing_position_ - 768;
        while (typing_window_start_ < typing_position_ &&
               (static_cast<uint8_t>(typing_text_[typing_window_start_]) & 0xc0) == 0x80) {
            ++typing_window_start_;
        }
        const size_t word_end = typing_text_.find_first_of(" \n", typing_window_start_);
        if (word_end != std::string::npos && word_end < typing_position_) {
            typing_window_start_ = word_end + 1;
        }
    }
    std::string rendered = typing_window_start_ == 0 ? "> " : "… ";
    rendered.append(typing_text_, typing_window_start_, typing_position_ - typing_window_start_);
    if (typing_cursor_visible_) {
        rendered.push_back('|');
    }
    if (rendered == typing_rendered_text_) {
        return;
    }
    typing_rendered_text_ = std::move(rendered);
    lv_label_set_text(subtitle_, typing_rendered_text_.c_str());
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

    const bool was_typing = typing_active_;

    if (typing_text_.empty()) {
        typing_text_ = incoming;
        typing_position_ = 0;
    } else if (incoming == typing_text_) {
        // Ignore a duplicate transcript update.
        return;
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
    if (!was_typing) {
        typing_last_update_us_ = esp_timer_get_time();
        typing_output_clock_us_ = Application::GetInstance().GetAudioService().GetOutputClockUs();
        typing_glyph_credit_ = 0;
    }
    // Render on the face clock; sentence callbacks do not force extra layouts.
}

void MochanDisplay::UpdateTyping(int64_t now_us) {
    if (typing_text_.empty()) {
        ResetTyping();
        return;
    }

    if (now_us - typing_last_update_us_ < kTypingUpdateIntervalUs) {
        return;
    }
    // Let an expensive eye/blink frame finish without adding a text layout.
    // Bound the deferral so continuous expression changes cannot starve text.
    if (esp_timer_get_time() - now_us > 18000 &&
        now_us - typing_last_update_us_ < kTypingMaxElapsedUs) {
        return;
    }
    const int64_t elapsed_us =
        std::clamp(now_us - typing_last_update_us_, int64_t{0}, kTypingMaxElapsedUs);
    typing_last_update_us_ = now_us;

    auto& audio = Application::GetInstance().GetAudioService();
    const uint32_t output_clock_us = audio.GetOutputClockUs();
    const uint32_t played_us = output_clock_us - typing_output_clock_us_;
    typing_output_clock_us_ = output_clock_us;
    const bool finishing_after_audio = typing_finishing_ && audio.IsPlaybackIdle();
    const int64_t progress_us = finishing_after_audio ? elapsed_us : played_us;
    const int rate = finishing_after_audio ? kTypingFinishingGlyphsPerSecond : kTypingGlyphsPerSecond;
    constexpr int max_glyphs_per_frame = 6;
    typing_glyph_credit_ = std::min<int64_t>(
        typing_glyph_credit_ + progress_us * rate, max_glyphs_per_frame * kTypingCreditScale);
    int glyphs_to_reveal = static_cast<int>(typing_glyph_credit_ / kTypingCreditScale);
    glyphs_to_reveal = std::min(glyphs_to_reveal, max_glyphs_per_frame);
    typing_glyph_credit_ -= static_cast<int64_t>(glyphs_to_reveal) * kTypingCreditScale;

    bool visual_changed = false;
    if (typing_position_ < typing_text_.size()) {
        // Advance one complete UTF-8 code point so Vietnamese glyphs never
        // appear as temporarily corrupted byte sequences. A longer sentence
        // must not accelerate the reveal ahead of audio playback.
        for (int glyph = 0;
             glyph < glyphs_to_reveal && typing_position_ < typing_text_.size();
             ++glyph) {
            ++typing_position_;
            while (typing_position_ < typing_text_.size() &&
                   (static_cast<uint8_t>(typing_text_[typing_position_]) & 0xc0) == 0x80) {
                ++typing_position_;
            }
            visual_changed = true;
        }
    }
    if (typing_position_ >= typing_text_.size()) {
        typing_active_ = false;
        typing_finishing_ = false;
    }

    const bool previous_cursor_visible = typing_cursor_visible_;
    if (typing_active_) {
        typing_cursor_phase_ = static_cast<uint8_t>((now_us / 66000) % 16);
        typing_cursor_visible_ = typing_cursor_phase_ < 8;
    } else {
        typing_cursor_visible_ = false;
    }
    visual_changed = visual_changed || typing_cursor_visible_ != previous_cursor_visible;
    if (visual_changed) {
        RenderTypingText();
    }
}

void MochanDisplay::FinishTyping() {
    if (typing_text_.empty()) {
        return;
    }
    if (typing_position_ < typing_text_.size()) {
        typing_finishing_ = true;
        typing_active_ = true;
        typing_cursor_visible_ = true;
        typing_last_update_us_ = esp_timer_get_time();
        typing_output_clock_us_ = Application::GetInstance().GetAudioService().GetOutputClockUs();
        typing_glyph_credit_ = 0;
    } else {
        typing_finishing_ = false;
        typing_cursor_visible_ = false;
        typing_active_ = false;
    }
    RenderTypingText();
}

void MochanDisplay::ResetTyping() {
    typing_text_.clear();
    typing_rendered_text_.clear();
    typing_window_start_ = 0;
    typing_position_ = 0;
    typing_last_update_us_ = 0;
    typing_output_clock_us_ = 0;
    typing_glyph_credit_ = 0;
    typing_cursor_phase_ = 0;
    typing_cursor_visible_ = false;
    typing_active_ = false;
    typing_finishing_ = false;
    response_scroll_target_ = 0;
    if (response_box_ != nullptr) {
        lv_obj_scroll_to_y(response_box_, 0, LV_ANIM_OFF);
    }
}

void MochanDisplay::SetTheme(Theme* theme) {
    // Asset updates call this after Wi-Fi connects. Apply them directly to
    // the custom Mochan hierarchy.
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
    lv_obj_align(battery_icon_, LV_ALIGN_TOP_LEFT, 31, 6);
    lv_obj_align(battery_status_, LV_ALIGN_TOP_LEFT, 54, 7);
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
    FreezeMouthForExit();
    CancelIdleScheduler(true);
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
    DisplayLockGuard lock(this);
    CancelIdleScheduler(true);
    {
        std::lock_guard<std::mutex> state_lock(emotion_mutex_);
        current_emotion_ = requested;
    }
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
    } else if (requested == "cool" || requested == "bored") {
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
    std::string current_emotion;
    {
        std::lock_guard<std::mutex> state_lock(emotion_mutex_);
        current_emotion = current_emotion_;
    }
    UpdateFaceLayoutTarget(current_emotion, esp_timer_get_time());
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
    const bool is_assistant = role != nullptr && std::strcmp(role, "assistant") == 0;
    const std::string visible_text = is_assistant ? ResponseDisplayText(content) : content;
    if (content[0] != '\0' && visible_text.empty()) {
        return;
    }
    content = visible_text.c_str();
    DisplayLockGuard lock(this);
    if (content[0] != '\0') {
        FreezeMouthForExit();
    }
    CancelIdleScheduler(true);
    lv_obj_set_style_text_align(subtitle_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_transform_pivot_x(subtitle_, 0, 0);
    if (content[0] == '\0') {
        ResetTyping();
        lv_label_set_text(subtitle_, "");
        lv_obj_add_flag(subtitle_, LV_OBJ_FLAG_HIDDEN);
        if (lv_obj_has_flag(notification_, LV_OBJ_FLAG_HIDDEN)) {
            HideResponseBox();
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
    if (image != nullptr) {
        FreezeMouthForExit();
    }
    CancelIdleScheduler(true);
    if (image == nullptr) {
        esp_timer_stop(preview_timer_);
        preview_show_pending_ = false;
        lv_obj_add_flag(camera_image_, LV_OBJ_FLAG_HIDDEN);
        camera_image_cached_.reset();
        if (lv_label_get_text(subtitle_)[0] != '\0') {
            RenderTypingText();
            ShowResponseBox();
            lv_obj_remove_flag(subtitle_, LV_OBJ_FLAG_HIDDEN);
        } else if (lv_obj_has_flag(notification_, LV_OBJ_FLAG_HIDDEN)) {
            HideResponseBox();
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
        ESP_LOGD(kTag, "Showing camera preview: %ux%u", descriptor->header.w, descriptor->header.h);
    }
    lv_obj_add_flag(subtitle_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(notification_, LV_OBJ_FLAG_HIDDEN);
    response_scroll_target_ = 0;
    lv_obj_scroll_to_y(response_box_, 0, LV_ANIM_OFF);
    ShowResponseBox();
    if (response_box_progress_ < 256) {
        lv_obj_add_flag(camera_image_, LV_OBJ_FLAG_HIDDEN);
    }
    esp_timer_stop(preview_timer_);
    preview_show_pending_ = true;
}

void MochanDisplay::HidePreview() { SetPreviewImage(nullptr); }

void MochanDisplay::HideNotification() {
    if (notification_ != nullptr) {
        lv_obj_add_flag(notification_, LV_OBJ_FLAG_HIDDEN);
    }
    if (response_box_ != nullptr && subtitle_ != nullptr &&
        lv_label_get_text(subtitle_)[0] == '\0') {
        lv_obj_add_flag(subtitle_, LV_OBJ_FLAG_HIDDEN);
        HideResponseBox();
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
    CancelIdleScheduler(true);
    SetFaceLayoutTarget(0, esp_timer_get_time());
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
    std::string emotion;
    {
        std::lock_guard<std::mutex> state_lock(emotion_mutex_);
        emotion = current_emotion_;
    }
    UpdateFaceLayoutTarget(emotion, esp_timer_get_time());
}
