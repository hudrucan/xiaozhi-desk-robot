#pragma once

#include <driver/i2c_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <string>

#include "secondary_oled_layout.h"

class SecondaryOled {
public:
    enum class WidgetType : uint8_t {
        kBranding,
        kDistance,
        kPower,
        kMotion,
        kCapacity,
    };

    enum class PowerMode : uint8_t {
        kCurrent,
        kPower,
        kCurrentAndPower,
    };

    enum class MotionMode : uint8_t {
        kState,
        kTilt,
        kStateAndTilt,
    };

    enum class CapacityMode : uint8_t {
        kMah,
        kElapsed,
        kMahAndElapsed,
    };

    using WidgetSize = secondary_oled_layout::WidgetSize;

    struct WidgetConfig {
        WidgetType type = WidgetType::kBranding;
        WidgetSize size = WidgetSize::kSmall;
        bool enabled = true;
        uint8_t mode = 0;
    };

    struct Config {
        bool flip_180 = false;
        std::string brand = "Desk Robot";
        std::string distance_prefix = "Dist";
        std::array<WidgetConfig, secondary_oled_layout::kMaxWidgets> widgets = {{
            {WidgetType::kBranding, WidgetSize::kSmall, true, 0},
            {WidgetType::kDistance, WidgetSize::kSmall, true, 0},
            {WidgetType::kPower, WidgetSize::kSmall, true, 2},
            {WidgetType::kCapacity, WidgetSize::kMedium, true, 2},
            {WidgetType::kMotion, WidgetSize::kMedium, true, 2},
        }};
    };

    struct Telemetry {
        bool distance_valid = false;
        int distance_mm = 0;
        bool power_valid = false;
        int current_ma = 0;
        int power_mw = 0;
        bool motion_valid = false;
        std::string motion_state = "calibrating";
        int roll_deg = 0;
        int pitch_deg = 0;
        bool capacity_active = false;
        bool capacity_measuring = false;
        uint32_t capacity_uah = 0;
        uint32_t capacity_seconds = 0;
    };

    bool Initialize(i2c_master_bus_handle_t bus, std::mutex& bus_mutex, uint8_t address, int width,
                    int height, bool flip_180);
    bool Configure(const Config& config);
    Config GetConfig() const;
    uint8_t GetPageCount() const;
    void UpdateTelemetry(const Telemetry& telemetry);
    void ShowTemporaryText(const std::string& text);
    void ClearTemporaryText();
    void Tick();
    bool IsAvailable() const { return panel_ != nullptr; }

private:
    enum class FontSize : uint8_t {
        kMicro,
        kCompact,
        kRegular,
        kEmphasis,
    };

    static const uint8_t* Glyph(char character);
    static int FontWidth(FontSize font);
    static int FontHeight(FontSize font);
    static int MeasureTextWidth(const std::string& text, FontSize font);
    static FontSize SelectSingleLineFont(const std::string& text, int width, int height,
                                         FontSize preferred);
    static FontSize SelectTwoLineFont(const std::string& first, const std::string& second,
                                      int width, int height, FontSize preferred);
    void Clear();
    void DrawBitmap(int x, int y, const uint8_t* bitmap, int width, int height);
    void DrawHorizontalLine(int x, int y, int width);
    void DrawIconTextFitted(int x, int y, int width, int height, const uint8_t* icon,
                            const std::string& text, FontSize preferred, bool allow_icon);
    void DrawIconTwoLinesFitted(int x, int y, int width, int height, const uint8_t* icon,
                                const std::string& first, const std::string& second,
                                FontSize preferred, bool allow_icon);
    void DrawText(int x, int y, const std::string& text, FontSize font, int max_width);
    void DrawTextFitted(int x, int y, int width, int height, const std::string& text,
                        FontSize preferred = FontSize::kRegular, bool center_horizontal = true);
    void DrawTwoLinesFitted(int x, int y, int width, int height, const std::string& first,
                            const std::string& second,
                            FontSize preferred = FontSize::kCompact,
                            bool center_horizontal = true);
    void DrawVerticalLine(int x, int y, int height);
    void RebuildLayoutLocked();
    void RenderDashboardLocked();
    void RenderLocked();
    void RenderSingleLineWidgetLocked(const secondary_oled_layout::Placement& placement,
                                      const WidgetConfig& widget);
    void RenderTemporaryTextLocked();
    void RenderWidgetLocked(const secondary_oled_layout::Placement& placement);
    void SetPixel(int x, int y);
    bool Flush();

    i2c_master_bus_handle_t bus_ = nullptr;
    std::mutex* bus_mutex_ = nullptr;
    esp_lcd_panel_io_handle_t io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    bool flip_180_ = false;
    unsigned consecutive_flush_failures_ = 0;
    Config config_;
    secondary_oled_layout::Layout layout_;
    Telemetry telemetry_;
    uint8_t current_page_ = 0;
    int64_t next_page_at_us_ = 0;
    bool dirty_ = true;
    std::string temporary_text_;
    mutable std::mutex mutex_;
    std::array<uint8_t, 128 * 32 / 8> framebuffer_ = {};
};
