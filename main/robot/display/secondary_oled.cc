#include "secondary_oled.h"

#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

#define TAG "SecondaryOled"

namespace {

constexpr int kSsd1306SetContrast = 0x81;
constexpr int64_t kGestureEventDurationUs = 1500000LL;
constexpr int64_t kLowBatteryEventDurationUs = 2500000LL;

using secondary_oled_layout::BuildLayout;
using secondary_oled_layout::WidgetSize;

constexpr uint8_t kSmallCapabilities =
    secondary_oled_layout::kFits42x32 | secondary_oled_layout::kFits64x32 |
    secondary_oled_layout::kFits128x16 | secondary_oled_layout::kFits128x32;
constexpr uint8_t kMediumCapabilities = secondary_oled_layout::kFits64x32 |
                                        secondary_oled_layout::kFits128x16 |
                                        secondary_oled_layout::kFits128x32;
constexpr uint8_t kLargeCapabilities = secondary_oled_layout::kFits128x32;

constexpr std::array<secondary_oled_layout::WidgetSpec, secondary_oled_layout::kMaxWidgets>
    kDefaultLayoutWidgets = {{
        {0, WidgetSize::kSmall, true, kSmallCapabilities},
        {1, WidgetSize::kSmall, true, kSmallCapabilities},
        {2, WidgetSize::kSmall, true, kSmallCapabilities},
        {3, WidgetSize::kMedium, true, kMediumCapabilities},
        {4, WidgetSize::kMedium, true, kMediumCapabilities},
    }};
constexpr auto kDefaultLayout = BuildLayout(kDefaultLayoutWidgets);
static_assert(secondary_oled_layout::IsValid(kDefaultLayout));
static_assert(kDefaultLayout.page_count == 2);
static_assert(kDefaultLayout.placement_count == 5);
static_assert(kDefaultLayout.placements[0].page == 0 &&
              kDefaultLayout.placements[0].width == 42 &&
              kDefaultLayout.placements[1].width == 43 &&
              kDefaultLayout.placements[2].width == 43);
static_assert(kDefaultLayout.placements[3].page == 1 &&
              kDefaultLayout.placements[3].width == 64 &&
              kDefaultLayout.placements[4].page == 1 &&
              kDefaultLayout.placements[4].x == 64 &&
              kDefaultLayout.placements[4].width == 64);

constexpr auto kSingleSmallLayout =
    BuildLayout({{{0, WidgetSize::kSmall, true, kSmallCapabilities}}});
static_assert(secondary_oled_layout::IsValid(kSingleSmallLayout));
static_assert(kSingleSmallLayout.page_count == 1 &&
              kSingleSmallLayout.placements[0].width == 128 &&
              kSingleSmallLayout.placements[0].height == 32);

constexpr auto kTwoRowLayout = BuildLayout(
    {{{0, WidgetSize::kMedium, true,
       secondary_oled_layout::kFits128x16 | secondary_oled_layout::kFits128x32},
      {1, WidgetSize::kMedium, true,
       secondary_oled_layout::kFits128x16 | secondary_oled_layout::kFits128x32}}});
static_assert(secondary_oled_layout::IsValid(kTwoRowLayout));
static_assert(kTwoRowLayout.page_count == 1);
static_assert(kTwoRowLayout.placements[0].width == 128 &&
              kTwoRowLayout.placements[0].height == 16 &&
              kTwoRowLayout.placements[1].y == 16 &&
              kTwoRowLayout.placements[1].height == 16);

constexpr auto kThreeLargeLayout =
    BuildLayout({{{0, WidgetSize::kLarge, true, kLargeCapabilities},
                  {1, WidgetSize::kLarge, true, kLargeCapabilities},
                  {2, WidgetSize::kLarge, true, kLargeCapabilities}}});
static_assert(secondary_oled_layout::IsValid(kThreeLargeLayout));
static_assert(kThreeLargeLayout.page_count == 3);
static_assert(kThreeLargeLayout.placements[0].page == 0 &&
              kThreeLargeLayout.placements[1].page == 1 &&
              kThreeLargeLayout.placements[2].page == 2);

constexpr uint8_t kMotionIcon[8] = {
    0x18, 0x5a, 0x3c, 0xff, 0x3c, 0x5a, 0x18, 0x00,
};
constexpr uint8_t kCapacityIcon[8] = {
    0x18, 0x7e, 0x42, 0x5a, 0x5a, 0x5a, 0x7e, 0x00,
};
constexpr uint8_t kWarningIcon[8] = {
    0x40, 0x30, 0x0c, 0x03, 0x0c, 0x30, 0x40, 0x00,
};
constexpr uint8_t kWifiIcon[8] = {
    0x02, 0x05, 0x29, 0x55, 0x29, 0x05, 0x02, 0x00,
};
constexpr uint8_t kTurnIcon[8] = {
    0x18, 0x24, 0x42, 0x42, 0x52, 0x32, 0x70, 0x00,
};
std::pair<std::string, std::string> SplitForTwoLines(const std::string& text) {
    if (text.empty()) {
        return {"", ""};
    }
    if (text.size() <= 10 && text.find(' ') == std::string::npos) {
        return {text, ""};
    }
    // Keep both halves balanced. A distant word boundary can make one half overflow a 42-pixel
    // panel, so only use whitespace immediately adjacent to the midpoint.
    const size_t middle = text.size() / 2;
    size_t split = middle;
    if (middle < text.size() && text[middle] == ' ') {
        split = middle;
    } else if (middle > 0 && text[middle - 1] == ' ') {
        split = middle - 1;
    }
    std::string first = text.substr(0, split);
    std::string second = text.substr(split);
    while (!second.empty() && second.front() == ' ') {
        second.erase(second.begin());
    }
    return {std::move(first), std::move(second)};
}

}  // namespace

