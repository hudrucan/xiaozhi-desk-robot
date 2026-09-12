#pragma once

#include <driver/i2c_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <string>

class SecondaryOled {
public:
    struct Config {
        bool flip_180 = false;
        bool show_brand = true;
        bool show_state = true;
        bool show_distance = true;
        bool show_battery = true;
        bool show_voltage = false;
        bool show_current = false;
        int text_scale = 2;
        std::string brand = "Xiaozhi";
        std::string distance_prefix = "Dist";
    };

    bool Initialize(i2c_master_bus_handle_t bus, std::mutex& bus_mutex, uint8_t address, int width,
                    int height, bool flip_180);
    bool Configure(const Config& config);
    Config GetConfig() const;
    void ShowStatus(const std::string& state, int distance_mm, bool distance_valid,
                    int battery_percent = -1, float battery_voltage_v = 0.0f,
                    float battery_current_ma = 0.0f);
    void ShowTemporaryText(const std::string& text);
    void ClearTemporaryText();
    void Tick();
    bool IsAvailable() const { return panel_ != nullptr; }

private:
    static const uint8_t* Glyph(char character);
    void Clear();
    void DrawTextScaled(int x, int y, const std::string& text, int scale);
    void RebuildMessageLocked();
    void RenderLocked();
    void SetPixel(int x, int y);
    bool Flush();

    i2c_master_bus_handle_t bus_ = nullptr;
    std::mutex* bus_mutex_ = nullptr;
    esp_lcd_panel_io_handle_t io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int scroll_x_ = 0;
    bool flip_180_ = false;
    unsigned consecutive_flush_failures_ = 0;
    Config config_;
    std::string state_ = "Starting";
    int distance_mm_ = 0;
    bool distance_valid_ = false;
    int battery_percent_ = -1;
    float battery_voltage_v_ = 0.0f;
    float battery_current_ma_ = 0.0f;
    std::string message_;
    std::string temporary_text_;
    mutable std::mutex mutex_;
    std::array<uint8_t, 128 * 32 / 8> framebuffer_ = {};
};
