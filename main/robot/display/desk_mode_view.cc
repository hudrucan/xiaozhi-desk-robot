#include "desk_mode_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>

LV_FONT_DECLARE(font_noto_sans_basic_16_4);
LV_FONT_DECLARE(font_noto_sans_basic_14_1);
extern const lv_font_t desk_mode_montserrat_52;

namespace {

// Palette constants
const lv_color_t kBackground = LV_COLOR_MAKE(0x00, 0x00, 0x00);
const lv_color_t kChampagneClock = LV_COLOR_MAKE(0xff, 0xea, 0xa0);
const lv_color_t kBrassDate = LV_COLOR_MAKE(0xa8, 0x85, 0x44);
const lv_color_t kBrassDark = LV_COLOR_MAKE(0x42, 0x2d, 0x13);
const lv_color_t kBrassCorner = LV_COLOR_MAKE(0x7d, 0x57, 0x26);
const lv_color_t kCitrineGold = LV_COLOR_MAKE(0xff, 0xce, 0x38);
const lv_color_t kCitrineMuted = LV_COLOR_MAKE(0x7a, 0x63, 0x20);
const lv_color_t kTempMuted = LV_COLOR_MAKE(0x8f, 0x2f, 0x19);

// Chamfered corner coordinates mapped to local container coordinates (y_local = y_screen - 24)
constexpr lv_point_precise_t kCornerTL[] = {{14, 38}, {14, 20}, {24, 10}, {42, 10}};
constexpr lv_point_precise_t kCornerTR[] = {{198, 10}, {216, 10}, {226, 20}, {226, 38}};
constexpr lv_point_precise_t kCornerBL[] = {{14, 178}, {14, 196}, {24, 206}, {42, 206}};
constexpr lv_point_precise_t kCornerBR[] = {{198, 206}, {216, 206}, {226, 196}, {226, 178}};

// Dashed frame outer edges
constexpr lv_point_precise_t kFrameEdgeTop[] = {{42, 10}, {198, 10}};
constexpr lv_point_precise_t kFrameEdgeBottom[] = {{42, 206}, {198, 206}};
constexpr lv_point_precise_t kFrameEdgeLeft[] = {{14, 38}, {14, 178}};
constexpr lv_point_precise_t kFrameEdgeRight[] = {{226, 38}, {226, 178}};

// Laser-etched scale track (60px) with graduations
constexpr lv_point_precise_t kScaleTicks[] = {
    {0, 4},  {0, 0},
    {15, 0}, {15, 3}, {15, 0},
    {30, 0}, {30, 4}, {30, 0},
    {44, 0}, {44, 3}, {44, 0},
    {59, 0}, {59, 4},
};

constexpr int kScaleMaxX = 59;
constexpr int kNeedleMinCenterX = 1;
constexpr int kNeedleMaxCenterX = 57;

lv_obj_t* CreatePanel(lv_obj_t* parent, int width, int height) {
    auto* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, width, height);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

void SetVisible(lv_obj_t* object, bool visible) {
    if (object == nullptr)
        return;
    if (visible)
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace

void DeskModeView::Setup(lv_obj_t* parent, int height) {
    if (container_ != nullptr || parent == nullptr) {
        return;
    }

    container_ = lv_obj_create(parent);
    lv_obj_set_size(container_, LV_PCT(100), height - 24);
    lv_obj_align(container_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(container_, kBackground, 0);
    lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_scrollbar_mode(container_, LV_SCROLLBAR_MODE_OFF);

    // 1. Mechanical chassis frame
    for (const auto* edge : {kFrameEdgeTop, kFrameEdgeBottom, kFrameEdgeLeft, kFrameEdgeRight}) {
        auto* line = lv_line_create(container_);
        lv_line_set_points(line, edge, 2);
        lv_obj_set_style_line_color(line, lv_color_make(0x4a, 0x33, 0x17), 0);
        lv_obj_set_style_line_width(line, 1, 0);
        lv_obj_set_style_line_dash_width(line, 5, 0);
        lv_obj_set_style_line_dash_gap(line, 3, 0);
    }

    for (const auto* corner : {kCornerTL, kCornerTR, kCornerBL, kCornerBR}) {
        auto* line = lv_line_create(container_);
        lv_line_set_points(line, corner, 4);
        lv_obj_set_style_line_color(line, kBrassCorner, 0);
        lv_obj_set_style_line_width(line, 1, 0);
    }

    // 2. Upper section: Clock & Date
    clock_group_ = CreatePanel(container_, 200, 82);
    lv_obj_align(clock_group_, LV_ALIGN_TOP_MID, 0, 17);

    time_ = lv_label_create(clock_group_);
    lv_label_set_text(time_, "00:00");
    lv_obj_set_style_text_color(time_, kChampagneClock, 0);
    lv_obj_set_style_text_font(time_, &desk_mode_montserrat_52, 0);
    lv_obj_set_style_text_letter_space(time_, 2, 0);
    lv_obj_align(time_, LV_ALIGN_TOP_MID, 0, 0);

    date_ = lv_label_create(clock_group_);
    lv_label_set_text(date_, "SAT · 26 SEP");
    lv_obj_set_style_text_color(date_, kBrassDate, 0);
    lv_obj_set_style_text_font(date_, &font_noto_sans_basic_16_4, 0);
    lv_obj_set_style_text_letter_space(date_, 2, 0);
    lv_obj_align(date_, LV_ALIGN_TOP_MID, 0, 58);

    // 3. Center vernier scale divider (y_local = 108)
    static constexpr lv_point_precise_t kVernierL[] = {{30, 108}, {100, 108}};
    static constexpr lv_point_precise_t kVernierR[] = {{140, 108}, {210, 108}};

    vernier_line_l_ = lv_line_create(container_);
    lv_line_set_points(vernier_line_l_, kVernierL, 2);
    lv_obj_set_style_line_color(vernier_line_l_, kBrassDark, 0);
    lv_obj_set_style_line_width(vernier_line_l_, 1, 0);

    vernier_line_r_ = lv_line_create(container_);
    lv_line_set_points(vernier_line_r_, kVernierR, 2);
    lv_obj_set_style_line_color(vernier_line_r_, kBrassDark, 0);
    lv_obj_set_style_line_width(vernier_line_r_, 1, 0);

    vernier_capsule_ = CreatePanel(container_, 28, 10);
    lv_obj_set_style_bg_color(vernier_capsule_, lv_color_make(0x14, 0x0e, 0x06), 0);
    lv_obj_set_style_bg_opa(vernier_capsule_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(vernier_capsule_, lv_color_make(0x6e, 0x4d, 0x1f), 0);
    lv_obj_set_style_border_width(vernier_capsule_, 1, 0);
    lv_obj_set_style_radius(vernier_capsule_, 2, 0);
    lv_obj_align(vernier_capsule_, LV_ALIGN_TOP_MID, 0, 103);

    auto* dot = CreatePanel(vernier_capsule_, 3, 3);
    lv_obj_set_style_bg_color(dot, kChampagneClock, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(dot, LV_ALIGN_CENTER, 0, 0);

    // Separator line between climate cells (y = 124..166)
    static constexpr lv_point_precise_t kSepPoints[] = {{120, 124}, {120, 166}};
    climate_separator_ = lv_line_create(container_);
    lv_line_set_points(climate_separator_, kSepPoints, 2);
    lv_obj_set_style_line_color(climate_separator_, lv_color_make(0x26, 0x1a, 0x0c), 0);
    lv_obj_set_style_line_width(climate_separator_, 1, 0);
    lv_obj_set_style_line_dash_width(climate_separator_, 2, 0);
    lv_obj_set_style_line_dash_gap(climate_separator_, 3, 0);

    // 4. Lower section: Temperature cell (centered at x = 67, offset = -53, y = 126)
    temperature_cell_ = CreatePanel(container_, 60, 42);
    lv_obj_align(temperature_cell_, LV_ALIGN_TOP_MID, -53, 126);

    auto* temperature_label = lv_label_create(temperature_cell_);
    lv_label_set_text(temperature_label, "TEMP");
    lv_obj_set_style_text_color(temperature_label, kTempMuted, 0);
    lv_obj_set_style_text_font(temperature_label, &font_noto_sans_basic_14_1, 0);
    lv_obj_align(temperature_label, LV_ALIGN_TOP_LEFT, 0, 0);

    temperature_ = lv_label_create(temperature_cell_);
    lv_label_set_text(temperature_, "--°");
    lv_obj_set_style_text_font(temperature_, &font_noto_sans_basic_16_4, 0);
    lv_obj_align(temperature_, LV_ALIGN_TOP_LEFT, 0, 14);

    auto* temp_scale = lv_line_create(temperature_cell_);
    lv_line_set_points(temp_scale, kScaleTicks, std::size(kScaleTicks));
    lv_obj_set_style_line_color(temp_scale, lv_color_make(0x3d, 0x18, 0x10), 0);
    lv_obj_set_style_line_width(temp_scale, 1, 0);
    lv_obj_align(temp_scale, LV_ALIGN_TOP_LEFT, 0, 36);

    temp_needle_line_ = CreatePanel(temperature_cell_, 2, 6);
    lv_obj_set_style_bg_opa(temp_needle_line_, LV_OPA_COVER, 0);
    lv_obj_set_pos(temp_needle_line_, 0, 31);

    temp_needle_dot_ = CreatePanel(temperature_cell_, 4, 4);
    lv_obj_set_style_bg_opa(temp_needle_dot_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(temp_needle_dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_pos(temp_needle_dot_, -1, 29);

    // 5. Lower section: Humidity cell (centered at x = 173, offset = +53, y = 126)
    humidity_cell_ = CreatePanel(container_, 60, 42);
    lv_obj_align(humidity_cell_, LV_ALIGN_TOP_MID, 53, 126);

    auto* humidity_label = lv_label_create(humidity_cell_);
    lv_label_set_text(humidity_label, "HUM");
    lv_obj_set_style_text_color(humidity_label, kCitrineMuted, 0);
    lv_obj_set_style_text_font(humidity_label, &font_noto_sans_basic_14_1, 0);
    lv_obj_align(humidity_label, LV_ALIGN_TOP_LEFT, 0, 0);

    humidity_ = lv_label_create(humidity_cell_);
    lv_label_set_text(humidity_, "--%");
    lv_obj_set_style_text_color(humidity_, kCitrineGold, 0);
    lv_obj_set_style_text_font(humidity_, &font_noto_sans_basic_16_4, 0);
    lv_obj_align(humidity_, LV_ALIGN_TOP_LEFT, 0, 14);

    auto* hum_scale = lv_line_create(humidity_cell_);
    lv_line_set_points(hum_scale, kScaleTicks, std::size(kScaleTicks));
    lv_obj_set_style_line_color(hum_scale, lv_color_make(0x38, 0x2e, 0x10), 0);
    lv_obj_set_style_line_width(hum_scale, 1, 0);
    lv_obj_align(hum_scale, LV_ALIGN_TOP_LEFT, 0, 36);

    hum_needle_line_ = CreatePanel(humidity_cell_, 2, 6);
    lv_obj_set_style_bg_color(hum_needle_line_, kCitrineGold, 0);
    lv_obj_set_style_bg_opa(hum_needle_line_, LV_OPA_COVER, 0);
    lv_obj_set_pos(hum_needle_line_, 0, 31);

    hum_needle_dot_ = CreatePanel(humidity_cell_, 4, 4);
    lv_obj_set_style_bg_color(hum_needle_dot_, kCitrineGold, 0);
    lv_obj_set_style_bg_opa(hum_needle_dot_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(hum_needle_dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_pos(hum_needle_dot_, -1, 29);

    // 6. Footer label (y = 178: leaves 12px from scale above and 14px to frame border below)
    auto* footer_label = lv_label_create(container_);
    lv_label_set_text(footer_label, "· DESK ROBOT ·");
    lv_obj_set_style_text_color(footer_label, lv_color_make(0x52, 0x39, 0x16), 0);
    lv_obj_set_style_text_font(footer_label, &font_noto_sans_basic_14_1, 0);
    lv_obj_set_style_text_letter_space(footer_label, 2, 0);
    lv_obj_align(footer_label, LV_ALIGN_TOP_MID, 0, 178);

    UpdateClimate();
    Hide();
}

void DeskModeView::Show() {
    if (container_ != nullptr) {
        lv_obj_remove_flag(container_, LV_OBJ_FLAG_HIDDEN);
    }
}

void DeskModeView::Hide() {
    if (container_ != nullptr) {
        lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
    }
}

void DeskModeView::UpdateClock(time_t now) {
    if (time_ == nullptr || now <= 0)
        return;

    const time_t minute = now / 60;
    if (minute == displayed_minute_)
        return;

    struct tm local_time = {};
    if (localtime_r(&now, &local_time) == nullptr || local_time.tm_year < 2025 - 1900) {
        return;
    }

    static constexpr const char* kWeekdays[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    static constexpr const char* kMonths[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                              "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

    int display_hour = local_time.tm_hour;
    if (!use_24_hour_) {
        display_hour %= 12;
        if (display_hour == 0)
            display_hour = 12;
    }

    char time_text[8];
    std::snprintf(time_text, sizeof(time_text), "%02d:%02d", display_hour, local_time.tm_min);

    char date_text[20];
    std::snprintf(date_text, sizeof(date_text), "%s · %02d %s", kWeekdays[local_time.tm_wday],
                  local_time.tm_mday, kMonths[local_time.tm_mon]);

    lv_label_set_text(time_, time_text);
    lv_label_set_text(date_, date_text);
    displayed_minute_ = minute;
}

void DeskModeView::SetUse24Hour(bool use_24_hour) {
    if (use_24_hour_ == use_24_hour)
        return;
    use_24_hour_ = use_24_hour;
    displayed_minute_ = -1;
    UpdateClock(time(nullptr));
}

void DeskModeView::SetEnvironment(bool temperature_valid, float temperature_c, bool humidity_valid,
                                  float humidity_percent) {
    const bool safe_temp = temperature_valid && std::isfinite(temperature_c);
    const bool safe_hum = humidity_valid && std::isfinite(humidity_percent);

    const int rounded_temp = safe_temp ? static_cast<int>(std::lround(temperature_c)) : 0;
    const int rounded_hum =
        safe_hum ? std::clamp(static_cast<int>(std::lround(humidity_percent)), 0, 100) : 0;

    if (safe_temp == temperature_valid_ && safe_hum == humidity_valid_ &&
        rounded_temp == temperature_c_ && rounded_hum == humidity_percent_) {
        return;
    }

    temperature_valid_ = safe_temp;
    humidity_valid_ = safe_hum;
    temperature_c_ = rounded_temp;
    humidity_percent_ = rounded_hum;

    UpdateClimate();
}

lv_color_t DeskModeView::GetTemperatureColor(int temp_c) {
    if (temp_c <= 20) {
        return lv_color_make(0xff, 0xea, 0xa0);
    } else if (temp_c < 28) {
        float ratio = (temp_c - 20.0f) / 8.0f;
        uint8_t g = 0xea - static_cast<uint8_t>(ratio * (0xea - 0x80));
        uint8_t b = 0xa0 - static_cast<uint8_t>(ratio * (0xa0 - 0x20));
        return lv_color_make(0xff, g, b);
    } else {
        float ratio = std::clamp((temp_c - 28.0f) / 6.0f, 0.0f, 1.0f);
        uint8_t g = 0x80 - static_cast<uint8_t>(ratio * (0x80 - 0x3e));
        uint8_t b = 0x20 - static_cast<uint8_t>(ratio * (0x20 - 0x1b));
        return lv_color_make(0xff, g, b);
    }
}

void DeskModeView::UpdateNeedle(lv_obj_t* needle_line, lv_obj_t* needle_dot, int value, int min_val,
                                int max_val) {
    int clamped = std::clamp(value, min_val, max_val);
    int x_pos = (clamped - min_val) * kScaleMaxX / (max_val - min_val);
    x_pos = std::clamp(x_pos, kNeedleMinCenterX, kNeedleMaxCenterX);
    lv_obj_set_pos(needle_line, x_pos, 31);
    lv_obj_set_pos(needle_dot, x_pos - 1, 29);
}

void DeskModeView::UpdateClimate() {
    if (temperature_ == nullptr)
        return;

    const bool climate_valid = temperature_valid_ || humidity_valid_;

    SetVisible(vernier_line_l_, climate_valid);
    SetVisible(vernier_line_r_, climate_valid);
    SetVisible(vernier_capsule_, climate_valid);
    SetVisible(climate_separator_, temperature_valid_ && humidity_valid_);
    SetVisible(temperature_cell_, temperature_valid_);
    SetVisible(humidity_cell_, humidity_valid_);

    lv_obj_align(clock_group_, LV_ALIGN_TOP_MID, 0, climate_valid ? 17 : 61);

    char val_buf[16];
    if (temperature_valid_) {
        std::snprintf(val_buf, sizeof(val_buf), "%d°", temperature_c_);
        lv_label_set_text(temperature_, val_buf);

        lv_color_t temp_color = GetTemperatureColor(temperature_c_);
        lv_obj_set_style_text_color(temperature_, temp_color, 0);
        lv_obj_set_style_bg_color(temp_needle_line_, temp_color, 0);
        lv_obj_set_style_bg_color(temp_needle_dot_, temp_color, 0);

        UpdateNeedle(temp_needle_line_, temp_needle_dot_, temperature_c_, 0, 50);
    }

    if (humidity_valid_) {
        std::snprintf(val_buf, sizeof(val_buf), "%d%%", humidity_percent_);
        lv_label_set_text(humidity_, val_buf);

        UpdateNeedle(hum_needle_line_, hum_needle_dot_, humidity_percent_, 0, 100);
    }

    // Perfectly balanced symmetrical alignment (offset +/- 53px, y = 126)
    lv_obj_align(temperature_cell_, LV_ALIGN_TOP_MID, humidity_valid_ ? -53 : 0, 126);
    lv_obj_align(humidity_cell_, LV_ALIGN_TOP_MID, temperature_valid_ ? 53 : 0, 126);
}
