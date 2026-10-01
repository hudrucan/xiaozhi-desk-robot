#include "mochan_display.h"

#include "application.h"
#include "desk_mode_view.h"
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
#include <cstdlib>
#include <cstring>

namespace {

const lv_color_t kFaceBackground = LV_COLOR_MAKE(0x00, 0x00, 0x00);
const lv_color_t kBrass = LV_COLOR_MAKE(0xc6, 0xa1, 0x5b);
const lv_color_t kBrassHighlight = LV_COLOR_MAKE(0xe3, 0xc2, 0x7b);
const lv_color_t kSpinnerTrack = LV_COLOR_MAKE(0x4b, 0x3b, 0x25);
// Sentence messages carry no word timestamps. Pace text with played audio,
// allowing delayed frames to catch up and finishing faster once audio drains.
constexpr int kTypingGlyphsPerSecond = 28;
constexpr int kTypingFinishingGlyphsPerSecond = 96;
// Sample text progress on every 30 FPS face frame. Audio commonly advances in
// larger PCM chunks, so spread accumulated glyph credit across frames instead
// of revealing several characters in a single 66 ms step.
constexpr int64_t kTypingUpdateIntervalUs = 33000;
constexpr int64_t kTypingCreditScale = 1000000;
constexpr int64_t kTypingMaxElapsedUs = 100000;
constexpr int kLiveMaxPendingGlyphs = 6;
constexpr int64_t kLiveRevealMaxLagUs = 120000;
constexpr int64_t kResponseScrollSmoothingUs = 120000;
constexpr char kTag[] = "MochanDisplay";

bool IsResponseCombiningMark(const std::string& text, size_t position) {
    if (position + 1 >= text.size()) {
        return false;
    }
    const auto first = static_cast<uint8_t>(text[position]);
    const auto second = static_cast<uint8_t>(text[position + 1]);
    return second >= 0x80 &&
           ((first == 0xcc && second <= 0xbf) || (first == 0xcd && second <= 0xaf));
}

size_t ResponseTailWindowStart(const std::string& text, size_t position, size_t window_start) {
    window_start = std::min(window_start, position);
    if (position - window_start > 1024) {
        window_start = position - 768;
        while (window_start < position &&
               (static_cast<uint8_t>(text[window_start]) & 0xc0) == 0x80) {
            ++window_start;
        }
        while (window_start < position && IsResponseCombiningMark(text, window_start)) {
            window_start += 2;
        }
        const size_t word_end = text.find_first_of(" \n", window_start);
        if (word_end != std::string::npos && word_end < position) {
            window_start = word_end + 1;
        }
    }
    return window_start;
}

size_t NextResponseGlyph(const std::string& text, size_t position) {
    if (position >= text.size()) {
        return text.size();
    }
    do {
        ++position;
        while (position < text.size() &&
               (static_cast<uint8_t>(text[position]) & 0xc0) == 0x80) {
            ++position;
        }
        // Keep Vietnamese decomposed accents with their base character.
    } while (IsResponseCombiningMark(text, position));
    return position;
}

uint32_t CountResponseCodepoints(const char* text, size_t length) {
    uint32_t count = 0;
    for (size_t i = 0; i < length; ++i) {
        if ((static_cast<uint8_t>(text[i]) & 0xc0) != 0x80) {
            ++count;
        }
    }
    return count;
}

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

}  // namespace

