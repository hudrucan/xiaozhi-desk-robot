#include "environment_controller.h"

#include "config/tuning.h"

#include <esp_log.h>
#include <esp_timer.h>

#define TAG "Environment"

namespace {

constexpr int64_t MillisecondsToMicroseconds(int64_t milliseconds) {
    return milliseconds * 1000;
}

}  // namespace

void EnvironmentController::TaskEntry(void* arg) {
    static_cast<EnvironmentController*>(arg)->RunTask();
}

void EnvironmentController::RunTask() {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ENVIRONMENT_START_DELAY_MS));
    if (running_.load(std::memory_order_acquire)) {
        int64_t now_us = esp_timer_get_time();
        InitializeAht20(now_us, false);
        vTaskDelay(pdMS_TO_TICKS(20));
        now_us = esp_timer_get_time();
        InitializeBmp280(now_us, false);
        vTaskDelay(pdMS_TO_TICKS(20));
        now_us = esp_timer_get_time();
        InitializeBh1750(now_us, false);
    }

    while (running_.load(std::memory_order_acquire)) {
        const int64_t now_us = esp_timer_get_time();
        ServiceAht20(now_us);
        ServiceBmp280(now_us);
        ServiceBh1750(now_us);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ENVIRONMENT_SERVICE_PERIOD_MS));
    }

    {
        std::lock_guard<std::mutex> bus_lock(*bus_mutex_);
        aht20_.Shutdown();
        bmp280_.Shutdown();
        bh1750_.Shutdown();
    }
    bus_ = nullptr;
    bus_mutex_ = nullptr;
    task_.store(nullptr, std::memory_order_release);
    vTaskDelete(nullptr);
}

void EnvironmentController::InitializeAht20(int64_t now_us, bool reprobe) {
    SetInitializing(Sensor::kAht20);
    bool ready = false;
    {
        std::lock_guard<std::mutex> bus_lock(*bus_mutex_);
        ready = aht20_.Initialize(bus_);
    }
    SetInitializationResult(Sensor::kAht20, ready, now_us, reprobe);
    aht20_phase_ = Aht20Phase::kIdle;
    next_aht20_sample_us_ = ready ? now_us : 0;
}

void EnvironmentController::InitializeBmp280(int64_t now_us, bool reprobe) {
    SetInitializing(Sensor::kBmp280);
    bool ready = false;
    {
        std::lock_guard<std::mutex> bus_lock(*bus_mutex_);
        ready = bmp280_.Initialize(bus_);
    }
    SetInitializationResult(Sensor::kBmp280, ready, now_us, reprobe);
    next_bmp280_sample_us_ = ready ? now_us + MillisecondsToMicroseconds(50) : 0;
}

void EnvironmentController::InitializeBh1750(int64_t now_us, bool reprobe) {
    SetInitializing(Sensor::kBh1750);
    bool ready = false;
    {
        std::lock_guard<std::mutex> bus_lock(*bus_mutex_);
        ready = bh1750_.Initialize(bus_);
    }
    SetInitializationResult(Sensor::kBh1750, ready, now_us, reprobe);
    next_bh1750_sample_us_ =
        ready ? now_us + MillisecondsToMicroseconds(BH1750_INITIAL_CONVERSION_TIME_MS) : 0;
}

void EnvironmentController::ServiceAht20(int64_t now_us) {
    const SensorHealth health = GetStatus().aht20_health;
    if (health.state == SensorState::kMissing) {
        if (now_us >= health.next_probe_us) {
            InitializeAht20(now_us, true);
        }
        return;
    }
    if (aht20_phase_ == Aht20Phase::kWaitingForMeasurement) {
        if (now_us < aht20_ready_us_) {
            return;
        }
        Aht20Sensor::Reading reading;
        bool success = false;
        {
            std::lock_guard<std::mutex> bus_lock(*bus_mutex_);
            success = aht20_.ReadMeasurement(reading);
        }
        if (success) {
            PublishAht20(reading, now_us);
        } else if (RecordFailure(Sensor::kAht20, now_us)) {
            std::lock_guard<std::mutex> bus_lock(*bus_mutex_);
            aht20_.Shutdown();
        }
        aht20_phase_ = Aht20Phase::kIdle;
        next_aht20_sample_us_ = now_us + MillisecondsToMicroseconds(AHT20_SAMPLE_PERIOD_MS);
        return;
    }
    if (now_us >= next_aht20_sample_us_) {
        bool triggered = false;
        {
            std::lock_guard<std::mutex> bus_lock(*bus_mutex_);
            triggered = aht20_.TriggerMeasurement();
        }
        if (triggered) {
            aht20_phase_ = Aht20Phase::kWaitingForMeasurement;
            aht20_ready_us_ = now_us + MillisecondsToMicroseconds(AHT20_CONVERSION_TIME_MS);
        } else {
            if (RecordFailure(Sensor::kAht20, now_us)) {
                std::lock_guard<std::mutex> bus_lock(*bus_mutex_);
                aht20_.Shutdown();
            }
            next_aht20_sample_us_ = now_us + MillisecondsToMicroseconds(AHT20_SAMPLE_PERIOD_MS);
        }
    }
}

