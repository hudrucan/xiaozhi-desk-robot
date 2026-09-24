#include "secondary_display_controller.h"

#include "application.h"
#include "config/hardware_config.h"
#include "config/tuning.h"
#include "settings.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#define TAG "SecondaryDisplay"

namespace {

constexpr int kWidgetSchemaVersion = 4;
constexpr size_t kVersion2WidgetCount = 8;
constexpr size_t kVersion3WidgetCount = 9;

bool DecodeUtf8Codepoint(const std::string& text, size_t& offset, uint32_t& codepoint) {
    const size_t begin = offset;
    const auto first = static_cast<uint8_t>(text[offset]);
    size_t length = 0;
    if (first < 0x80) {
        length = 1;
        codepoint = first;
    } else if ((first & 0xe0) == 0xc0) {
        length = 2;
        codepoint = first & 0x1f;
    } else if ((first & 0xf0) == 0xe0) {
        length = 3;
        codepoint = first & 0x0f;
    } else if ((first & 0xf8) == 0xf0) {
        length = 4;
        codepoint = first & 0x07;
    } else {
        ++offset;
        return false;
    }
    if (begin + length > text.size()) {
        offset = text.size();
        return false;
    }
    for (size_t index = 1; index < length; ++index) {
        const auto continuation = static_cast<uint8_t>(text[begin + index]);
        if ((continuation & 0xc0) != 0x80) {
            ++offset;
            return false;
        }
        codepoint = (codepoint << 6) | (continuation & 0x3f);
    }
    offset += length;
    if ((length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) ||
        (length == 4 && codepoint < 0x10000) ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff) {
        return false;
    }
    return true;
}

bool IsUnicodeWhitespace(uint32_t codepoint) {
    return codepoint == 0x20 || (codepoint >= 0x09 && codepoint <= 0x0d) || codepoint == 0x85 ||
           codepoint == 0xa0 || codepoint == 0x1680 ||
           (codepoint >= 0x2000 && codepoint <= 0x200a) || codepoint == 0x2028 ||
           codepoint == 0x2029 || codepoint == 0x202f || codepoint == 0x205f ||
           codepoint == 0x3000;
}

}  // namespace

std::string SecondaryDisplayController::WidgetKey(size_t index, const char* field) {
    return "ow" + std::to_string(index) + "_" + field;
}

int SecondaryDisplayController::WidgetActionIndex(const std::string& action,
                                                  const std::string& prefix) {
    if (action.size() != prefix.size() + 1 || action.compare(0, prefix.size(), prefix) != 0) {
        return -1;
    }
    const char digit = action.back();
    if (digit < '0' || static_cast<size_t>(digit - '0') >= secondary_oled_layout::kMaxWidgets) {
        return -1;
    }
    return digit - '0';
}

const char* SecondaryDisplayController::WidgetTypeName(SecondaryOled::WidgetType type) {
    switch (type) {
        case SecondaryOled::WidgetType::kBranding:
            return "branding";
        case SecondaryOled::WidgetType::kDistance:
            return "distance";
        case SecondaryOled::WidgetType::kPower:
            return "power";
        case SecondaryOled::WidgetType::kMotion:
            return "motion";
        case SecondaryOled::WidgetType::kCapacity:
            return "capacity";
        case SecondaryOled::WidgetType::kClimate:
            return "climate";
        case SecondaryOled::WidgetType::kPressure:
            return "pressure";
        case SecondaryOled::WidgetType::kLight:
            return "light";
        case SecondaryOled::WidgetType::kBatteryRemaining:
            return "battery_remaining";
        case SecondaryOled::WidgetType::kNetwork:
            return "network";
    }
    return "branding";
}