bool MochanDisplay::ResolveEmotionFaceState(const std::string& emotion, FaceState& state) {
    if (emotion == "neutral" || emotion == "robot_2") {
        state = FaceState::kIdle;
    } else if (emotion == "happy") {
        state = FaceState::kHappy;
    } else if (emotion == "laughing") {
        state = FaceState::kLaughing;
    } else if (emotion == "funny") {
        state = FaceState::kFunny;
    } else if (emotion == "angry") {
        state = FaceState::kAngry;
    } else if (emotion == "sad") {
        state = FaceState::kSad;
    } else if (emotion == "crying") {
        state = FaceState::kCrying;
    } else if (emotion == "loving") {
        state = FaceState::kLoving;
    } else if (emotion == "embarrassed") {
        state = FaceState::kEmbarrassed;
    } else if (emotion == "surprised") {
        state = FaceState::kSurprised;
    } else if (emotion == "shocked") {
        state = FaceState::kShocked;
    } else if (emotion == "winking") {
        state = FaceState::kWinking;
    } else if (emotion == "cool" || emotion == "bored") {
        state = FaceState::kCool;
    } else if (emotion == "relaxed") {
        state = FaceState::kRelaxed;
    } else if (emotion == "delicious") {
        state = FaceState::kDelicious;
    } else if (emotion == "kissy") {
        state = FaceState::kKissy;
    } else if (emotion == "confident") {
        state = FaceState::kConfident;
    } else if (emotion == "sleepy") {
        state = FaceState::kSleepy;
    } else if (emotion == "silly") {
        state = FaceState::kSilly;
    } else if (emotion == "confused") {
        state = FaceState::kConfused;
    } else if (emotion == "suspicious") {
        state = FaceState::kSuspicious;
    } else if (emotion == "shake") {
        state = FaceState::kShake;
    } else if (emotion == "left") {
        state = FaceState::kLookLeft;
    } else if (emotion == "right") {
        state = FaceState::kLookRight;
    } else if (emotion == "up") {
        state = FaceState::kLookUp;
    } else if (emotion == "down") {
        state = FaceState::kLookDown;
    } else if (emotion == "up_left") {
        state = FaceState::kLookUpLeft;
    } else if (emotion == "up_right") {
        state = FaceState::kLookUpRight;
    } else if (emotion == "down_left") {
        state = FaceState::kLookDownLeft;
    } else if (emotion == "down_right") {
        state = FaceState::kLookDownRight;
    } else if (emotion == "thinking") {
        state = FaceState::kThinking;
    } else if (emotion == "speaking") {
        state = FaceState::kSpeaking;
    } else if (emotion == "listening") {
        state = FaceState::kListening;
    } else {
        return false;
    }
    return true;
}

void MochanDisplay::ApplyRestingFaceLocked() {
    if (face_override_active_.load(std::memory_order_acquire) || emotion_active_) {
        return;
    }

    std::string emotion = "neutral";
    FaceState state = FaceState::kIdle;
    if (activity_state_ == FaceState::kListening) {
        emotion = "listening";
        state = FaceState::kListening;
    } else if (activity_state_ == FaceState::kThinking) {
        emotion = "thinking";
        state = FaceState::kThinking;
    } else if (activity_state_ == FaceState::kSpeaking) {
        emotion = "speaking";
        state = FaceState::kSpeaking;
    } else if (!ambient_base_face_.empty() &&
               !ResolveEmotionFaceState(ambient_base_face_, state)) {
        ambient_base_face_.clear();
    } else if (!ambient_base_face_.empty()) {
        emotion = ambient_base_face_;
    }

    {
        std::lock_guard<std::mutex> state_lock(emotion_mutex_);
        current_emotion_ = emotion;
    }
    SetFaceState(state);
    UpdateFaceLayoutTarget(emotion, esp_timer_get_time());
}

