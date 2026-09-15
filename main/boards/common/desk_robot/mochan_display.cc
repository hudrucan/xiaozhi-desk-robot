#include "mochan_display.h"

#include "assets/lang_config.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "src/misc/cache/instance/lv_image_cache.h"

#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <material_symbols.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
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
constexpr int kEyeLayoutOffsetY = 10;
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
constexpr int kMouthIdleGapOffsetY = 8;
constexpr char kTag[] = "MochanDisplay";

constexpr std::array<const char*, 34> kSupportedEmotions = {
    "neutral",    "happy",       "laughing",  "funny",     "sad",        "angry",   "crying",
    "loving",     "embarrassed", "surprised", "shocked",   "thinking",   "winking", "cool",
    "relaxed",    "delicious",   "kissy",     "confident", "sleepy",     "silly",   "confused",
    "suspicious", "shake",       "speaking",  "listening", "left",       "right",   "up",
    "down",       "up_left",     "up_right",  "down_left", "down_right", "bored",
};

struct MouthPoint {
    int8_t x;
    int8_t y;
};

struct MouthGeometry {
    const char* emotion;
    int x;
    int y;
    int width;
    int height;
    int idle_eye_offset_y;
    int base_scale_y;
    int blink_scale_y;
    int idle_open_scale_y;
    const MouthPoint* points;
    size_t point_count;
};

constexpr MouthPoint kNeutralMouth[] = {
    {6, 3},   {15, 5},  {24, 6},  {33, 6},  {42, 6},  {50, 6},  {58, 5},  {66, 3},  {73, 1},
    {76, 1},  {78, 2},  {79, 3},  {80, 6},  {80, 9},  {79, 12}, {76, 15}, {73, 18}, {65, 21},
    {57, 24}, {49, 25}, {40, 25}, {30, 25}, {20, 23}, {13, 21}, {6, 18},  {3, 16},  {2, 14},
    {0, 12},  {0, 10},  {0, 8},   {2, 6},   {3, 4},   {6, 3},
};
constexpr MouthPoint kHappyMouth[] = {
    {4, 1},  {14, 5}, {23, 7}, {33, 9},  {43, 10}, {53, 10}, {64, 8},  {74, 5},  {85, 1},  {88, 0},
    {90, 1}, {90, 3}, {90, 7}, {85, 21}, {76, 32}, {64, 39}, {48, 42}, {34, 41}, {22, 37}, {12, 30},
    {4, 20}, {2, 16}, {1, 12}, {0, 9},   {0, 6},   {0, 4},   {1, 2},   {2, 1},   {4, 1},
};
constexpr MouthPoint kBoredMouth[] = {
    {6, 4},   {19, 5},  {32, 6},  {47, 4},  {63, 2},  {68, 1},  {72, 2},  {75, 3},  {78, 5},
    {80, 8},  {81, 11}, {82, 15}, {82, 20}, {81, 22}, {80, 24}, {78, 24}, {76, 23}, {72, 20},
    {68, 19}, {63, 18}, {58, 19}, {19, 26}, {14, 26}, {9, 25},  {5, 22},  {2, 19},  {1, 17},
    {0, 14},  {0, 12},  {0, 10},  {1, 8},   {2, 6},   {4, 5},   {6, 4},
};
constexpr MouthPoint kSleepyMouth[] = {
    {10, 4},  {17, 2},  {25, 2},  {33, 1},  {42, 2},  {52, 3},  {60, 3},  {69, 2},  {76, 1},
    {80, 1},  {84, 3},  {86, 6},  {88, 10}, {88, 16}, {87, 21}, {84, 26}, {78, 29}, {68, 31},
    {58, 32}, {47, 32}, {35, 31}, {25, 30}, {16, 28}, {9, 26},  {4, 23},  {2, 20},  {0, 18},
    {0, 14},  {1, 11},  {2, 9},   {4, 7},   {7, 5},   {10, 4},
};
constexpr MouthPoint kSurprisedMouth[] = {
    {17, 4},  {26, 2},  {34, 1},  {42, 1},  {49, 3},  {56, 6},  {61, 9},  {65, 14},
    {67, 20}, {69, 28}, {69, 35}, {67, 41}, {63, 47}, {58, 51}, {52, 54}, {45, 56},
    {36, 56}, {27, 55}, {19, 54}, {13, 50}, {7, 46},  {3, 41},  {1, 35},  {0, 29},
    {1, 22},  {3, 16},  {6, 11},  {11, 7},  {17, 4},
};
constexpr MouthPoint kAngryMouth[] = {
    {6, 14},  {14, 10}, {23, 6},  {32, 4},  {40, 2},  {44, 1},  {49, 1},  {53, 2},
    {57, 3},  {65, 5},  {72, 8},  {79, 11}, {86, 15}, {89, 17}, {91, 20}, {92, 23},
    {92, 27}, {92, 30}, {90, 31}, {88, 31}, {85, 30}, {75, 26}, {66, 23}, {57, 22},
    {48, 21}, {39, 21}, {29, 23}, {19, 26}, {8, 30},  {4, 31},  {2, 31},  {0, 30},
    {0, 27},  {0, 23},  {2, 20},  {3, 17},  {6, 14},
};

