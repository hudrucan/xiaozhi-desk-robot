#pragma once

#include <lvgl.h>

#include <ctime>

class DeskModeView {
public:
    void Setup(lv_obj_t* parent, int height);
    void Show();
    void Hide();
    void UpdateClock(time_t now);
    void SetUse24Hour(bool use_24_hour);
    void SetEnvironment(bool temperature_valid, float temperature_c,
                        bool humidity_valid, float humidity_percent);

private:
    void UpdateClimate();

    lv_obj_t* container_ = nullptr;
    lv_obj_t* clock_group_ = nullptr;
    lv_obj_t* time_ = nullptr;
    lv_obj_t* date_ = nullptr;
    lv_obj_t* divider_ = nullptr;
    lv_obj_t* climate_separator_ = nullptr;
    lv_obj_t* temperature_cell_ = nullptr;
    lv_obj_t* humidity_cell_ = nullptr;
    lv_obj_t* temperature_icon_ = nullptr;
    lv_obj_t* humidity_icon_ = nullptr;
    lv_obj_t* temperature_ = nullptr;
    lv_obj_t* temperature_label_ = nullptr;
    lv_obj_t* humidity_ = nullptr;
    lv_obj_t* humidity_label_ = nullptr;
    bool temperature_valid_ = false;
    bool humidity_valid_ = false;
    int temperature_c_ = 0;
    int humidity_percent_ = 0;
    time_t displayed_minute_ = -1;
    bool use_24_hour_ = true;
};