bool SecondaryOled::Initialize(i2c_master_bus_handle_t bus, std::mutex& bus_mutex, uint8_t address,
                               int width, int height, bool flip_180) {
    if (width != 128 || height != 32) {
        ESP_LOGE(TAG, "Only SSD1306 128x32 is supported");
        return false;
    }
    if (bus == nullptr) {
        ESP_LOGW(TAG, "Cannot initialize OLED without an I2C bus");
        return false;
    }
    bus_ = bus;
    bus_mutex_ = &bus_mutex;
    esp_err_t error = ESP_OK;
    {
        // Panel IO creation and initialization both transact on the shared bus. Serialize the
        // complete one-time setup just like regular frame transfers.
        std::lock_guard<std::mutex> bus_lock(bus_mutex);
        error = i2c_master_probe(bus_, address, 100);
        if (error != ESP_OK && (address == 0x3c || address == 0x3d)) {
            const uint8_t alternate_address = address == 0x3c ? 0x3d : 0x3c;
            error = i2c_master_probe(bus_, alternate_address, 100);
            if (error == ESP_OK) {
                ESP_LOGW(TAG, "SSD1306 found at 0x%02x instead of configured address 0x%02x",
                         alternate_address, address);
                address = alternate_address;
            }
        }
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "SSD1306 not detected at 0x3c or 0x3d on shared I2C bus");
            return false;
        }

        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = address,
            .scl_speed_hz = 400 * 1000,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .flags = {.dc_low_on_data = 0, .disable_control_phase = 0},
        };
        error = esp_lcd_new_panel_io_i2c(bus_, &io_config, &io_);
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "Cannot create OLED panel IO: %s", esp_err_to_name(error));
            return false;
        }

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(height),
        };
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;
        panel_config.vendor_config = &ssd1306_config;
        error = esp_lcd_new_panel_ssd1306(io_, &panel_config, &panel_);
        if (error != ESP_OK || esp_lcd_panel_reset(panel_) != ESP_OK ||
            esp_lcd_panel_init(panel_) != ESP_OK ||
            esp_lcd_panel_mirror(panel_, flip_180, flip_180) != ESP_OK ||
            esp_lcd_panel_disp_on_off(panel_, true) != ESP_OK) {
            ESP_LOGW(TAG, "Cannot initialize SSD1306 panel");
            panel_ = nullptr;
            return false;
        }
    }

    width_ = width;
    height_ = height;
    flip_180_ = flip_180;
    config_.flip_180 = flip_180;
    RebuildLayoutLocked();
    Clear();
    Flush();
    ESP_LOGI(TAG, "SSD1306 status display ready on shared I2C bus");
    return true;
}