constexpr std::array<MouthGeometry, 6> kMouthGeometries = {{
    {"neutral", 80, 124, 80, 25, 15, 256, -28, 96, kNeutralMouth, std::size(kNeutralMouth)},
    {"happy", 75, 109, 91, 42, 13, 224, -42, 72, kHappyMouth, std::size(kHappyMouth)},
    {"bored", 79, 116, 83, 26, 14, 256, -20, 80, kBoredMouth, std::size(kBoredMouth)},
    {"sleepy", 76, 120, 89, 33, -2, 256, 34, 112, kSleepyMouth, std::size(kSleepyMouth)},
    {"surprised", 85, 116, 70, 56, 9, 256, -14, 64, kSurprisedMouth, std::size(kSurprisedMouth)},
    {"angry", 74, 121, 93, 32, 13, 256, -24, 72, kAngryMouth, std::size(kAngryMouth)},
}};

const MouthGeometry* FindMouthGeometry(const std::string& emotion) {
    auto found = std::find_if(kMouthGeometries.begin(), kMouthGeometries.end(),
                              [&emotion](const auto& item) { return emotion == item.emotion; });
    return found == kMouthGeometries.end() ? nullptr : &*found;
}

bool PointInMouth(float x, float y, const MouthGeometry& geometry) {
    bool inside = false;
    for (size_t i = 0, previous = geometry.point_count - 1; i < geometry.point_count;
         previous = i++) {
        const auto& a = geometry.points[i];
        const auto& b = geometry.points[previous];
        if (((a.y > y) != (b.y > y)) &&
            x < (b.x - a.x) * (y - a.y) / static_cast<float>(b.y - a.y) + a.x) {
            inside = !inside;
        }
    }
    return inside;
}

bool EndsWithSentencePunctuation(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    const char last = text.back();
    return last == '.' || last == '!' || last == '?' || last == ':' || last == ';';
}

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

