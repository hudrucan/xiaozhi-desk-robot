#include "secondary_display_controller.h"

#include "application.h"
#include "config/hardware_config.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

#define TAG "SecondaryDisplay"

namespace {

constexpr int kWidgetSchemaVersion = 3;
constexpr size_t kVersion2WidgetCount = 8;

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
    }
    return "branding";
}

std::string SecondaryDisplayController::NormalizeConfigText(const std::string& text,
                                                            const char* fallback) {
    std::string normalized;
    normalized.reserve(std::min<size_t>(text.size(), 20));
    bool previous_space = true;
    for (unsigned char character : text) {
        if (normalized.size() >= 20) {
            break;
        }
        if (std::isalnum(character) || character == '-') {
            normalized.push_back(static_cast<char>(character));
            previous_space = false;
        } else if (std::isspace(character) && !previous_space) {
            normalized.push_back(' ');
            previous_space = true;
        }
    }
    while (!normalized.empty() && normalized.back() == ' ') {
        normalized.pop_back();
    }
    return normalized.empty() ? fallback : normalized;
}

bool SecondaryDisplayController::LoadWidgets(Settings& settings, SecondaryOled::Config& config) {
    const int version = settings.GetInt("oled_w_ver", 0);
    if (version != 2 && version != kWidgetSchemaVersion) {
        return false;
    }
    const size_t stored_count =
        version == 2 ? kVersion2WidgetCount : config.widgets.size();
    auto widgets = config.widgets;
    std::array<bool, secondary_oled_layout::kMaxWidgets> seen = {};
    for (size_t index = 0; index < stored_count; ++index) {
        const int type = settings.GetInt(WidgetKey(index, "type"), -1);
        const int type_limit = version == 2
                                   ? static_cast<int>(kVersion2WidgetCount)
                                   : static_cast<int>(secondary_oled_layout::kMaxWidgets);
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
    config.brand = settings.GetString("oled_brand", "Desk Robot");
    config.distance_prefix = settings.GetString("oled_prefix", "Dist");
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

    if (xTaskCreate(TaskEntry, "status_oled", 8192, this, 2, &task_) != pdPASS) {
        task_ = nullptr;
        ESP_LOGE(TAG, "Failed to create secondary OLED task");
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
    config.brand = NormalizeConfigText(config.brand, "Desk Robot");
    config.distance_prefix = NormalizeConfigText(config.distance_prefix, "Dist");
    if (config.distance_prefix.size() > 10) {
        config.distance_prefix.resize(10);
    }
    Application::GetInstance().Schedule([this, config = std::move(config)]() {
        if (oled_.Configure(config)) {
            PersistConfig(config);
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
