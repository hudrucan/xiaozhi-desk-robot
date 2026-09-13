#include "secondary_oled.h"

#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <utility>

#define TAG "SecondaryOled"

namespace {

// Compact 5x7 uppercase font. Columns are stored least-significant bit at the top.
constexpr uint8_t kBlank[5] = {};
constexpr uint8_t kHyphen[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
constexpr uint8_t kPeriod[5] = {0x00, 0x60, 0x60, 0x00, 0x00};
constexpr uint8_t kPercent[5] = {0x63, 0x13, 0x08, 0x64, 0x63};
constexpr uint8_t kDigits[][5] = {
    {0x3e, 0x51, 0x49, 0x45, 0x3e}, {0x00, 0x42, 0x7f, 0x40, 0x00}, {0x42, 0x61, 0x51, 0x49, 0x46},
    {0x21, 0x41, 0x45, 0x4b, 0x31}, {0x18, 0x14, 0x12, 0x7f, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3c, 0x4a, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03}, {0x36, 0x49, 0x49, 0x49, 0x36},
    {0x06, 0x49, 0x49, 0x29, 0x1e},
};
constexpr uint8_t kLetters[][5] = {
    {0x7e, 0x11, 0x11, 0x11, 0x7e}, {0x7f, 0x49, 0x49, 0x49, 0x36}, {0x3e, 0x41, 0x41, 0x41, 0x22},
    {0x7f, 0x41, 0x41, 0x22, 0x1c}, {0x7f, 0x49, 0x49, 0x49, 0x41}, {0x7f, 0x09, 0x09, 0x09, 0x01},
    {0x3e, 0x41, 0x49, 0x49, 0x7a}, {0x7f, 0x08, 0x08, 0x08, 0x7f}, {0x00, 0x41, 0x7f, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3f, 0x01}, {0x7f, 0x08, 0x14, 0x22, 0x41}, {0x7f, 0x40, 0x40, 0x40, 0x40},
    {0x7f, 0x02, 0x0c, 0x02, 0x7f}, {0x7f, 0x04, 0x08, 0x10, 0x7f}, {0x3e, 0x41, 0x41, 0x41, 0x3e},
    {0x7f, 0x09, 0x09, 0x09, 0x06}, {0x3e, 0x41, 0x51, 0x21, 0x5e}, {0x7f, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7f, 0x01, 0x01}, {0x3f, 0x40, 0x40, 0x40, 0x3f},
    {0x1f, 0x20, 0x40, 0x20, 0x1f}, {0x3f, 0x40, 0x38, 0x40, 0x3f}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43},
};
constexpr uint8_t kLowercase[][5] = {
    {0x20, 0x54, 0x54, 0x54, 0x78}, {0x7f, 0x48, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x20},
    {0x38, 0x44, 0x44, 0x48, 0x7f}, {0x38, 0x54, 0x54, 0x54, 0x18}, {0x08, 0x7e, 0x09, 0x01, 0x02},
    {0x0c, 0x52, 0x52, 0x52, 0x3e}, {0x7f, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7d, 0x40, 0x00},
    {0x20, 0x40, 0x44, 0x3d, 0x00}, {0x7f, 0x10, 0x28, 0x44, 0x00}, {0x00, 0x41, 0x7f, 0x40, 0x00},
    {0x7c, 0x04, 0x18, 0x04, 0x78}, {0x7c, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38},
    {0x7c, 0x14, 0x14, 0x14, 0x08}, {0x08, 0x14, 0x14, 0x18, 0x7c}, {0x7c, 0x08, 0x04, 0x04, 0x08},
    {0x48, 0x54, 0x54, 0x54, 0x20}, {0x04, 0x3f, 0x44, 0x40, 0x20}, {0x3c, 0x40, 0x40, 0x20, 0x7c},
    {0x1c, 0x20, 0x40, 0x20, 0x1c}, {0x3c, 0x40, 0x30, 0x40, 0x3c}, {0x44, 0x28, 0x10, 0x28, 0x44},
    {0x0c, 0x50, 0x50, 0x50, 0x3c}, {0x44, 0x64, 0x54, 0x4c, 0x44},
};

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
    ShowStatus("STARTING", 0, false);
    ESP_LOGI(TAG, "SSD1306 status display ready on shared I2C bus");
    return true;
}

const uint8_t* SecondaryOled::Glyph(char character) {
    if (character == '-') {
        return kHyphen;
    }
    if (character == '.') {
        return kPeriod;
    }
    if (character == '%') {
        return kPercent;
    }
    if (character >= '0' && character <= '9') {
        return kDigits[character - '0'];
    }
    if (character >= 'a' && character <= 'z') {
        return kLowercase[character - 'a'];
    }
    if (character >= 'A' && character <= 'Z') {
        return kLetters[character - 'A'];
    }
    return kBlank;
}

void SecondaryOled::Clear() { framebuffer_.fill(0); }

void SecondaryOled::SetPixel(int x, int y) {
    if (x >= 0 && x < width_ && y >= 0 && y < height_) {
        framebuffer_[x + (y / 8) * width_] |= 1U << (y & 7);
    }
}

void SecondaryOled::DrawTextScaled(int x, int y, const std::string& text, int scale) {
    for (char character : text) {
        const uint8_t* glyph = Glyph(character);
        for (int column = 0; column < 5; ++column) {
            for (int row = 0; row < 7; ++row) {
                if ((glyph[column] & (1U << row)) != 0) {
                    for (int dx = 0; dx < scale; ++dx) {
                        for (int dy = 0; dy < scale; ++dy) {
                            SetPixel(x + column * scale + dx, y + row * scale + dy);
                        }
                    }
                }
            }
        }
        x += 6 * scale;
        if (x >= width_) {
            break;
        }
    }
}