bool SecondaryOled::Configure(const Config& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    Config normalized = config;
    if (normalized.brand.empty()) {
        normalized.brand = "Desk Robot";
    }
    if (normalized.brand.size() > 20) {
        normalized.brand.resize(20);
    }
    if (normalized.distance_prefix.empty()) {
        normalized.distance_prefix = "Dist";
    }
    if (normalized.distance_prefix.size() > 10) {
        normalized.distance_prefix.resize(10);
    }
    std::array<bool, secondary_oled_layout::kMaxWidgets> seen_types = {};
    for (auto& widget : normalized.widgets) {
        const uint8_t type = static_cast<uint8_t>(widget.type);
        if (type >= seen_types.size() || seen_types[type]) {
            ESP_LOGW(TAG, "Rejected duplicate or invalid secondary OLED widget type");
            return false;
        }
        seen_types[type] = true;
        widget.size = static_cast<WidgetSize>(
            std::clamp(static_cast<int>(widget.size), 0, 2));
        widget.mode = static_cast<uint8_t>(std::min<uint8_t>(widget.mode, 2));
    }

    if (panel_ != nullptr && normalized.flip_180 != flip_180_) {
        std::lock_guard<std::mutex> bus_lock(*bus_mutex_);
        const esp_err_t error =
            esp_lcd_panel_mirror(panel_, normalized.flip_180, normalized.flip_180);
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "Cannot flip SSD1306: %s", esp_err_to_name(error));
            return false;
        }
        flip_180_ = normalized.flip_180;
    }
    if (io_ != nullptr && normalized.contrast != contrast_) {
        std::lock_guard<std::mutex> bus_lock(*bus_mutex_);
        const esp_err_t error =
            esp_lcd_panel_io_tx_param(io_, kSsd1306SetContrast, &normalized.contrast, 1);
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "Cannot set SSD1306 contrast: %s", esp_err_to_name(error));
            return false;
        }
        contrast_ = normalized.contrast;
    }
    config_ = std::move(normalized);
    config_.flip_180 = flip_180_;
    config_.contrast = static_cast<uint8_t>(contrast_ >= 0 ? contrast_ : config_.contrast);
    RebuildLayoutLocked();
    current_page_ = 0;
    next_page_at_us_ = 0;
    dirty_ = true;
    return true;
}

void SecondaryOled::RebuildLayoutLocked() {
    std::array<secondary_oled_layout::WidgetSpec, secondary_oled_layout::kMaxWidgets> widgets = {};
    for (size_t index = 0; index < config_.widgets.size(); ++index) {
        uint8_t capabilities = kLargeCapabilities;
        if (config_.widgets[index].size == WidgetSize::kSmall) {
            capabilities = kSmallCapabilities;
        } else if (config_.widgets[index].size == WidgetSize::kMedium) {
            capabilities = kMediumCapabilities;
        }
        widgets[index] = {
            static_cast<uint8_t>(config_.widgets[index].type),
            config_.widgets[index].size,
            config_.widgets[index].enabled,
            capabilities,
        };
    }
    layout_ = BuildLayout(widgets);
    if (current_page_ >= layout_.page_count) {
        current_page_ = 0;
    }
}

SecondaryOled::Config SecondaryOled::GetConfig() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

uint8_t SecondaryOled::GetPageCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return layout_.page_count;
}

