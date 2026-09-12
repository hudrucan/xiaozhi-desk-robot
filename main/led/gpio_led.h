#ifndef _GPIO_LED_H_
#define _GPIO_LED_H_

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <mutex>
#include "device_state.h"
#include "led.h"

class GpioLed : public Led {
public:
    enum class StatusProfile { kDefault, kEdison };
    enum class EffectOverride { kNone, kSteady, kBreathe, kBlink, kOff };

    GpioLed(gpio_num_t gpio);
    GpioLed(gpio_num_t gpio, int output_invert);
    GpioLed(gpio_num_t gpio, int output_invert, ledc_timer_t timer_num, ledc_channel_t channel);
    virtual ~GpioLed();

    void OnStateChanged() override;
    void TurnOn();
    void TurnOff();
    void SetBrightness(uint8_t brightness);
    void SetBrightnessScale(uint8_t brightness_scale);
    void SetStatusProfile(StatusProfile profile);
    void SetActivityOverride(bool enabled);
    void SetEffectOverride(EffectOverride effect);

private:
    std::mutex mutex_;
    TaskHandle_t blink_task_ = nullptr;
    ledc_channel_config_t ledc_channel_ = {0};
    bool ledc_initialized_ = false;
    uint32_t duty_ = 0;
    std::atomic_uint8_t brightness_scale_{100};
    std::atomic<StatusProfile> status_profile_{StatusProfile::kDefault};
    std::atomic_bool activity_override_{false};
    std::atomic<EffectOverride> effect_override_{EffectOverride::kNone};
    std::atomic_bool fade_enabled_{false};
    int blink_counter_ = 0;
    int blink_interval_ms_ = 0;
    esp_timer_handle_t blink_timer_ = nullptr;
    bool fade_up_ = true;
    TaskHandle_t event_task_handle_;

    static void EventTask(void* arg);
    void StartBlinkTask(int times, int interval_ms);
    void OnBlinkTimer();

    void BlinkOnce();
    void Blink(int times, int interval_ms);
    void StartContinuousBlink(int interval_ms);
    void StartFadeTask();
    void OnFadeEnd();
    void ApplyEdisonStatus(DeviceState state);
    static bool IRAM_ATTR FadeCallback(const ledc_cb_param_t* param, void* user_arg);
};

#endif  // _GPIO_LED_H_