bool SecondaryOled::Configure(const Config& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    Config normalized = config;
    normalized.text_scale = std::clamp(normalized.text_scale, 1, 3);
    if (normalized.brand.empty()) {
        normalized.brand = "Xiaozhi";
    }
    if (normalized.distance_prefix.empty()) {
        normalized.distance_prefix = "Dist";
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
    config_ = std::move(normalized);
    config_.flip_180 = flip_180_;
    scroll_x_ = 0;
    RebuildMessageLocked();
    RenderLocked();
    return true;
}

SecondaryOled::Config SecondaryOled::GetConfig() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
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

void SecondaryOled::ShowStatus(const std::string& state, int distance_mm, bool distance_valid,
                               int battery_percent, float battery_voltage_v,
                               float battery_current_ma) {
    if (panel_ == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::string normalized_state = state;
    std::replace(normalized_state.begin(), normalized_state.end(), '_', ' ');
    bool capitalize_next = true;
    for (char& character : normalized_state) {
        if (character == ' ') {
            capitalize_next = true;
        } else if (capitalize_next) {
            character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
            capitalize_next = false;
        } else {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
    }
    state_ = std::move(normalized_state);
    distance_mm_ = distance_mm;
    distance_valid_ = distance_valid;
    battery_percent_ = battery_percent;
    battery_voltage_v_ = battery_voltage_v;
    battery_current_ma_ = battery_current_ma;
    RebuildMessageLocked();
    RenderLocked();
}

void SecondaryOled::Tick() {
    std::lock_guard<std::mutex> lock(mutex_);
    RenderLocked();
}

void SecondaryOled::ShowTemporaryText(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    temporary_text_ = text;
    scroll_x_ = 0;
    RebuildMessageLocked();
    RenderLocked();
}

void SecondaryOled::ClearTemporaryText() {
    std::lock_guard<std::mutex> lock(mutex_);
    temporary_text_.clear();
    scroll_x_ = 0;
    RebuildMessageLocked();
    RenderLocked();
}

void SecondaryOled::RebuildMessageLocked() {
    if (!temporary_text_.empty()) {
        if (message_ != temporary_text_) {
            message_ = temporary_text_;
            scroll_x_ = 0;
        }
        return;
    }
    std::string message;
    const auto append = [&message](const std::string& segment) {
        if (segment.empty()) {
            return;
        }
        if (!message.empty()) {
            message += " - ";
        }
        message += segment;
    };
    if (config_.show_brand) {
        append(config_.brand);
    }
    if (config_.show_state) {
        append(state_);
    }
    if (config_.show_distance) {
        std::string distance = config_.distance_prefix + " ";
        if (distance_valid_) {
            distance += std::to_string(std::clamp(distance_mm_, 0, 9999)) + " mm";
        } else {
            distance += "0";
        }
        append(distance);
    }
    if (config_.show_battery && battery_percent_ >= 0) {
        char battery[32] = {};
        std::snprintf(battery, sizeof(battery), "Bat %d%%", std::clamp(battery_percent_, 0, 100));
        append(battery);
    }
    if (config_.show_voltage && battery_percent_ >= 0) {
        char voltage[24] = {};
        const int centivolts =
            std::max(0, static_cast<int>(std::lround(battery_voltage_v_ * 100.0f)));
        std::snprintf(voltage, sizeof(voltage), "Vol %d.%02dV", centivolts / 100, centivolts % 100);
        append(voltage);
    }
    if (config_.show_current && battery_percent_ >= 0) {
        char current[24] = {};
        const int milliamps = static_cast<int>(std::lround(battery_current_ma_));
        std::snprintf(current, sizeof(current), "Amp %dmA", milliamps);
        append(current);
    }
    if (message != message_) {
        message_ = std::move(message);
        if (message_.empty()) {
            scroll_x_ = 0;
        }
    }
}

void SecondaryOled::RenderLocked() {
    if (panel_ == nullptr) {
        return;
    }
    if (message_.empty()) {
        Clear();
        Flush();
        return;
    }

    const int scale = config_.text_scale;
    constexpr int kScrollStep = 1;
    const int text_width = static_cast<int>(message_.size()) * 6 * scale;
    // The upper third of this physical OLED is damaged. Center the smaller text inside
    // the remaining lower two-thirds and never light a pixel in the damaged rows.
    const int safe_top = (height_ + 2) / 3;
    const int safe_height = height_ - safe_top;
    const int text_y = safe_top + (safe_height - 7 * scale) / 2;
    Clear();
    if (text_width <= width_) {
        DrawTextScaled((width_ - text_width) / 2, text_y, message_, scale);
        scroll_x_ = 0;
        Flush();
        return;
    }

    const std::string scrolling_text = message_ + " - ";
    const int scrolling_width = static_cast<int>(scrolling_text.size()) * 6 * scale;
    if (scroll_x_ > 0 || scroll_x_ <= -scrolling_width) {
        scroll_x_ = 0;
    }
    DrawTextScaled(scroll_x_, text_y, scrolling_text, scale);
    DrawTextScaled(scroll_x_ + scrolling_width, text_y, scrolling_text, scale);
    Flush();
    scroll_x_ -= kScrollStep;
    if (scroll_x_ <= -scrolling_width) {
        scroll_x_ += scrolling_width;
    }
}