void MochanDisplay::SetStatus(const char* status) {
    if (status == nullptr) {
        return;
    }
    FaceState next_activity = FaceState::kIdle;
    bool clear_emotion = false;
    const bool status_dot_busy = std::strcmp(status, Lang::Strings::INITIALIZING) == 0 ||
                                 std::strcmp(status, Lang::Strings::PREPARING_ASR) == 0 ||
                                 std::strcmp(status, Lang::Strings::PROCESSING) == 0;
    if (std::strcmp(status, Lang::Strings::LISTENING) == 0) {
        next_activity = FaceState::kListening;
        clear_emotion = true;
    } else if (std::strcmp(status, Lang::Strings::SPEAKING) == 0) {
        next_activity = FaceState::kSpeaking;
    } else if (std::strcmp(status, Lang::Strings::INITIALIZING) == 0 ||
               std::strcmp(status, Lang::Strings::CONNECTING) == 0 ||
               std::strcmp(status, Lang::Strings::PREPARING_ASR) == 0 ||
               std::strcmp(status, Lang::Strings::PROCESSING) == 0 ||
               std::strcmp(status, Lang::Strings::REGISTERING_NETWORK) == 0) {
        next_activity = FaceState::kThinking;
    } else {
        clear_emotion = true;
    }

    AmbientActivity ambient_activity = AmbientActivity::kIdle;
    if (next_activity == FaceState::kListening) {
        ambient_activity = AmbientActivity::kListening;
    } else if (next_activity == FaceState::kThinking) {
        ambient_activity = AmbientActivity::kThinking;
    } else if (next_activity == FaceState::kSpeaking) {
        ambient_activity = AmbientActivity::kSpeaking;
    }
    ambient_activity_.store(ambient_activity, std::memory_order_release);

    DisplayLockGuard lock(this);
    if (next_activity != FaceState::kIdle || status_dot_busy) {
        CancelCameraAttentionLocked();
        HideDeskModeLocked();
    }
    activity_state_ = next_activity;
    status_dot_busy_ = status_dot_busy;
    if (camera_attention_active_.load(std::memory_order_acquire)) {
        UpdateStatusDot();
        return;
    }
    if (activity_state_ != FaceState::kSpeaking) {
        FinishTyping();
    }
    UpdateStatusDot();
    if (face_override_active_.load(std::memory_order_acquire)) {
        return;
    }
    FreezeMouthForExit();
    if (clear_emotion) {
        emotion_active_ = false;
    }
    CancelAmbientAnimations();
    if (!emotion_active_) {
        ApplyRestingFaceLocked();
        return;
    }
    std::string emotion;
    {
        std::lock_guard<std::mutex> state_lock(emotion_mutex_);
        emotion = current_emotion_;
    }
    UpdateFaceLayoutTarget(emotion, esp_timer_get_time());
}

void MochanDisplay::SetFaceOverrideActive(bool active) {
    face_override_active_.store(active, std::memory_order_release);
    if (active) {
        DisplayLockGuard lock(this);
        CancelCameraAttentionLocked();
        HideDeskModeLocked();
    }
}

void MochanDisplay::RestoreActivityFace() {
    DisplayLockGuard lock(this);
    CancelCameraAttentionLocked();
    HideDeskModeLocked();
    FreezeMouthForExit();
    emotion_active_ = false;
    CancelAmbientAnimations();
    ApplyRestingFaceLocked();
}

void MochanDisplay::RenderTypingText() {
    if (subtitle_ == nullptr || typing_text_.empty() || live_user_transcript_active_) {
        return;
    }

    // Keep the label/layout workload bounded during long answers. Retain the
    // full transcript for cumulative server updates, but only render its tail.
    const size_t previous_start = typing_window_start_;
    typing_window_start_ = ResponseTailWindowStart(typing_text_, typing_position_, previous_start);
    RenderResponseText(typing_text_, typing_position_, typing_window_start_,
                       typing_window_start_ == 0 ? "> " : "… ",
                       typing_window_start_ - previous_start);
    UpdateResponseCursor(typing_cursor_visible_);
}