bool SecondaryOled::Flush() {
    if (panel_ == nullptr || bus_mutex_ == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> bus_lock(*bus_mutex_);

    const esp_err_t error =
        esp_lcd_panel_draw_bitmap(panel_, 0, 0, width_, height_, framebuffer_.data());
    if (error == ESP_OK) {
        consecutive_flush_failures_ = 0;
        return true;
    }

    ++consecutive_flush_failures_;
    if (consecutive_flush_failures_ == 1) {
        ESP_LOGW(TAG, "OLED frame transfer failed: %s", esp_err_to_name(error));
    }
    if (consecutive_flush_failures_ < 3) {
        return false;
    }

    // The OLED shares this controller with INA219 and MPU6050. Resetting the whole bus from an
    // optional display invalidates assumptions made by the other live device handles and can turn
    // a flaky or damaged OLED into a reboot loop. Quarantine only the OLED until the next reboot.
    ESP_LOGE(TAG, "Disabling SSD1306 after repeated transfer failures; shared I2C bus left intact");
    panel_ = nullptr;
    return false;
}

void SecondaryOled::UpdateTelemetry(const Telemetry& telemetry) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool is_gesture = telemetry.motion_valid && telemetry.motion_state != "steady" &&
                            telemetry.motion_state != "calibrating";
    const std::string observed_gesture = is_gesture ? telemetry.motion_state : std::string();
    bool pulse_triggered = false;
    if (observed_gesture != observed_gesture_) {
        observed_gesture_ = observed_gesture;
        if (!observed_gesture.empty()) {
            gesture_event_text_ = observed_gesture;
            gesture_event_until_us_ = esp_timer_get_time() + kGestureEventDurationUs;
            pulse_triggered = true;
        }
    }
    if (telemetry.low_battery && !previous_low_battery_) {
        low_battery_percent_ = telemetry.battery_percent;
        low_battery_voltage_mv_ = telemetry.battery_voltage_mv;
        low_battery_event_until_us_ = esp_timer_get_time() + kLowBatteryEventDurationUs;
        pulse_triggered = true;
    }
    previous_low_battery_ = telemetry.low_battery;
    const bool changed = telemetry.distance_valid != telemetry_.distance_valid ||
                         telemetry.distance_mm != telemetry_.distance_mm ||
                         telemetry.power_valid != telemetry_.power_valid ||
                         telemetry.current_ma != telemetry_.current_ma ||
                         telemetry.power_mw != telemetry_.power_mw ||
                         telemetry.motion_valid != telemetry_.motion_valid ||
                         telemetry.motion_state != telemetry_.motion_state ||
                         telemetry.roll_deg != telemetry_.roll_deg ||
                         telemetry.pitch_deg != telemetry_.pitch_deg ||
                         telemetry.capacity_active != telemetry_.capacity_active ||
                         telemetry.capacity_measuring != telemetry_.capacity_measuring ||
                         telemetry.capacity_uah != telemetry_.capacity_uah ||
                         telemetry.capacity_seconds != telemetry_.capacity_seconds ||
                         telemetry.cliff_detected != telemetry_.cliff_detected ||
                         telemetry.network_state != telemetry_.network_state ||
                         telemetry.gyro_turn_pending != telemetry_.gyro_turn_pending ||
                         telemetry.gyro_turn_active != telemetry_.gyro_turn_active ||
                         telemetry.gyro_turn_target_deg != telemetry_.gyro_turn_target_deg ||
                         telemetry.gyro_turn_progress_deg != telemetry_.gyro_turn_progress_deg ||
                         telemetry.gyro_turn_intensity_percent !=
                             telemetry_.gyro_turn_intensity_percent ||
                         telemetry.motion_calibrating != telemetry_.motion_calibrating ||
                         telemetry.low_battery != telemetry_.low_battery ||
                         telemetry.battery_percent != telemetry_.battery_percent ||
                         telemetry.battery_voltage_mv != telemetry_.battery_voltage_mv ||
                         telemetry.temperature_valid != telemetry_.temperature_valid ||
                         telemetry.temperature_tenths_c != telemetry_.temperature_tenths_c ||
                         telemetry.humidity_valid != telemetry_.humidity_valid ||
                         telemetry.humidity_tenths_percent != telemetry_.humidity_tenths_percent ||
                         telemetry.pressure_valid != telemetry_.pressure_valid ||
                         telemetry.pressure_tenths_hpa != telemetry_.pressure_tenths_hpa ||
                         telemetry.illuminance_valid != telemetry_.illuminance_valid ||
                         telemetry.illuminance_lux != telemetry_.illuminance_lux ||
                         telemetry.light_level != telemetry_.light_level;
    if (changed || pulse_triggered) {
        telemetry_ = telemetry;
        dirty_ = true;
    }
}