std::string SecondaryDisplayController::NormalizeUtf8Text(const std::string& text,
                                                          size_t max_codepoints,
                                                          const char* fallback) {
    std::string normalized;
    normalized.reserve(std::min(text.size(), max_codepoints * 4));
    bool previous_space = true;
    size_t codepoint_count = 0;
    for (size_t offset = 0; offset < text.size() && codepoint_count < max_codepoints;) {
        const size_t begin = offset;
        uint32_t codepoint = 0;
        if (!DecodeUtf8Codepoint(text, offset, codepoint)) {
            continue;
        }
        if (IsUnicodeWhitespace(codepoint)) {
            if (!previous_space) {
                normalized.push_back(' ');
                previous_space = true;
                ++codepoint_count;
            }
            continue;
        }
        if (codepoint < 0x20 || codepoint == 0x7f) {
            continue;
        }
        normalized.append(text, begin, offset - begin);
        previous_space = false;
        ++codepoint_count;
        if (normalized.size() >= max_codepoints * 4) {
            break;
        }
    }
    while (!normalized.empty() && normalized.back() == ' ') {
        normalized.pop_back();
    }
    return normalized.empty() ? fallback : normalized;
}

std::string SecondaryDisplayController::NormalizeConfigText(const std::string& text,
                                                            const char* fallback) {
    return NormalizeUtf8Text(text, 20, fallback);
}

bool SecondaryDisplayController::LoadWidgets(Settings& settings, SecondaryOled::Config& config) {
    const int version = settings.GetInt("oled_w_ver", 0);
    if (version != 2 && version != 3 && version != kWidgetSchemaVersion) {
        return false;
    }
    const size_t stored_count = version == 2   ? kVersion2WidgetCount
                                : version == 3 ? kVersion3WidgetCount
                                               : config.widgets.size();
    auto widgets = config.widgets;
    std::array<bool, secondary_oled_layout::kMaxWidgets> seen = {};
    for (size_t index = 0; index < stored_count; ++index) {
        const int type = settings.GetInt(WidgetKey(index, "type"), -1);
        const int type_limit = static_cast<int>(stored_count);
        if (type < 0 || type >= type_limit || seen[type]) {
            ESP_LOGW(TAG, "Ignoring invalid persisted secondary OLED widget order");
            return false;
        }
        seen[type] = true;
        widgets[index].type = static_cast<SecondaryOled::WidgetType>(type);
        widgets[index].size = static_cast<SecondaryOled::WidgetSize>(std::clamp(
            static_cast<int>(settings.GetInt(WidgetKey(index, "size"), 0)), 0, 2));
        widgets[index].enabled =
            settings.GetBool(WidgetKey(index, "on"), widgets[index].enabled);
        widgets[index].mode = static_cast<uint8_t>(
            std::clamp(static_cast<int>(settings.GetInt(WidgetKey(index, "mode"), 0)), 0, 2));
    }
    config.widgets = widgets;
    return version != kWidgetSchemaVersion;
}

bool SecondaryDisplayController::Initialize(i2c_master_bus_handle_t bus, std::mutex& bus_mutex,
                                            void* telemetry_context,
                                            TelemetryProvider telemetry_provider) {
    Settings settings("desk_robot", false);
    SecondaryOled::Config config;
    config.flip_180 = settings.GetBool("oled_flip", SECONDARY_OLED_FLIP_180);
    config.contrast = static_cast<uint8_t>(
        std::clamp(static_cast<int>(settings.GetInt("oled_contrast", 128)), 0, 255));
    config.auto_contrast_enabled = settings.GetBool("oled_ac", false);
    config.auto_contrast_minimum = static_cast<uint8_t>(std::clamp(
        static_cast<int>(settings.GetInt("oled_acmin", OLED_AUTO_CONTRAST_DEFAULT_MIN)), 0, 255));
    config.auto_contrast_maximum = static_cast<uint8_t>(std::clamp(
        static_cast<int>(settings.GetInt("oled_acmax", OLED_AUTO_CONTRAST_DEFAULT_MAX)), 0, 255));
    if (config.auto_contrast_maximum < config.auto_contrast_minimum) {
        config.auto_contrast_maximum = config.auto_contrast_minimum;
    }
    config.brand = NormalizeUtf8Text(settings.GetString("oled_brand", "Desk Robot"), 20,
                                     "Desk Robot");
    config.distance_prefix =
        NormalizeUtf8Text(settings.GetString("oled_prefix", "Dist"), 10, "Dist");
    const bool migrate_widgets = LoadWidgets(settings, config);
    if (!oled_.Initialize(bus, bus_mutex, SECONDARY_OLED_I2C_ADDRESS, SECONDARY_OLED_WIDTH,
                          SECONDARY_OLED_HEIGHT, config.flip_180)) {
        return false;
    }
    if (!oled_.Configure(config)) {
        return false;
    }
    if (migrate_widgets) {
        PersistConfig(config);
        ESP_LOGI(TAG, "Migrated secondary OLED widget layout to v%d", kWidgetSchemaVersion);
    }
    telemetry_context_ = telemetry_context;
    telemetry_provider_ = telemetry_provider;

    esp_timer_create_args_t timer_args = {
        .callback = TemporaryTextResetTimer,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "oled_text_reset",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &temporary_text_reset_timer_));

    if (xTaskCreateWithCaps(TaskEntry, "status_oled", 8192, this, 2, &task_,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        task_ = nullptr;
        ESP_LOGE(TAG, "Failed to create secondary OLED task in PSRAM");
        return false;
    }
    return true;
}