void MochanDisplay::RenderResponseText(const std::string& text, size_t position,
                                      size_t window_start, const char* prefix,
                                      size_t dropped_bytes) {
    const size_t prefix_length = std::strlen(prefix);
    const size_t text_length = position - window_start;
    const size_t length = prefix_length + text_length;
    if (length >= response_text_buffer_.size()) {
        return;  // Callers retain a UTF-8-safe tail of at most 1024 bytes.
    }
    if (length == response_text_length_ &&
        std::memcmp(response_text_buffer_.data(), prefix, prefix_length) == 0 &&
        std::memcmp(response_text_buffer_.data() + prefix_length,
                    text.data() + window_start, text_length) == 0) {
        return;
    }

    // When dropping an off-screen prefix, retain the first surviving line's
    // screen position instead of animating backward over removed content.
    int32_t anchor_y = 0;
    const int32_t old_scroll = lv_obj_get_scroll_y(response_box_);
    if (dropped_bytes != 0 && response_text_length_ != 0) {
        const size_t anchor_byte =
            std::min(response_text_length_, response_prefix_length_ + dropped_bytes);
        lv_point_t anchor{};
        lv_label_get_letter_pos(subtitle_,
            CountResponseCodepoints(response_text_buffer_.data(), anchor_byte), &anchor);
        anchor_y = anchor.y;
    }

    std::memcpy(response_text_buffer_.data(), prefix, prefix_length);
    std::memcpy(response_text_buffer_.data() + prefix_length,
                text.data() + window_start, text_length);
    response_text_buffer_[length] = '\0';
    response_text_length_ = length;
    response_prefix_length_ = prefix_length;
    response_text_codepoints_ = CountResponseCodepoints(response_text_buffer_.data(), length);
    lv_label_set_text_static(subtitle_, response_text_buffer_.data());
    UpdateResponseTextScroll();
    if (anchor_y != 0) {
        lv_obj_scroll_to_y(response_box_, std::max<int32_t>(0, old_scroll - anchor_y), LV_ANIM_OFF);
    }
    UpdateResponseCursor(false, true);
}

