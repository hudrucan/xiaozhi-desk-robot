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
    void UpdateNeedle(lv_obj_t* needle_line, lv_obj_t* needle_dot, int value, int min_val, int max_val);
    static lv_color_t GetTemperatureColor(int temp_c);

    // Container gốc
    lv_obj_t* container_ = nullptr;

    // Khoang trên: Đồng hồ & Ngày tháng (y: 34 -> 132)
    lv_obj_t* clock_group_ = nullptr;
    lv_obj_t* time_ = nullptr;
    lv_obj_t* date_ = nullptr;

    // Thanh phân cách Vernier trung tâm (y = 132)
    lv_obj_t* vernier_line_l_ = nullptr;
    lv_obj_t* vernier_line_r_ = nullptr;
    lv_obj_t* vernier_capsule_ = nullptr;
    lv_obj_t* climate_separator_ = nullptr;

    // Khoang dưới: Nhiệt độ (TEMP)
    lv_obj_t* temperature_cell_ = nullptr;
    lv_obj_t* temperature_ = nullptr;
    lv_obj_t* temp_needle_line_ = nullptr;
    lv_obj_t* temp_needle_dot_ = nullptr;

    // Khoang dưới: Độ ẩm (HUMIDITY)
    lv_obj_t* humidity_cell_ = nullptr;
    lv_obj_t* humidity_ = nullptr;
    lv_obj_t* hum_needle_line_ = nullptr;
    lv_obj_t* hum_needle_dot_ = nullptr;

    // Trạng thái hệ thống & Cảm biến
    bool temperature_valid_ = false;
    bool humidity_valid_ = false;
    int temperature_c_ = 0;
    int humidity_percent_ = 0;
    time_t displayed_minute_ = -1;
    bool use_24_hour_ = true;
};