SecondaryOled::EventType SecondaryOled::ActiveEventLocked(int64_t now_us) const {
    // This order is the event priority contract: safety, system warnings, active motion,
    // informational events, then the base dashboard.
    if (telemetry_.cliff_detected) {
        return EventType::kCliff;
    }
    if (telemetry_.network_state != NetworkState::kConnected) {
        return EventType::kNetwork;
    }
    if (now_us < low_battery_event_until_us_) {
        return EventType::kLowBattery;
    }
    if (telemetry_.gyro_turn_pending || telemetry_.gyro_turn_active) {
        return EventType::kGyroTurn;
    }
    if (telemetry_.motion_calibrating) {
        return EventType::kCalibration;
    }
    if (!temporary_text_.empty()) {
        return EventType::kTemporaryText;
    }
    if (now_us < gesture_event_until_us_) {
        return EventType::kGesture;
    }
    return EventType::kNone;
}

void SecondaryOled::Tick() {
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t now_us = esp_timer_get_time();
    const EventType resolved_event = ActiveEventLocked(now_us);
    if (resolved_event != active_event_) {
        active_event_ = resolved_event;
        dirty_ = true;
    }
    if (active_event_ == EventType::kNone && layout_.page_count > 1) {
        if (next_page_at_us_ == 0) {
            next_page_at_us_ = now_us + 5000000LL;
        } else if (now_us >= next_page_at_us_) {
            current_page_ = static_cast<uint8_t>((current_page_ + 1) % layout_.page_count);
            next_page_at_us_ = now_us + 5000000LL;
            dirty_ = true;
        }
    } else {
        next_page_at_us_ = 0;
    }
    if (dirty_) {
        RenderLocked();
    }
}

void SecondaryOled::ShowTemporaryText(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (temporary_text_ != text) {
        temporary_text_ = text;
        dirty_ = true;
    }
}

void SecondaryOled::ClearTemporaryText() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!temporary_text_.empty()) {
        temporary_text_.clear();
        next_page_at_us_ = 0;
        dirty_ = true;
    }
}

void SecondaryOled::RenderDashboardLocked() {
    Clear();
    if (layout_.page_count == 0) {
        return;
    }

    bool has_second_row = false;
    for (size_t index = 0; index < layout_.placement_count; ++index) {
        const auto& placement = layout_.placements[index];
        if (placement.page == current_page_ && placement.y == 16) {
            has_second_row = true;
            break;
        }
    }
    if (has_second_row) {
        DrawHorizontalLine(0, 15, width_);
    }

    for (size_t index = 0; index < layout_.placement_count; ++index) {
        const auto& placement = layout_.placements[index];
        if (placement.page != current_page_) {
            continue;
        }
        if (placement.x > 0) {
            DrawVerticalLine(placement.x, placement.y, placement.height);
        }
        RenderWidgetLocked(placement);
    }
}