void MochanDisplay::UpdateResponseCursor(bool visible, bool update_position) {
    if (response_cursor_ == nullptr || subtitle_ == nullptr) {
        return;
    }
    if (update_position) {
        lv_point_t position{};
        lv_label_get_letter_pos(subtitle_, response_text_codepoints_, &position);
        const int32_t max_x = std::max<int32_t>(0, lv_obj_get_content_width(subtitle_) - 2);
        lv_obj_set_pos(response_cursor_, std::min(position.x, max_x), position.y + 2);
        lv_obj_set_height(response_cursor_,
                          std::max<int32_t>(1, response_font_.line_height - response_font_.base_line - 2));
    }
    if (visible) {
        lv_obj_remove_flag(response_cursor_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(response_cursor_, LV_OBJ_FLAG_HIDDEN);
    }
}

void MochanDisplay::UpdateResponseTextScroll() {
    if (subtitle_ == nullptr || response_box_ == nullptr) {
        return;
    }
    lv_obj_update_layout(response_box_);

    // Native font geometry agrees with LVGL layout; cursor is a floating child
    // of the label and never changes wrapping or the scrollable content size.
    const int32_t label_height = lv_obj_get_height(subtitle_);
    const int32_t viewport_height = lv_obj_get_content_height(response_box_);
    response_scroll_target_ = std::max<int32_t>(0, label_height - viewport_height);
}

void MochanDisplay::AdvanceResponseTextScroll(int64_t now_us) {
    const int64_t elapsed_us = response_scroll_last_update_us_ == 0 ? kTypingUpdateIntervalUs :
        std::clamp(now_us - response_scroll_last_update_us_, int64_t{0}, kTypingMaxElapsedUs);
    response_scroll_last_update_us_ = now_us;
    if (response_box_ == nullptr || subtitle_ == nullptr ||
        lv_obj_has_flag(subtitle_, LV_OBJ_FLAG_HIDDEN) || !response_box_requested_) {
        return;
    }
    const int32_t current = lv_obj_get_scroll_y(response_box_);
    const int32_t delta = response_scroll_target_ - current;
    if (delta == 0 || elapsed_us == 0) {
        return;
    }
    const int32_t distance = std::abs(delta);
    const int32_t step = std::min<int32_t>(distance,
        std::max<int32_t>(1, (distance * elapsed_us + kResponseScrollSmoothingUs - 1) /
                              kResponseScrollSmoothingUs));
    // Retarget the same motion each frame, in either direction, without
    // restarting LVGL animations whenever a new glyph changes the line count.
    lv_obj_scroll_to_y(response_box_, current + (delta > 0 ? step : -step), LV_ANIM_OFF);
}

void MochanDisplay::ResetResponseTextScroll() {
    response_scroll_target_ = 0;
    response_scroll_last_update_us_ = 0;
    if (response_box_ != nullptr) {
        lv_obj_scroll_to_y(response_box_, 0, LV_ANIM_OFF);
    }
}

void MochanDisplay::StartTyping(const char* content) {
    const std::string incoming(content);
    if (incoming.empty()) {
        return;
    }

    const bool was_typing = typing_active_;

    if (typing_text_.empty()) {
        // A new assistant response starts a new viewport. Do not interpolate
        // backward through the preceding user's transcript or preview scroll.
        ResetResponseTextScroll();
        RenderResponseText("", 0, 0, "");
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
            // Preserve server punctuation, including comma/soft boundaries.
            // The Web transcript also joins segments with a space only.
            typing_text_ += " ";
        }
        typing_text_ += incoming;
    }

    typing_pending_glyphs_ = 0;
    for (size_t position = typing_position_; position < typing_text_.size();
         position = NextResponseGlyph(typing_text_, position)) {
        ++typing_pending_glyphs_;
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
    // At 30 FPS allow up to two glyphs only when played audio earned them.
    // Increase the frame budget when delayed; never discard earned audio progress
    // because the face renderer was busy. Credit cannot exceed pending text.
    const int max_glyphs_per_frame = static_cast<int>(std::clamp<int64_t>(
        (elapsed_us * rate + kTypingCreditScale - 1) / kTypingCreditScale,
        2, finishing_after_audio ? 6 : 4));
    typing_glyph_credit_ = std::min<int64_t>(
        typing_glyph_credit_ + progress_us * rate,
        static_cast<int64_t>(typing_pending_glyphs_) * kTypingCreditScale);
    const int glyphs_to_reveal = static_cast<int>(std::min<int64_t>(
        typing_glyph_credit_ / kTypingCreditScale, max_glyphs_per_frame));
    typing_glyph_credit_ -= static_cast<int64_t>(glyphs_to_reveal) * kTypingCreditScale;

    bool visual_changed = false;
    if (typing_position_ < typing_text_.size()) {
        // Reveal complete UTF-8 glyphs, including their combining accents.
        for (int glyph = 0;
             glyph < glyphs_to_reveal && typing_position_ < typing_text_.size();
             ++glyph) {
            typing_position_ = NextResponseGlyph(typing_text_, typing_position_);
            --typing_pending_glyphs_;
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
    } else {
        typing_finishing_ = false;
        typing_cursor_visible_ = false;
        typing_active_ = false;
    }
    RenderTypingText();
}

void MochanDisplay::ResetTyping(bool reset_scroll) {
    typing_text_.clear();
    typing_window_start_ = 0;
    typing_position_ = 0;
    typing_pending_glyphs_ = 0;
    typing_last_update_us_ = 0;
    typing_output_clock_us_ = 0;
    typing_glyph_credit_ = 0;
    typing_cursor_phase_ = 0;
    typing_cursor_visible_ = false;
    typing_active_ = false;
    typing_finishing_ = false;
    UpdateResponseCursor(false);
    if (reset_scroll) {
        ResetResponseTextScroll();
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
        // Keep response text at native 16px. Preserve asset/dynamic fallback
        // for characters outside its Latin/Vietnamese and basic 16px coverage.
        response_fallback_font_.fallback = text_font;
        lv_obj_set_style_text_font(subtitle_, &response_font_, 0);
        UpdateResponseTextScroll();
        UpdateResponseCursor(live_user_transcript_active_ || typing_cursor_visible_, true);
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
    if (mic_mute_icon_ != nullptr && lvgl_theme->icon_font() != nullptr) {
        lv_obj_set_style_text_font(mic_mute_icon_, lvgl_theme->icon_font()->font(), 0);
    }
    if (battery_status_ != nullptr) {
        lv_obj_set_style_text_font(battery_status_, text_font, 0);
    }
}

void MochanDisplay::SetMicrophoneMuted(bool muted) {
    if (mic_mute_icon_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    if (muted) {
        lv_obj_remove_flag(mic_mute_icon_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(mic_mute_icon_, LV_OBJ_FLAG_HIDDEN);
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
    CancelCameraAttentionLocked();
    HideDeskModeLocked();
    FreezeMouthForExit();
    CancelAmbientAnimations();
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
    CancelCameraAttentionLocked();
    HideDeskModeLocked();
    CancelAmbientAnimations();
    FaceState state = FaceState::kIdle;
    if (!ResolveEmotionFaceState(requested, state)) {
        ESP_LOGW(kTag, "Unsupported emotion: %s", emotion);
        emotion_active_ = false;
        ApplyRestingFaceLocked();
        return;
    }
    if (requested == "neutral" || requested == "robot_2") {
        emotion_active_ = false;
        ApplyRestingFaceLocked();
        return;
    }

    emotion_active_ = true;
    {
        std::lock_guard<std::mutex> state_lock(emotion_mutex_);
        current_emotion_ = requested;
    }
    SetFaceState(state);
    UpdateFaceLayoutTarget(requested, esp_timer_get_time());
}

std::string MochanDisplay::GetCurrentEmotion() const {
    std::lock_guard<std::mutex> lock(emotion_mutex_);
    return current_emotion_;
}

bool MochanDisplay::IsSupportedEmotion(const std::string& emotion) {
    return std::find(kSupportedEmotions.begin(), kSupportedEmotions.end(), emotion) !=
           kSupportedEmotions.end();
}

void MochanDisplay::SetLiveUserTranscript(const char* content) {
    if (subtitle_ == nullptr || response_box_ == nullptr || content == nullptr ||
        content[0] == '\0') {
        return;
    }
    std::string incoming(content);

    DisplayLockGuard lock(this);
    if (!lock) {
        return;
    }
    if (live_user_transcript_active_ && incoming == live_user_text_) {
        return;
    }
    const bool first_partial = !live_user_transcript_active_;
    const bool pending_before_update = live_user_position_ < live_user_text_.size();
    const bool correction = !first_partial &&
        (incoming.size() < live_user_text_.size() ||
         incoming.compare(0, live_user_text_.size(), live_user_text_) != 0);
    if (first_partial) {
        ResetTyping();
        ResetLiveUserTranscript();
        RenderResponseText("", 0, 0, "");
    }
    // Live text takes ownership of the existing viewport, including a pending
    // preview overlay. Camera capture/attention policy remains unchanged.
    if (camera_image_ != nullptr &&
        (preview_show_pending_ || !lv_obj_has_flag(camera_image_, LV_OBJ_FLAG_HIDDEN))) {
        esp_timer_stop(preview_timer_);
        preview_show_pending_ = false;
        lv_obj_add_flag(camera_image_, LV_OBJ_FLAG_HIDDEN);
        camera_image_cached_.reset();
    }
    live_user_transcript_active_ = true;
    live_user_text_ = std::move(incoming);
    const int64_t now_us = esp_timer_get_time();
    if (correction) {
        // Hypotheses can revise earlier words. Replace those immediately;
        // never append a correction to an obsolete version or animate it twice.
        live_user_position_ = live_user_text_.size();
        live_user_window_start_ = 0;
        live_user_reveal_deadline_us_ = 0;
    } else {
        size_t pending = 0;
        for (size_t pos = live_user_position_; pos < live_user_text_.size();
             pos = NextResponseGlyph(live_user_text_, pos)) {
            ++pending;
        }
        // Animate only a short suffix: a model chunk may deliver a whole word
        // at once, but the UI must never build a growing transcript backlog.
        while (pending > kLiveMaxPendingGlyphs) {
            live_user_position_ = NextResponseGlyph(live_user_text_, live_user_position_);
            --pending;
        }
        const int64_t deadline = now_us + kLiveRevealMaxLagUs;
        live_user_reveal_deadline_us_ = pending_before_update && !first_partial
            ? std::min(live_user_reveal_deadline_us_, deadline) : deadline;
    }
    CancelCameraAttentionLocked();
    HideDeskModeLocked();
    FreezeMouthForExit();
    CancelAmbientAnimations();
    lv_obj_set_style_text_align(subtitle_, LV_TEXT_ALIGN_LEFT, 0);
    if (notification_ != nullptr) {
        lv_obj_add_flag(notification_, LV_OBJ_FLAG_HIDDEN);
    }
    ShowResponseBox();
    lv_obj_remove_flag(subtitle_, LV_OBJ_FLAG_HIDDEN);
    if (correction) {
        live_user_window_start_ = ResponseTailWindowStart(live_user_text_, live_user_position_, 0);
        RenderResponseText(live_user_text_, live_user_position_, live_user_window_start_,
                           live_user_window_start_ == 0 ? "" : "… ");
        UpdateResponseCursor(true);
    }
}

void MochanDisplay::UpdateLiveUserTranscript(int64_t now_us) {
    if (now_us - live_user_last_update_us_ < kTypingUpdateIntervalUs) {
        return;
    }
    if (esp_timer_get_time() - now_us > 18000 &&
        now_us - live_user_last_update_us_ < kTypingMaxElapsedUs &&
        now_us < live_user_reveal_deadline_us_) {
        return;
    }
    live_user_last_update_us_ = now_us;
    if (now_us >= live_user_reveal_deadline_us_) {
        live_user_position_ = live_user_text_.size();
    } else {
        for (int glyph = 0; glyph < 2 && live_user_position_ < live_user_text_.size(); ++glyph) {
            live_user_position_ = NextResponseGlyph(live_user_text_, live_user_position_);
        }
    }
    const size_t previous_start = live_user_window_start_;
    live_user_window_start_ = ResponseTailWindowStart(live_user_text_, live_user_position_, previous_start);
    RenderResponseText(live_user_text_, live_user_position_, live_user_window_start_,
                       live_user_window_start_ == 0 ? "" : "… ",
                       live_user_window_start_ - previous_start);
    UpdateResponseCursor((now_us / 500000) % 2 == 0);
}

void MochanDisplay::ResetLiveUserTranscript() {
    live_user_transcript_active_ = false;
    live_user_text_.clear();
    live_user_position_ = 0;
    live_user_window_start_ = 0;
    live_user_last_update_us_ = 0;
    live_user_reveal_deadline_us_ = 0;
}

void MochanDisplay::ClearLiveUserTranscript() {
    DisplayLockGuard lock(this);
    if (!lock || !live_user_transcript_active_) {
        return;
    }
    ResetLiveUserTranscript();
    RenderResponseText("", 0, 0, "");
    UpdateResponseCursor(false);
    lv_obj_add_flag(subtitle_, LV_OBJ_FLAG_HIDDEN);
    const bool notification_visible = notification_ != nullptr &&
                                      !lv_obj_has_flag(notification_, LV_OBJ_FLAG_HIDDEN);
    const bool preview_visible = preview_show_pending_ ||
                                 (camera_image_ != nullptr &&
                                  !lv_obj_has_flag(camera_image_, LV_OBJ_FLAG_HIDDEN));
    if (!notification_visible && !preview_visible) {
        ResetResponseTextScroll();
        HideResponseBox();
    }
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
    if (!lock) {
        return;
    }
    const bool commits_live_user = live_user_transcript_active_ && role != nullptr &&
                                   std::strcmp(role, "user") == 0;
    const size_t previous_live_window = live_user_window_start_;
    const bool extends_live_text = commits_live_user && visible_text.size() >= live_user_text_.size() &&
                                  visible_text.compare(0, live_user_text_.size(), live_user_text_) == 0;
    ResetLiveUserTranscript();
    if (content[0] != '\0') {
        CancelCameraAttentionLocked();
        HideDeskModeLocked();
        FreezeMouthForExit();
    }
    CancelAmbientAnimations();
    lv_obj_set_style_text_align(subtitle_, LV_TEXT_ALIGN_LEFT, 0);
    if (content[0] == '\0') {
        ResetTyping();
        RenderResponseText("", 0, 0, "");
        lv_obj_add_flag(subtitle_, LV_OBJ_FLAG_HIDDEN);
        if (lv_obj_has_flag(notification_, LV_OBJ_FLAG_HIDDEN)) {
            HideResponseBox();
        }
    } else {
        if (is_assistant) {
            StartTyping(content);
        } else {
            ResetTyping(!commits_live_user);
            const size_t window_start = ResponseTailWindowStart(visible_text, visible_text.size(), 0);
            const size_t dropped_bytes = extends_live_text && window_start > previous_live_window
                ? window_start - previous_live_window : 0;
            // Final is authoritative: cancel pending partial reveal and replace
            // immediately, keeping scroll continuity for the accepted turn.
            RenderResponseText(visible_text, visible_text.size(), window_start,
                               window_start == 0 ? "" : "… ", dropped_bytes);
            UpdateResponseCursor(false);
        }
        lv_obj_add_flag(notification_, LV_OBJ_FLAG_HIDDEN);
        ShowResponseBox();
        lv_obj_remove_flag(subtitle_, LV_OBJ_FLAG_HIDDEN);
        if (!is_assistant) {
            UpdateResponseTextScroll();
        }
    }
}

void MochanDisplay::ClearChatMessages() { SetChatMessage("system", ""); }

void MochanDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    if (camera_image_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    if (image != nullptr) {
        CancelCameraAttentionLocked();
        HideDeskModeLocked();
        FreezeMouthForExit();
    }
    CancelAmbientAnimations();
    if (image == nullptr) {
        esp_timer_stop(preview_timer_);
        preview_show_pending_ = false;
        lv_obj_add_flag(camera_image_, LV_OBJ_FLAG_HIDDEN);
        camera_image_cached_.reset();
        if (lv_label_get_text(subtitle_)[0] != '\0' ||
            live_user_transcript_active_ || typing_active_) {
            RenderTypingText();
            ShowResponseBox();
            lv_obj_remove_flag(subtitle_, LV_OBJ_FLAG_HIDDEN);
            UpdateResponseTextScroll();
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
    ResetResponseTextScroll();
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
        lv_label_get_text(subtitle_)[0] == '\0' &&
        !live_user_transcript_active_ && !typing_active_) {
        lv_obj_add_flag(subtitle_, LV_OBJ_FLAG_HIDDEN);
        HideResponseBox();
    } else if (subtitle_ != nullptr) {
        lv_obj_remove_flag(subtitle_, LV_OBJ_FLAG_HIDDEN);
        UpdateResponseTextScroll();
    }
    notification_timer_ = nullptr;
}

void MochanDisplay::ShowBootSplash() {
    if (splash_ != nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    CancelCameraAttentionLocked();
    HideDeskModeLocked();
    CancelAmbientAnimations();
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