void SecondaryDisplayController::TaskEntry(void* arg) {
    static_cast<SecondaryDisplayController*>(arg)->RunTask();
}

void SecondaryDisplayController::RunTask() {
    // Let the board constructor and application singleton finish before reading runtime state.
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "Secondary OLED dashboard task started");
    TickType_t last_wake_time = xTaskGetTickCount();
    while (true) {
        SecondaryOled::Telemetry telemetry;
        telemetry.network_state = network_state_.load(std::memory_order_relaxed);
        if (telemetry_provider_ != nullptr) {
            telemetry_provider_(telemetry_context_, telemetry);
        }
        oled_.UpdateTelemetry(telemetry);
        oled_.Tick();
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(100));
    }
}

void SecondaryDisplayController::PersistConfig(const SecondaryOled::Config& config) {
    Settings settings("desk_robot", true);
    settings.SetBool("oled_flip", config.flip_180);
    settings.SetInt("oled_contrast", config.contrast);
    settings.SetBool("oled_ac", config.auto_contrast_enabled);
    settings.SetInt("oled_acmin", config.auto_contrast_minimum);
    settings.SetInt("oled_acmax", config.auto_contrast_maximum);
    settings.SetString("oled_brand", config.brand);
    settings.SetString("oled_prefix", config.distance_prefix);
    settings.SetInt("oled_w_ver", kWidgetSchemaVersion);
    for (size_t index = 0; index < config.widgets.size(); ++index) {
        const auto& widget = config.widgets[index];
        settings.SetInt(WidgetKey(index, "type"), static_cast<int>(widget.type));
        settings.SetInt(WidgetKey(index, "size"), static_cast<int>(widget.size));
        settings.SetBool(WidgetKey(index, "on"), widget.enabled);
        settings.SetInt(WidgetKey(index, "mode"), widget.mode);
    }
}

void SecondaryDisplayController::QueueConfig(SecondaryOled::Config config) {
    if (config.auto_contrast_maximum < config.auto_contrast_minimum) {
        config.auto_contrast_maximum = config.auto_contrast_minimum;
    }
    config.brand = NormalizeUtf8Text(config.brand, 20, "Desk Robot");
    config.distance_prefix = NormalizeUtf8Text(config.distance_prefix, 10, "Dist");
    Application::GetInstance().Schedule([this, config = std::move(config)]() {
        if (oled_.Configure(config)) {
            PersistConfig(config);
            std::lock_guard<std::mutex> lock(auto_contrast_mutex_);
            filtered_lux_valid_ = false;
            last_auto_contrast_ = -1;
            last_auto_contrast_update_us_ = 0;
        }
    });
}

