#include "desk_mode_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>

LV_FONT_DECLARE(font_noto_sans_basic_16_4);
LV_FONT_DECLARE(font_noto_sans_basic_14_1);
extern const lv_font_t desk_mode_montserrat_52;

namespace {

constexpr int kLayoutOffsetY = -8;

const lv_color_t kBackground = LV_COLOR_MAKE(0x00, 0x00, 0x00);
const lv_color_t kBrass = LV_COLOR_MAKE(0xe3, 0xc2, 0x7b);
const lv_color_t kBrassHighlight = LV_COLOR_MAKE(0xff, 0xe4, 0x9b);
constexpr lv_point_precise_t kDividerPoints[] = {
    {0, 0}, {76, 0}, {80, 3}, {102, 3}, {106, 0}, {182, 0},
};
constexpr lv_point_precise_t kHumidityIconPoints[] = {
    {7, 0}, {3, 7}, {0, 13}, {0, 16}, {2, 19}, {5, 21},
    {9, 21}, {12, 19}, {14, 16}, {14, 13}, {11, 7}, {7, 0},
};
constexpr lv_point_precise_t kHumidityShinePoints[] = {
    {4, 13}, {4, 16}, {6, 18},
};
constexpr lv_point_precise_t kThermometerPoints[] = {
    {3, 13}, {3, 3}, {4, 1}, {6, 0}, {8, 1}, {9, 3}, {9, 13},
    {11, 15}, {12, 18}, {11, 21}, {9, 23}, {6, 24}, {3, 23},
    {1, 21}, {0, 18}, {1, 15}, {3, 13},
};
constexpr lv_point_precise_t kMercuryPoints[] = {
    {6, 8}, {6, 18},
};

lv_obj_t* Group(lv_obj_t* parent, int width, int height) {
    auto* object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_size(object, width, height);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    return object;
}

template <size_t N>
lv_obj_t* Outline(lv_obj_t* parent, const lv_point_precise_t (&points)[N]) {
    auto* line = lv_line_create(parent);
    lv_line_set_points(line, points, N);
    lv_obj_set_style_line_color(line, kBrassHighlight, 0);
    lv_obj_set_style_line_width(line, 2, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    return line;
}

void SetVisible(lv_obj_t* object, bool visible) {
    if (visible) lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
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

    // Reference inner display mapped to 240 x 240. The existing status bar
    // occupies y=0..23; all positions below are relative to y=24.
    clock_group_ = Group(container_, 194, 58);
    lv_obj_align(clock_group_, LV_ALIGN_TOP_MID, 0, 32 + kLayoutOffsetY);
    time_ = lv_label_create(clock_group_);
    lv_label_set_text(time_, "00:00");
    lv_obj_set_style_text_color(time_, kBrassHighlight, 0);
    lv_obj_set_style_text_align(time_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(time_, &desk_mode_montserrat_52, 0);
    lv_obj_set_style_text_letter_space(time_, 7, 0);
    lv_obj_align(time_, LV_ALIGN_TOP_MID, 0, 0);

    date_ = lv_label_create(container_);
    lv_label_set_text(date_, "SAT • 26 SEP");
    lv_obj_set_style_text_color(date_, kBrassHighlight, 0);
    lv_obj_set_style_text_align(date_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(date_, 3, 0);
    lv_obj_set_style_text_font(date_, &lv_font_montserrat_18, 0);
    lv_obj_align(date_, LV_ALIGN_TOP_MID, 0, 94 + kLayoutOffsetY);

    divider_ = lv_line_create(container_);
    lv_line_set_points(divider_, kDividerPoints, std::size(kDividerPoints));
    lv_obj_set_size(divider_, 183, 4);
    lv_obj_set_style_line_color(divider_, kBrass, 0);
    lv_obj_set_style_line_width(divider_, 1, 0);
    lv_obj_align(divider_, LV_ALIGN_TOP_MID, 0, 131 + kLayoutOffsetY);

    climate_separator_ = Group(container_, 1, 31);
    lv_obj_set_style_bg_color(climate_separator_, kBrass, 0);
    lv_obj_set_style_bg_opa(climate_separator_, LV_OPA_COVER, 0);
    lv_obj_align(climate_separator_, LV_ALIGN_TOP_MID, 0, 149 + kLayoutOffsetY);

    temperature_cell_ = Group(container_, 75, 42);
    humidity_cell_ = Group(container_, 75, 42);
    temperature_icon_ = Group(temperature_cell_, 16, 28);
    lv_obj_align(temperature_icon_, LV_ALIGN_LEFT_MID, 0, 0);
    auto* outline = Outline(temperature_icon_, kThermometerPoints);
    lv_obj_set_pos(outline, 2, 1);
    auto* mercury = Outline(temperature_icon_, kMercuryPoints);
    lv_obj_set_pos(mercury, 2, 1);
    auto* bulb = Group(temperature_icon_, 5, 5);
    lv_obj_set_style_bg_color(bulb, kBrassHighlight, 0);
    lv_obj_set_style_bg_opa(bulb, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bulb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_pos(bulb, 6, 17);

    humidity_icon_ = Group(humidity_cell_, 18, 28);
    lv_obj_align(humidity_icon_, LV_ALIGN_LEFT_MID, 0, 0);
    auto* drop = Outline(humidity_icon_, kHumidityIconPoints);
    lv_obj_set_pos(drop, 2, 2);
    auto* shine = Outline(humidity_icon_, kHumidityShinePoints);
    lv_obj_set_pos(shine, 2, 2);

    // Center the icon against both text rows. The 23 px text offset leaves
    // at least 5 px between the icon box and the shared, centered text column.
    temperature_ = lv_label_create(temperature_cell_);
    temperature_label_ = lv_label_create(temperature_cell_);
    humidity_ = lv_label_create(humidity_cell_);
    humidity_label_ = lv_label_create(humidity_cell_);
    for (auto* value : {temperature_, humidity_}) {
        lv_obj_set_style_text_color(value, kBrassHighlight, 0);
        lv_obj_set_style_text_font(value, &font_noto_sans_basic_16_4, 0);
        lv_obj_set_width(value, 50);
        lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(value, 23, 0);
    }
    for (auto* label : {temperature_label_, humidity_label_}) {
        lv_obj_set_style_text_color(label, kBrass, 0);
        lv_obj_set_style_text_font(label, &font_noto_sans_basic_14_1, 0);
        lv_obj_set_width(label, 50);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(label, 23, 23);
    }
    lv_label_set_text(temperature_label_, "TEMP");
    lv_label_set_text(humidity_label_, "HUM");
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
    if (time_ == nullptr || now <= 0) {
        return;
    }
    const time_t minute = now / 60;
    if (minute == displayed_minute_) {
        return;
    }
    struct tm local_time = {};
    if (localtime_r(&now, &local_time) == nullptr || local_time.tm_year < 2025 - 1900) {
        return;
    }
    static constexpr const char* kWeekdays[] = {"SUN", "MON", "TUE", "WED",
                                                "THU", "FRI", "SAT"};
    static constexpr const char* kMonths[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                              "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    char time_text[6] = {};
    char date_text[16] = {};
    int display_hour = local_time.tm_hour;
    if (!use_24_hour_) {
        display_hour %= 12;
        if (display_hour == 0) {
            display_hour = 12;
        }
    }
    std::snprintf(time_text, sizeof(time_text), "%02d:%02d", display_hour,
                  local_time.tm_min);
    std::snprintf(date_text, sizeof(date_text), "%s • %02d %s",
                  kWeekdays[local_time.tm_wday], local_time.tm_mday,
                  kMonths[local_time.tm_mon]);
    lv_label_set_text(time_, time_text);
    lv_label_set_text(date_, date_text);
    displayed_minute_ = minute;
}

void DeskModeView::SetUse24Hour(bool use_24_hour) {
    if (use_24_hour_ == use_24_hour) {
        return;
    }
    use_24_hour_ = use_24_hour;
    displayed_minute_ = -1;
    UpdateClock(time(nullptr));
}

void DeskModeView::SetEnvironment(bool temperature_valid, float temperature_c,
                                  bool humidity_valid, float humidity_percent) {
    const bool safe_temperature = temperature_valid && std::isfinite(temperature_c);
    const bool safe_humidity = humidity_valid && std::isfinite(humidity_percent);
    const int rounded_temperature =
        safe_temperature ? static_cast<int>(std::lround(temperature_c)) : 0;
    const int rounded_humidity = safe_humidity
                                     ? std::clamp(
                                           static_cast<int>(std::lround(humidity_percent)), 0, 100)
                                     : 0;
    if (safe_temperature == temperature_valid_ && safe_humidity == humidity_valid_ &&
        rounded_temperature == temperature_c_ && rounded_humidity == humidity_percent_) {
        return;
    }
    temperature_valid_ = safe_temperature;
    humidity_valid_ = safe_humidity;
    temperature_c_ = rounded_temperature;
    humidity_percent_ = rounded_humidity;
    UpdateClimate();
}

void DeskModeView::UpdateClimate() {
    if (temperature_ == nullptr) {
        return;
    }
    const bool climate_valid = temperature_valid_ || humidity_valid_;
    SetVisible(divider_, climate_valid);
    SetVisible(climate_separator_, temperature_valid_ && humidity_valid_);
    SetVisible(temperature_cell_, temperature_valid_);
    SetVisible(humidity_cell_, humidity_valid_);
    lv_obj_align(clock_group_, LV_ALIGN_TOP_MID, 0,
                 (climate_valid ? 32 : 55) + kLayoutOffsetY);
    lv_obj_align(date_, LV_ALIGN_TOP_MID, 0,
                 (climate_valid ? 94 : 117) + kLayoutOffsetY);
    char value[16] = {};
    if (temperature_valid_) {
        std::snprintf(value, sizeof(value), "%d°C", temperature_c_);
        lv_label_set_text(temperature_, value);
    }
    if (humidity_valid_) {
        std::snprintf(value, sizeof(value), "%d%%", humidity_percent_);
        lv_label_set_text(humidity_, value);
    }
    lv_obj_align(temperature_cell_, LV_ALIGN_TOP_MID, humidity_valid_ ? -48 : 0,
                 147 + kLayoutOffsetY);
    lv_obj_align(humidity_cell_, LV_ALIGN_TOP_MID, temperature_valid_ ? 63 : 0,
                 147 + kLayoutOffsetY);
}