void EnvironmentController::ServiceBmp280(int64_t now_us) {
    const SensorHealth health = GetStatus().bmp280_health;
    if (health.state == SensorState::kMissing) {
        if (now_us >= health.next_probe_us) {
            InitializeBmp280(now_us, true);
        }
        return;
    }
    if (now_us < next_bmp280_sample_us_) {
        return;
    }
    Bmp280Sensor::Reading reading;
    bool success = false;
    {
        std::lock_guard<std::mutex> bus_lock(*bus_mutex_);
        success = bmp280_.Read(reading);
    }
    if (success) {
        PublishBmp280(reading, now_us);
    } else if (RecordFailure(Sensor::kBmp280, now_us)) {
        std::lock_guard<std::mutex> bus_lock(*bus_mutex_);
        bmp280_.Shutdown();
    }
    next_bmp280_sample_us_ = now_us + MillisecondsToMicroseconds(BMP280_SAMPLE_PERIOD_MS);
}

void EnvironmentController::ServiceBh1750(int64_t now_us) {
    const SensorHealth health = GetStatus().bh1750_health;
    if (health.state == SensorState::kMissing) {
        if (now_us >= health.next_probe_us) {
            InitializeBh1750(now_us, true);
        }
        return;
    }
    if (now_us < next_bh1750_sample_us_) {
        return;
    }
    float lux = 0.0f;
    bool success = false;
    {
        std::lock_guard<std::mutex> bus_lock(*bus_mutex_);
        success = bh1750_.ReadLux(lux);
    }
    if (success) {
        PublishBh1750(lux, now_us);
    } else {
        if (light_callback_ != nullptr) {
            light_callback_(light_context_, false, 0.0f, now_us);
        }
        if (RecordFailure(Sensor::kBh1750, now_us)) {
            std::lock_guard<std::mutex> bus_lock(*bus_mutex_);
            bh1750_.Shutdown();
        }
    }
    next_bh1750_sample_us_ = now_us + MillisecondsToMicroseconds(BH1750_SAMPLE_PERIOD_MS);
}

void EnvironmentController::SetInitializing(Sensor sensor) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    HealthFor(sensor).state = SensorState::kInitializing;
    SetAvailable(sensor, false);
    SetSampleValid(sensor, false);
}

void EnvironmentController::SetInitializationResult(Sensor sensor, bool ready, int64_t now_us,
                                                     bool reprobe) {
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        SensorHealth& health = HealthFor(sensor);
        health.state = ready ? SensorState::kReady : SensorState::kMissing;
        health.sample_valid = false;
        health.consecutive_failures = 0;
        health.next_probe_us =
            ready ? 0 : now_us + MillisecondsToMicroseconds(ENVIRONMENT_REPROBE_PERIOD_MS);
        SetAvailable(sensor, ready);
        SetSampleValid(sensor, false);
    }
    if (ready) {
        if (reprobe) {
            ESP_LOGI(TAG, "%s recovered", SensorName(sensor));
        } else {
            ESP_LOGI(TAG, "%s ready", SensorName(sensor));
        }
    } else if (!reprobe) {
        ESP_LOGI(TAG, "%s not detected; background reprobe enabled", SensorName(sensor));
    }
}

bool EnvironmentController::RecordFailure(Sensor sensor, int64_t now_us) {
    bool first_failure = false;
    bool became_missing = false;
    uint32_t failure_count = 0;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        SensorHealth& health = HealthFor(sensor);
        health.sample_valid = false;
        health.last_failure_us = now_us;
        health.total_failures++;
        health.consecutive_failures++;
        failure_count = health.consecutive_failures;
        first_failure = health.consecutive_failures == 1;
        if (health.consecutive_failures >= ENVIRONMENT_FAILURE_THRESHOLD) {
            health.state = SensorState::kMissing;
            health.next_probe_us =
                now_us + MillisecondsToMicroseconds(ENVIRONMENT_REPROBE_PERIOD_MS);
            SetAvailable(sensor, false);
            if (sensor == Sensor::kBmp280) {
                derived_.ResetPressure();
            }
            became_missing = true;
        } else {
            health.state = SensorState::kDegraded;
        }
        SetSampleValid(sensor, false);
    }
    if (first_failure) {
        ESP_LOGW(TAG, "%s read failed; sensor degraded", SensorName(sensor));
    }
    if (became_missing) {
        ESP_LOGW(TAG, "%s disabled after %u consecutive failures", SensorName(sensor),
                 failure_count);
    }
    return became_missing;
}

