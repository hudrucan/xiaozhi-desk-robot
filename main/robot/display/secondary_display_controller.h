#pragma once

#include "secondary_oled.h"

#include <driver/i2c_master.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>

class Settings;

class SecondaryDisplayController {
public:
    using TelemetryProvider = void (*)(void* context, SecondaryOled::Telemetry& telemetry);

    bool Initialize(i2c_master_bus_handle_t bus, std::mutex& bus_mutex, void* telemetry_context,
                    TelemetryProvider telemetry_provider);
    bool IsAvailable() const { return oled_.IsAvailable(); }
    SecondaryOled::Config GetConfig() const { return oled_.GetConfig(); }
    uint8_t GetPageCount() const { return oled_.GetPageCount(); }

    void SetNetworkState(SecondaryOled::NetworkState state) {
        network_state_.store(state, std::memory_order_relaxed);
    }
    void QueueConfig(SecondaryOled::Config config);
    bool QueueTemporaryText(const std::string& text, int duration_ms);

    static int WidgetActionIndex(const std::string& action, const std::string& prefix);
    static const char* WidgetTypeName(SecondaryOled::WidgetType type);
    static std::string NormalizeConfigText(const std::string& text, const char* fallback);

private:
    static std::string WidgetKey(size_t index, const char* field);
    static bool LoadWidgets(Settings& settings, SecondaryOled::Config& config);
    static void TaskEntry(void* arg);
    static void TemporaryTextResetTimer(void* arg);
    void RunTask();
    void PersistConfig(const SecondaryOled::Config& config);
    void ClearTemporaryText();

    SecondaryOled oled_;
    std::atomic<SecondaryOled::NetworkState> network_state_{
        SecondaryOled::NetworkState::kConnecting};
    TaskHandle_t task_ = nullptr;
    esp_timer_handle_t temporary_text_reset_timer_ = nullptr;
    void* telemetry_context_ = nullptr;
    TelemetryProvider telemetry_provider_ = nullptr;
};