int SecondaryDisplayController::AutoContrastTarget(float illuminance_lux) {
    if (illuminance_lux < AUTO_BRIGHTNESS_DARK_MAX_LUX) {
        return OLED_AUTO_CONTRAST_DARK;
    }
    if (illuminance_lux < AUTO_BRIGHTNESS_DIM_MAX_LUX) {
        return OLED_AUTO_CONTRAST_DIM;
    }
    if (illuminance_lux < AUTO_BRIGHTNESS_INDOOR_MAX_LUX) {
        return OLED_AUTO_CONTRAST_INDOOR;
    }
    if (illuminance_lux < AUTO_BRIGHTNESS_BRIGHT_MAX_LUX) {
        return OLED_AUTO_CONTRAST_BRIGHT;
    }
    if (illuminance_lux < AUTO_BRIGHTNESS_VERY_BRIGHT_MAX_LUX) {
        return OLED_AUTO_CONTRAST_VERY_BRIGHT;
    }
    return OLED_AUTO_CONTRAST_SUNLIT;
}

void SecondaryDisplayController::UpdateAmbientLight(bool valid, float illuminance_lux,
                                                     int64_t timestamp_us) {
    const bool sample_valid =
        valid && std::isfinite(illuminance_lux) && illuminance_lux >= 0.0f;
    ambient_light_available_.store(sample_valid, std::memory_order_relaxed);
    const SecondaryOled::Config config = oled_.GetConfig();
    if (!config.auto_contrast_enabled) {
        return;
    }

    int target = config.contrast;
    {
        std::lock_guard<std::mutex> lock(auto_contrast_mutex_);
        if (!sample_valid) {
            filtered_lux_valid_ = false;
        } else {
            if (!filtered_lux_valid_) {
                filtered_lux_ = illuminance_lux;
                filtered_lux_valid_ = true;
            } else {
                filtered_lux_ +=
                    OLED_AUTO_CONTRAST_FILTER_ALPHA * (illuminance_lux - filtered_lux_);
            }
            target = std::clamp(AutoContrastTarget(filtered_lux_),
                                static_cast<int>(config.auto_contrast_minimum),
                                static_cast<int>(config.auto_contrast_maximum));
        }

        const bool unchanged = last_auto_contrast_ == target;
        const bool within_hysteresis =
            sample_valid && last_auto_contrast_ >= 0 &&
            std::abs(target - last_auto_contrast_) < OLED_AUTO_CONTRAST_HYSTERESIS;
        if (unchanged || within_hysteresis) {
            return;
        }
        if (sample_valid && last_auto_contrast_update_us_ > 0 &&
            timestamp_us >= last_auto_contrast_update_us_ &&
            timestamp_us - last_auto_contrast_update_us_ <
                static_cast<int64_t>(OLED_AUTO_CONTRAST_MIN_UPDATE_INTERVAL_MS) * 1000) {
            return;
        }
        last_auto_contrast_ = target;
        last_auto_contrast_update_us_ = timestamp_us;
    }

    Application::GetInstance().Schedule([this, contrast = static_cast<uint8_t>(target)]() {
        if (oled_.GetConfig().auto_contrast_enabled) {
            oled_.SetRuntimeContrast(contrast);
        }
    });
}

bool SecondaryDisplayController::QueueTemporaryText(const std::string& text, int duration_ms) {
    if (text.empty() || !oled_.IsAvailable()) {
        return false;
    }
    const int safe_duration = std::clamp(duration_ms, 500, 60000);
    Application::GetInstance().Schedule([this, text]() { oled_.ShowTemporaryText(text); });
    if (temporary_text_reset_timer_ != nullptr) {
        esp_timer_stop(temporary_text_reset_timer_);
        ESP_ERROR_CHECK(
            esp_timer_start_once(temporary_text_reset_timer_, safe_duration * 1000ULL));
    }
    return true;
}

void SecondaryDisplayController::TemporaryTextResetTimer(void* arg) {
    static_cast<SecondaryDisplayController*>(arg)->ClearTemporaryText();
}

void SecondaryDisplayController::ClearTemporaryText() {
    Application::GetInstance().Schedule([this]() { oled_.ClearTemporaryText(); });
}