void EnvironmentController::PublishAht20(const Aht20Sensor::Reading& reading, int64_t now_us) {
    bool recovered = false;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.temperature_c = reading.temperature_c;
        status_.humidity_percent = reading.humidity_percent;
        status_.temperature_valid = true;
        status_.humidity_valid = true;
        status_.temperature_last_good_us = now_us;
        status_.humidity_last_good_us = now_us;
        SensorHealth& health = HealthFor(Sensor::kAht20);
        recovered = health.state == SensorState::kDegraded;
        health.state = SensorState::kReady;
        health.sample_valid = true;
        health.consecutive_failures = 0;
        health.last_good_us = now_us;
        derived_.UpdateClimate(status_);
    }
    if (recovered) {
        ESP_LOGI(TAG, "AHT20 read recovered");
    }
}

void EnvironmentController::PublishBmp280(const Bmp280Sensor::Reading& reading, int64_t now_us) {
    bool recovered = false;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.pressure_hpa = reading.pressure_hpa;
        status_.pressure_valid = true;
        status_.pressure_last_good_us = now_us;
        SensorHealth& health = HealthFor(Sensor::kBmp280);
        recovered = health.state == SensorState::kDegraded;
        health.state = SensorState::kReady;
        health.sample_valid = true;
        health.consecutive_failures = 0;
        health.last_good_us = now_us;
        derived_.UpdatePressure(status_, now_us);
    }
    if (recovered) {
        ESP_LOGI(TAG, "BMP280 read recovered");
    }
}

void EnvironmentController::PublishBh1750(float lux, int64_t now_us) {
    bool recovered = false;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.illuminance_lux = lux;
        status_.illuminance_valid = true;
        status_.illuminance_last_good_us = now_us;
        SensorHealth& health = HealthFor(Sensor::kBh1750);
        recovered = health.state == SensorState::kDegraded;
        health.state = SensorState::kReady;
        health.sample_valid = true;
        health.consecutive_failures = 0;
        health.last_good_us = now_us;
        derived_.UpdateLight(status_);
    }
    if (recovered) {
        ESP_LOGI(TAG, "BH1750 read recovered");
    }
    if (light_callback_ != nullptr) {
        light_callback_(light_context_, true, lux, now_us);
    }
}

SensorHealth& EnvironmentController::HealthFor(Sensor sensor) {
    switch (sensor) {
        case Sensor::kAht20:
            return status_.aht20_health;
        case Sensor::kBmp280:
            return status_.bmp280_health;
        case Sensor::kBh1750:
            return status_.bh1750_health;
    }
    return status_.aht20_health;
}

void EnvironmentController::SetAvailable(Sensor sensor, bool available) {
    switch (sensor) {
        case Sensor::kAht20:
            status_.aht20_available = available;
            break;
        case Sensor::kBmp280:
            status_.bmp280_available = available;
            break;
        case Sensor::kBh1750:
            status_.bh1750_available = available;
            break;
    }
}

void EnvironmentController::SetSampleValid(Sensor sensor, bool valid) {
    switch (sensor) {
        case Sensor::kAht20:
            status_.temperature_valid = valid;
            status_.humidity_valid = valid;
            if (!valid) {
                status_.comfort_level = ComfortLevel::kUnavailable;
            }
            break;
        case Sensor::kBmp280:
            status_.pressure_valid = valid;
            if (!valid) {
                status_.pressure_trend = PressureTrend::kUnavailable;
            }
            break;
        case Sensor::kBh1750:
            status_.illuminance_valid = valid;
            if (!valid) {
                status_.light_level = LightLevel::kUnavailable;
            }
            break;
    }
}

const char* EnvironmentController::SensorName(Sensor sensor) {
    switch (sensor) {
        case Sensor::kAht20:
            return "AHT20";
        case Sensor::kBmp280:
            return "BMP280";
        case Sensor::kBh1750:
            return "BH1750";
    }
    return "environment sensor";
}