int TriangleWave(uint16_t phase, int period, int amplitude) {
    const int position = phase % period;
    const int half = period / 2;
    const int ramp = position < half ? position : period - position;
    return ramp * amplitude * 2 / half - amplitude;
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

    typing_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            static_cast<MochanDisplay*>(lv_timer_get_user_data(timer))->UpdateTyping();
        },
        kTypingPeriodMs, this);
    lv_timer_pause(typing_timer_);
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
    return FindMouthGeometry(emotion) != nullptr;
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
    if (const auto* geometry = FindMouthGeometry(layout_emotion); geometry != nullptr) {
        face_layout_full_offset_y_ = TimedValue(
            face_layout_full_offset_from_y_, face_layout_full_offset_target_y_,
            face_layout_emotion_transition_started_us_, kEmotionLayoutTransitionMs, now_us);
        if (geometry->idle_eye_offset_y != face_layout_full_offset_target_y_) {
            if (face_layout_progress_ == 0) {
                face_layout_full_offset_y_ = geometry->idle_eye_offset_y;
                face_layout_full_offset_from_y_ = geometry->idle_eye_offset_y;
                face_layout_full_offset_target_y_ = geometry->idle_eye_offset_y;
            } else {
                face_layout_full_offset_from_y_ = face_layout_full_offset_y_;
                face_layout_full_offset_target_y_ = geometry->idle_eye_offset_y;
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
    if (FindMouthGeometry(current_emotion_) != nullptr) {
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
    UpdateEyes(blink_amount, idle_eligible);
    UpdateMouth(blink_amount, emotion);
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
    ESP_LOGI(kTag, "Face perf: frame=%lld us (max %lld), callback=%lld us (max %lld)",
             static_cast<long long>(frame_interval_us),
             static_cast<long long>(max_frame_interval_us_),
             static_cast<long long>(callback_duration_us),
             static_cast<long long>(max_callback_duration_us_));
    last_performance_log_us_ = callback_started_us;
    max_frame_interval_us_ = 0;
    max_callback_duration_us_ = 0;
}

bool MochanDisplay::InitializeEyeRasters() {
    constexpr size_t bytes = EyeRaster::kWidth * EyeRaster::kHeight * sizeof(uint32_t);
    for (auto* raster : {&left_raster_, &right_raster_}) {
        // Allocate once in PSRAM, never in the animation/audio loop. If PSRAM
        // is unavailable, preserve the existing lightweight rounded-eye UI.
        raster->pixels =
            static_cast<uint32_t*>(heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (raster->pixels == nullptr) {
            for (auto* allocated : {&left_raster_, &right_raster_}) {
                heap_caps_free(allocated->pixels);
                allocated->pixels = nullptr;
            }
            ESP_LOGW(kTag, "Eye raster allocation unavailable; using rounded eyes");
            return false;
        }
        raster->descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
        raster->descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
        raster->descriptor.header.w = EyeRaster::kWidth;
        raster->descriptor.header.h = EyeRaster::kHeight;
        raster->descriptor.header.stride = EyeRaster::kWidth * sizeof(uint32_t);
        raster->descriptor.data_size = bytes;
        raster->descriptor.data = reinterpret_cast<const uint8_t*>(raster->pixels);
    }
    return true;
}

bool MochanDisplay::InitializeMouthRaster() {
    constexpr size_t bytes = MouthRaster::kWidth * MouthRaster::kHeight * sizeof(uint32_t);
    mouth_raster_.pixels =
        static_cast<uint32_t*>(heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (mouth_raster_.pixels == nullptr) {
        heap_caps_free(mouth_raster_.pixels);
        mouth_raster_.pixels = nullptr;
        ESP_LOGW(kTag, "Mouth raster allocation unavailable; keeping the legacy eye-only face");
        return false;
    }
    mouth_raster_.descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    mouth_raster_.descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
    mouth_raster_.descriptor.header.w = MouthRaster::kWidth;
    mouth_raster_.descriptor.header.h = MouthRaster::kHeight;
    mouth_raster_.descriptor.header.stride = MouthRaster::kWidth * sizeof(uint32_t);
    mouth_raster_.descriptor.data_size = bytes;
    mouth_raster_.descriptor.data = reinterpret_cast<const uint8_t*>(mouth_raster_.pixels);
    return true;
}

bool MochanDisplay::RenderMouthTarget(const std::string& emotion) {
    const auto* geometry = FindMouthGeometry(emotion);
    if (geometry == nullptr || mouth_raster_.pixels == nullptr) {
        return false;
    }
    if (mouth_raster_.rendered_emotion == emotion) {
        return false;
    }
    const int64_t render_started_us = esp_timer_get_time();
    mouth_raster_.rendered_emotion = emotion;

    constexpr int kRasterScreenX = (240 - MouthRaster::kWidth) / 2;
    constexpr int kRasterScreenY = (240 - MouthRaster::kHeight) / 2 + 25;
    constexpr std::array<float, 2> kSamples = {0.25f, 0.75f};
    const int local_x = geometry->x - kRasterScreenX;
    const int local_y = geometry->y - kRasterScreenY;

    std::fill_n(mouth_raster_.pixels, MouthRaster::kWidth * MouthRaster::kHeight, 0);
    for (int y = std::max(0, local_y - 10); y < std::min(MouthRaster::kHeight, local_y + 72); ++y) {
        for (int x = std::max(0, local_x - 2); x < std::min(MouthRaster::kWidth, local_x + 96);
             ++x) {
            int dark_samples = 0;
            int bright_samples = 0;
            for (float sample_y : kSamples) {
                for (float sample_x : kSamples) {
                    const float shape_x = x + sample_x - local_x;
                    const float shape_y = y + sample_y - local_y;
                    if (!PointInMouth(shape_x, shape_y, *geometry)) {
                        continue;
                    }
                    ++dark_samples;
                    // The approved mock clips a brighter copy of the same solid shape,
                    // shifted four pixels right and up, matching the eye's layered brass.
                    if (PointInMouth(shape_x - 4, shape_y + 4, *geometry)) {
                        ++bright_samples;
                    }
                }
            }
            if (dark_samples == 0) {
                continue;
            }
            const uint32_t color = bright_samples * 2 >= dark_samples ? 0xc6a15b : 0x896a36;
            const uint32_t alpha = static_cast<uint32_t>(dark_samples * 255 / 4);
            mouth_raster_.pixels[y * MouthRaster::kWidth + x] = (alpha << 24) | color;
        }
    }
    lv_image_cache_drop(&mouth_raster_.descriptor);
    ESP_LOGI(kTag, "Mouth raster %s: %lld us", emotion.c_str(),
             static_cast<long long>(esp_timer_get_time() - render_started_us));
    return true;
}

void MochanDisplay::UpdateMouth(uint8_t blink_amount, const std::string& current_emotion) {
    if (mouth_ == nullptr || mouth_raster_.pixels == nullptr) {
        return;
    }
    std::string emotion = current_emotion;
    if (face_layout_target_ == 0 && !exiting_mouth_emotion_.empty()) {
        emotion = exiting_mouth_emotion_;
    }
    if (FindMouthGeometry(emotion) == nullptr || face_layout_progress_ == 0) {
        if (!lv_obj_has_flag(mouth_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
        }
        if (face_layout_progress_ == 0) {
            exiting_mouth_emotion_.clear();
        }
        return;
    }

    const auto* geometry = FindMouthGeometry(emotion);
    if (RenderMouthTarget(emotion)) {
        lv_obj_invalidate(mouth_);
    }
    constexpr int kRasterScreenY = (240 - MouthRaster::kHeight) / 2 + 25;
    const int pivot_y = geometry->y - kRasterScreenY + geometry->height / 2;
    if (mouth_raster_.previous_pivot_y != pivot_y) {
        mouth_raster_.previous_pivot_y = pivot_y;
        lv_obj_set_style_transform_pivot_y(mouth_, pivot_y, 0);
    }
    const int layout_scale = 192 + face_layout_progress_ * 64 / 256;
    int expression_scale_x = 256;
    int expression_deformation_y = 0;
    if (emotion == "surprised") {
        const int pulse = TriangleWave(animation_phase_, 30, 2);
        expression_scale_x += pulse * 2;
        expression_deformation_y += pulse * 5;
    } else if (emotion == "angry") {
        const int tension = TriangleWave(animation_phase_, 40, 2);
        expression_scale_x -= tension * 3;
        expression_deformation_y += tension * 3;
    }
    const int scale_x =
        layout_scale * (256 - yawn_amount_ * 24 / 256) / 256 * expression_scale_x / 256;
    const int deformation_y = geometry->base_scale_y + expression_deformation_y +
                              yawn_amount_ * 160 / 256 +
                              mouth_motion_amount_ * geometry->idle_open_scale_y / 256 +
                              blink_amount * geometry->blink_scale_y / 100;
    const int scale_y = layout_scale * deformation_y / 256;
    if (mouth_raster_.previous_scale_x != scale_x) {
        mouth_raster_.previous_scale_x = scale_x;
        lv_image_set_scale_x(mouth_, scale_x);
    }
    if (mouth_raster_.previous_scale_y != scale_y) {
        mouth_raster_.previous_scale_y = scale_y;
        lv_image_set_scale_y(mouth_, scale_y);
    }
    const uint8_t opacity = face_layout_progress_ * LV_OPA_COVER / 256;
    if (mouth_raster_.previous_opacity != opacity) {
        mouth_raster_.previous_opacity = opacity;
        lv_obj_set_style_opa(mouth_, opacity, 0);
    }
    const int mouth_x = idle_gaze_x_ * 3 / 4;
    const int mouth_y = 25 + kMouthIdleGapOffsetY + face_layout_offset_y_ + idle_gaze_y_ * 3 / 4 -
                        yawn_amount_ * 4 / 256 - mouth_motion_amount_ * 2 / 256 + blink_amount / 65;
    if (mouth_raster_.previous_x != mouth_x || mouth_raster_.previous_y != mouth_y) {
        mouth_raster_.previous_x = mouth_x;
        mouth_raster_.previous_y = mouth_y;
        lv_obj_align(mouth_, LV_ALIGN_CENTER, mouth_x, mouth_y);
    }
    if (lv_obj_has_flag(mouth_, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
    }
}

bool MochanDisplay::RenderEyeRaster(EyeRaster& raster, const EyeGeometry& geometry,
                                    uint8_t blink_amount) {
    const auto& previous = raster.previous;
    if (raster.rendered && raster.previous_blink == blink_amount &&
        previous.width == geometry.width && previous.height == geometry.height &&
        previous.top_curve == geometry.top_curve &&
        previous.bottom_curve == geometry.bottom_curve && previous.slope == geometry.slope &&
        previous.water == geometry.water) {
        return false;
    }
    raster.previous = geometry;
    raster.previous_blink = blink_amount;
    raster.rendered = true;
    const float openness = 1.0f - blink_amount / 100.0f;
    const float height = std::max(7.0f, geometry.height * openness);
    const float half_width = geometry.width * 0.5f;
    const auto rounded_distance = [](float x, float y, float half_w, float half_h, float radius) {
        const float qx = std::fabs(x) - half_w + radius;
        const float qy = std::fabs(y) - half_h + radius;
        const float dx = std::max(qx, 0.0f);
        const float dy = std::max(qy, 0.0f);
        return (dx > 0.0f && dy > 0.0f ? std::sqrt(dx * dx + dy * dy) : dx + dy) +
               std::min(std::max(qx, qy), 0.0f) - radius;
    };
    const auto blend = [](uint32_t a, uint32_t b, float amount) {
        uint32_t result = 0;
        for (int shift : {0, 8, 16}) {
            const float first = (a >> shift) & 0xff;
            const float second = (b >> shift) & 0xff;
            result |= static_cast<uint32_t>(first + (second - first) * amount) << shift;
        }
        return result;
    };
    lv_image_cache_drop(&raster.descriptor);
    for (int x = 0; x < EyeRaster::kWidth; ++x) {
        const float px = x + 0.5f - EyeRaster::kWidth * 0.5f;
        const float u = std::clamp(px / half_width, -1.0f, 1.0f);
        const float curve = 1.0f - u * u;
        const float tilt = geometry.slope * u * openness;
        const float top = -height * 0.5f + geometry.top_curve * curve * openness + tilt;
        const float bottom = height * 0.5f + geometry.bottom_curve * curve * openness;
        const float half_height = std::max(3.5f, (bottom - top) * 0.5f);
        const float center = (top + bottom) * 0.5f;
        const float radius = std::min(18.0f, std::min(half_width, half_height));
        // A second, offset copy of the same filled shape exposes a soft left/
        // lower layer. Scale the offset down when squinting or blinking.
        const float layer_scale = std::clamp(height / 54.0f, 0.0f, 1.0f);
        const float inner_x = px - 5.0f * layer_scale;
        const float inner_u = std::clamp(inner_x / half_width, -1.0f, 1.0f);
        const float inner_curve = 1.0f - inner_u * inner_u;
        const float inner_top =
            -height * 0.5f +
            (geometry.top_curve * inner_curve + geometry.slope * inner_u) * openness;
        const float inner_bottom = height * 0.5f + geometry.bottom_curve * inner_curve * openness;
        const float inner_half_height = std::max(3.5f, (inner_bottom - inner_top) * 0.5f);
        const float inner_center = (inner_top + inner_bottom) * 0.5f - 6.0f * layer_scale;
        const float inner_radius = std::min(18.0f, std::min(half_width, inner_half_height));
        for (int y = 0; y < EyeRaster::kHeight; ++y) {
            const float py = y + 0.5f - EyeRaster::kHeight * 0.5f;
            const float distance =
                rounded_distance(px, py - center, half_width, half_height, radius);
            const float coverage = std::clamp(0.5f - distance, 0.0f, 1.0f);
            const float inner_distance = rounded_distance(inner_x, py - inner_center, half_width,
                                                          inner_half_height, inner_radius);
            const float t = std::clamp((1.0f - inner_distance) / 2.0f, 0.0f, 1.0f);
            uint32_t color = blend(0x896a36, 0xc6a15b, t * t * (3.0f - 2.0f * t));
            if (geometry.water > 0) {
                const float outer = geometry.x < 0 ? -u : u;
                const float spread = std::clamp((outer + 0.35f) / 1.35f, 0.0f, 1.0f);
                const float waterline =
                    bottom - geometry.water * openness * spread * spread * (3.0f - 2.0f * spread);
                const float water_mix = std::clamp((py - waterline + 1.0f) / 2.0f, 0.0f, 1.0f);
                color = blend(color, 0x80643b, water_mix);
            }
            raster.pixels[y * EyeRaster::kWidth + x] =
                (static_cast<uint32_t>(coverage * 255.0f) << 24) | color;
        }
    }
    return true;
}

void MochanDisplay::ApplyRoundedEye(lv_obj_t* eye, lv_obj_t* shadow, const EyeGeometry& geometry,
                                    uint8_t blink_amount) {
    auto& raster = eye == left_eye_ ? left_raster_ : right_raster_;
    if (raster.pixels != nullptr) {
        if (!lv_obj_has_flag(shadow, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(shadow, LV_OBJ_FLAG_HIDDEN);
        }
        const bool pixels_changed = RenderEyeRaster(raster, geometry, blink_amount);
        if (!raster.positioned) {
            lv_obj_set_style_transform_pivot_x(eye, EyeRaster::kWidth / 2, 0);
            lv_obj_set_style_transform_pivot_y(eye, EyeRaster::kHeight / 2, 0);
        }
        if (!raster.positioned || raster.displayed_rotation != geometry.rotation) {
            raster.displayed_rotation = geometry.rotation;
            lv_obj_set_style_transform_rotation(eye, geometry.rotation, 0);
        }
        if (!raster.positioned || raster.displayed_x != geometry.x ||
            raster.displayed_y != geometry.y) {
            raster.displayed_x = geometry.x;
            raster.displayed_y = geometry.y;
            lv_obj_align(eye, LV_ALIGN_CENTER, geometry.x, geometry.y);
        }
        raster.positioned = true;
        if (pixels_changed) {
            lv_obj_invalidate(eye);
        }
        return;
    }
    if (raster.rendered && raster.previous_blink == blink_amount &&
        raster.previous.width == geometry.width && raster.previous.height == geometry.height &&
        raster.previous.x == geometry.x && raster.previous.y == geometry.y &&
        raster.previous.rotation == geometry.rotation &&
        raster.previous.top_curve == geometry.top_curve &&
        raster.previous.bottom_curve == geometry.bottom_curve &&
        raster.previous.slope == geometry.slope && raster.previous.water == geometry.water) {
        return;
    }
    raster.previous = geometry;
    raster.previous_blink = blink_amount;
    raster.rendered = true;
    const int height = std::max(7, geometry.height - (geometry.height - 7) * blink_amount / 100);
    const int inset_x = std::min(4, std::max(2, geometry.width / 8));
    const int inset_y = std::min(6, std::max(2, height / 4));
    const int shadow_radius = std::clamp(std::min(geometry.width, height) / 3, 6, 18);

    lv_obj_remove_flag(shadow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(eye, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(shadow, geometry.width, height);
    lv_obj_set_style_radius(shadow, shadow_radius, 0);
    lv_obj_set_style_transform_pivot_x(shadow, geometry.width / 2, 0);
    lv_obj_set_style_transform_pivot_y(shadow, height / 2, 0);
    lv_obj_set_style_transform_rotation(shadow, geometry.rotation, 0);
    lv_obj_align(shadow, LV_ALIGN_CENTER, geometry.x, geometry.y);

    const int bright_width = std::max(8, geometry.width - inset_x);
    const int bright_height = std::max(4, height - inset_y);
    lv_obj_set_size(eye, bright_width, bright_height);
    lv_obj_set_style_radius(eye, std::max(5, shadow_radius - 1), 0);
    lv_obj_set_style_transform_pivot_x(eye, bright_width / 2, 0);
    lv_obj_set_style_transform_pivot_y(eye, bright_height / 2, 0);
    lv_obj_set_style_transform_rotation(eye, geometry.rotation, 0);
    lv_obj_align(eye, LV_ALIGN_CENTER, geometry.x - inset_x / 2, geometry.y - inset_y / 2);
}

void MochanDisplay::UpdateEyes(uint8_t blink_amount, bool idle_eligible) {
    if (left_eye_ == nullptr || right_eye_ == nullptr) {
        return;
    }

    struct EyeTarget {
        EyeGeometry geometry;
    };
    EyeTarget left{{74, 54, -48, -59, 0}};
    EyeTarget right{{74, 54, 48, -59, 0}};

    const int gentle = TriangleWave(animation_phase_, 32, 2);

    switch (face_state_) {
        case FaceState::kListening:
            left.geometry = {78, 54 + gentle, -48, -59, 0};
            right.geometry = {78, 54 + gentle, 48, -59, 0};
            break;
        case FaceState::kSpeaking: {
            const int voice = TriangleWave(animation_phase_, 18, 5);
            left.geometry = {70, 43 + voice, -48, -59, 0};
            right.geometry = {70, 43 - voice, 48, -59, 0};
            break;
        }
        case FaceState::kThinking:
            left.geometry = {60, 54, -53, -63, 0, 3, 0, -5};
            right.geometry = {68, 34, 45, -53, 0, 8, 0, 3};
            break;
        case FaceState::kHappy:
            left.geometry = {74, 43, -47, -56 + gentle, 0, -5, -17};
            right.geometry = {74, 43, 47, -56 + gentle, 0, -5, -17};
            break;
        case FaceState::kLaughing: {
            const int bounce = TriangleWave(animation_phase_, 16, 4);
            left.geometry = {76, 38, -47, -54 + bounce, 0, -9, -19, -2};
            right.geometry = {76, 38, 47, -54 + bounce, 0, -9, -19, 2};
            break;
        }
        case FaceState::kFunny: {
            const int sway = TriangleWave(animation_phase_, 30, 4);
            left.geometry = {54, 61, -48 + sway, -61, -60, 0, -3};
            right.geometry = {72, 40, 48 + sway, -54, 40, -5, -18};
            break;
        }
        case FaceState::kAngry: {
            const int tension = TriangleWave(animation_phase_, 40, 2);
            left.geometry = {74, 43, -45 + tension, -55 + tension / 2, 0, 2, 0, 12};
            right.geometry = {74, 43, 45 - tension, -55 + tension / 2, 0, 2, 0, -12};
            break;
        }
        case FaceState::kSad:
            left.geometry = {72, 49, -47, -55, 0, 10, 0, -9};
            right.geometry = {72, 49, 47, -55, 0, 10, 0, 9};
            break;
        case FaceState::kCrying:
            left.geometry = {72, 49, -47, -55 + gentle, 0, 10, 0, -9, 18};
            right.geometry = {72, 49, 47, -55 + gentle, 0, 10, 0, 9, 18};
            break;
        case FaceState::kLoving: {
            const int pulse = TriangleWave(animation_phase_, 36, 2);
            left.geometry = {62 + pulse, 48 + pulse, -40, -57, 60, -4, -12, 2};
            right.geometry = {62 + pulse, 48 + pulse, 40, -57, -60, -4, -12, -2};
            break;
        }
        case FaceState::kEmbarrassed:
            left.geometry = {59, 38, -51, -47 + gentle, 0, 8, -3, -5};
            right.geometry = {59, 38, 41, -47 + gentle, 0, 8, -3, 5};
            break;
        case FaceState::kSurprised: {
            const int pulse = TriangleWave(animation_phase_, 30, 2);
            const int spread = pulse / 2;
            left.geometry = {49, 65, -44 - spread, -57, 0};
            right.geometry = {49, 65, 44 + spread, -57, 0};
            break;
        }
        case FaceState::kShocked: {
            const int tremble = TriangleWave(animation_phase_, 10, 2);
            left.geometry = {59, 69, -44 + tremble, -57, 0};
            right.geometry = {49, 72, 44 + tremble, -59, 0};
            break;
        }
        case FaceState::kWinking:
            left.geometry = {69, 30, -47, -53, 0, -6, -15};
            right.geometry = {70, 54, 47, -58, 0, 0, -5};
            break;
        case FaceState::kCool:
            left.geometry = {78, 33, -46, -55, 0, 0, 0, -3};
            right.geometry = {78, 33, 46, -55, 0, 0, 0, 3};
            break;
        case FaceState::kRelaxed:
            left.geometry = {71, 40, -47, -54 + gentle, 0, 4, -6};
            right.geometry = {71, 40, 47, -54 + gentle, 0, 4, -6};
            break;
        case FaceState::kDelicious: {
            const int savor = TriangleWave(animation_phase_, 32, 3);
            left.geometry = {69, 41, -45, -54 + savor, 0, -4, -15};
            right.geometry = {69, 41, 45, -54 - savor, 0, -4, -15};
            break;
        }
        case FaceState::kKissy:
            left.geometry = {53, 33, -37, -54, -80, -3, -12};
            right.geometry = {53, 33, 37, -54, 80, -3, -12};
            break;
        case FaceState::kConfident:
            left.geometry = {70, 51, -46, -61, 0, -3, 0, 3};
            right.geometry = {73, 34, 46, -53, 0, 4, -2, -4};
            break;
        case FaceState::kSleepy:
            left.geometry = {71, 28, -46, -48 + gentle, 0, 7, 0};
            right.geometry = {71, 28, 46, -48 + gentle, 0, 7, 0};
            break;
        case FaceState::kSilly: {
            const int sway = TriangleWave(animation_phase_, 24, 4);
            left.geometry = {48, 62, -48 + sway, -64, 80, 0, 0, -3};
            right.geometry = {77, 33, 48 + sway, -46, -80, 2, -8};
            break;
        }
        case FaceState::kConfused:
            left.geometry = {69, 33, -49, -49, 0, 8, 0, -7};
            right.geometry = {54, 57, 48, -63, 0, -3, 0, 3};
            break;
        case FaceState::kSuspicious:
            left.geometry = {74, 31, -40, -52, 0, 6, 0, 3};
            right.geometry = {63, 43, 54, -59, 0, 9, 0, -3};
            break;
        case FaceState::kShake: {
            const int shake = TriangleWave(animation_phase_, 12, 11);
            left.geometry = {72, 46, -48 + shake, -57, 0};
            right.geometry = {72, 46, 48 + shake, -57, 0};
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
        case FaceState::kIdle:
            break;
    }

    int target_gaze_x = 0;
    int target_gaze_y = 0;
    if (idle_eligible) {
        int gaze_amplitude = 8;
        int gaze_down = 4;
        if (face_state_ == FaceState::kHappy) {
            gaze_amplitude = 6;
            gaze_down = 3;
        } else if (face_state_ == FaceState::kCool) {
            gaze_amplitude = 7;
            gaze_down = 3;
        } else if (face_state_ == FaceState::kSleepy) {
            gaze_amplitude = 4;
            gaze_down = 2;
        } else if (face_state_ == FaceState::kSurprised) {
            gaze_amplitude = 5;
            gaze_down = 1;
        }
        const int idle_phase = idle_motion_phase_ % 240;
        if (idle_phase >= 45 && idle_phase < 85) {
            target_gaze_x = -gaze_amplitude;
            target_gaze_y = std::max(1, gaze_down - 1);
        } else if (idle_phase >= 145 && idle_phase < 185) {
            target_gaze_x = gaze_amplitude;
            target_gaze_y = gaze_down;
        }
    }
    idle_gaze_x_ += std::clamp(target_gaze_x - idle_gaze_x_, -1, 1);
    idle_gaze_y_ += std::clamp(target_gaze_y - idle_gaze_y_, -1, 1);
    left.geometry.x += idle_gaze_x_;
    right.geometry.x += idle_gaze_x_;
    left.geometry.y += idle_gaze_y_;
    right.geometry.y += idle_gaze_y_;

    if (idle_eligible) {
        const int positive_mouth_motion = std::max<int>(0, mouth_motion_amount_);
        const int mouth_reaction = positive_mouth_motion * 2 / 256;
        if (face_state_ == FaceState::kSurprised) {
            left.geometry.height += mouth_reaction;
            right.geometry.height += mouth_reaction;
        } else {
            left.geometry.height = std::max(7, left.geometry.height - mouth_reaction);
            right.geometry.height = std::max(7, right.geometry.height - mouth_reaction);
            left.geometry.y -= positive_mouth_motion / 256;
            right.geometry.y -= positive_mouth_motion / 256;
        }
    }

    left.geometry.y += kEyeLayoutOffsetY;
    right.geometry.y += kEyeLayoutOffsetY;

    // Keep the existing smoothing for intrinsic eye/emotion geometry. The
    // layout offset is applied after this block so it follows elapsed time
    // directly and does not acquire a second transition tail.
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
        current.top_curve = smooth(current.top_curve, target.top_curve);
        current.bottom_curve = smooth(current.bottom_curve, target.bottom_curve);
        current.slope = smooth(current.slope, target.slope);
        current.water = smooth(current.water, target.water);
    };
    if (!eye_geometry_initialized_) {
        left_eye_geometry_ = left.geometry;
        right_eye_geometry_ = right.geometry;
        eye_geometry_initialized_ = true;
    } else {
        approach(left_eye_geometry_, left.geometry);
        approach(right_eye_geometry_, right.geometry);
    }

    EyeGeometry displayed_left = left_eye_geometry_;
    EyeGeometry displayed_right = right_eye_geometry_;
    displayed_left.y += face_layout_offset_y_;
    displayed_right.y += face_layout_offset_y_;
    ApplyRoundedEye(left_eye_, left_eyelid_, displayed_left, blink_amount);
    ApplyRoundedEye(right_eye_, right_eyelid_, displayed_right, blink_amount);
}

void MochanDisplay::SetStatus(const char* status) {
    if (status == nullptr) {
        return;
    }
    FaceState next_activity = FaceState::kIdle;
    bool clear_emotion = false;
    if (std::strcmp(status, Lang::Strings::LISTENING) == 0) {
        next_activity = FaceState::kListening;
        clear_emotion = true;
    } else if (std::strcmp(status, Lang::Strings::SPEAKING) == 0) {
        next_activity = FaceState::kSpeaking;
    } else if (std::strcmp(status, Lang::Strings::CONNECTING) == 0 ||
               std::strcmp(status, Lang::Strings::REGISTERING_NETWORK) == 0) {
        next_activity = FaceState::kThinking;
    } else {
        clear_emotion = true;
    }

    DisplayLockGuard lock(this);
    FreezeMouthForExit();
    activity_state_ = next_activity;
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
    DisplayLockGuard lock(this);
    if (content[0] != '\0') {
        FreezeMouthForExit();
    }
    CancelIdleScheduler(true);
    const bool is_assistant = role != nullptr && std::strcmp(role, "assistant") == 0;
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
        ESP_LOGI(kTag, "Showing camera preview: %ux%u", descriptor->header.w, descriptor->header.h);
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