void SecondaryOled::RenderTemporaryTextLocked() {
    Clear();
    if (MeasureTextWidth(temporary_text_, FontSize::kRegular) <= width_ - 4) {
        DrawTextFitted(2, 0, width_ - 4, height_, temporary_text_, FontSize::kEmphasis);
        return;
    }
    const auto lines = SplitForTwoLines(temporary_text_);
    DrawTwoLinesFitted(2, 0, width_ - 4, height_, lines.first, lines.second,
                       FontSize::kRegular);
}

void SecondaryOled::RenderEventLocked(EventType event) {
    Clear();
    char detail[40] = {};
    switch (event) {
        case EventType::kCliff:
            if (telemetry_.distance_valid) {
                std::snprintf(detail, sizeof(detail), "%dmm STOP",
                              std::clamp(telemetry_.distance_mm, 0, 9999));
            } else {
                std::snprintf(detail, sizeof(detail), "STOP");
            }
            DrawEventMessageFitted(kWarningIcon, "CLIFF!", detail);
            break;
        case EventType::kLowBattery: {
            const int voltage_mv = std::clamp(low_battery_voltage_mv_, 0, 9999);
            std::snprintf(detail, sizeof(detail), "%d%% %d.%02dV",
                          std::clamp(low_battery_percent_, 0, 100), voltage_mv / 1000,
                          (voltage_mv % 1000) / 10);
            DrawEventMessageFitted(kCapacityIcon, "LOW BATTERY", detail);
            break;
        }
        case EventType::kNetwork: {
            const char* title = "WiFi Lost";
            const char* status = "Reconnecting...";
            if (telemetry_.network_state == NetworkState::kScanning) {
                title = "WiFi Scan";
                status = "Searching...";
            } else if (telemetry_.network_state == NetworkState::kConnecting) {
                title = "WiFi Connecting";
            } else if (telemetry_.network_state == NetworkState::kConfigMode) {
                title = "WiFi Setup";
                status = "Config mode";
            }
            DrawEventMessageFitted(kWifiIcon, title, status);
            break;
        }
        case EventType::kGyroTurn: {
            const int target = std::clamp(telemetry_.gyro_turn_target_deg, -180, 180);
            const int progress = std::clamp(telemetry_.gyro_turn_progress_deg, -180, 180);
            const char* title = target < 0 ? "TURN LEFT" : "TURN RIGHT";
            if (telemetry_.gyro_turn_pending && !telemetry_.gyro_turn_active) {
                title = "TURN STARTING";
            }
            std::snprintf(detail, sizeof(detail), "%d/%ddeg PWM %d", std::abs(progress),
                          std::abs(target),
                          std::clamp(telemetry_.gyro_turn_intensity_percent, 0, 100));
            DrawIconTextFitted(2, 0, width_ - 4, 13, kTurnIcon, title, FontSize::kRegular, true,
                               16);
            DrawTextFitted(2, 13, width_ - 4, 10, detail, FontSize::kRegular);
            const int target_magnitude = std::max(1, std::abs(target));
            const int progress_width =
                std::clamp(std::abs(progress) * (width_ - 10) / target_magnitude, 0, width_ - 10);
            DrawHorizontalLine(4, 26, width_ - 8);
            DrawHorizontalLine(4, 30, width_ - 8);
            for (int y = 27; y < 30; ++y) {
                for (int x = 5; x < 5 + progress_width; ++x) {
                    SetPixel(x, y);
                }
            }
            break;
        }
        case EventType::kCalibration:
            DrawEventMessageFitted(kMotionIcon, "Calibrating MPU", "Keep robot still");
            break;
        case EventType::kTemporaryText:
            RenderTemporaryTextLocked();
            break;
        case EventType::kGesture:
            DrawEventMessageFitted(kMotionIcon, "Gesture", gesture_event_text_);
            break;
        case EventType::kNone:
            RenderDashboardLocked();
            break;
    }
}

void SecondaryOled::RenderLocked() {
    if (panel_ == nullptr) {
        return;
    }
    if (active_event_ == EventType::kNone) {
        RenderDashboardLocked();
    } else {
        RenderEventLocked(active_event_);
    }
    dirty_ = !Flush();
}
