#pragma once

#include "aht20_sensor.h"
#include "bh1750_sensor.h"
#include "bmp280_sensor.h"
#include "environment_types.h"
#include "environment_derived.h"

#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <mutex>

class EnvironmentController {
public:
    using LightCallback = void (*)(void* context, bool valid, float illuminance_lux,
                                   int64_t timestamp_us);

    bool Start(i2c_master_bus_handle_t bus, std::mutex& bus_mutex,
               void* light_context = nullptr,
               LightCallback light_callback = nullptr);
    void Stop();
    EnvironmentStatus GetStatus() const;

private:
    enum class Sensor : uint8_t { kAht20, kBmp280, kBh1750 };
    enum class Aht20Phase : uint8_t { kIdle, kWaitingForMeasurement };

    static void TaskEntry(void* arg);
    void RunTask();

    void InitializeAht20(int64_t now_us, bool reprobe);
    void InitializeBmp280(int64_t now_us, bool reprobe);
    void InitializeBh1750(int64_t now_us, bool reprobe);
    void ServiceAht20(int64_t now_us);
    void ServiceBmp280(int64_t now_us);
    void ServiceBh1750(int64_t now_us);

    void SetInitializing(Sensor sensor);
    void SetInitializationResult(Sensor sensor, bool ready, int64_t now_us, bool reprobe);
    bool RecordFailure(Sensor sensor, int64_t now_us);
    void PublishAht20(const Aht20Sensor::Reading& reading, int64_t now_us);
    void PublishBmp280(const Bmp280Sensor::Reading& reading, int64_t now_us);
    void PublishBh1750(float lux, int64_t now_us);

    SensorHealth& HealthFor(Sensor sensor);
    void SetAvailable(Sensor sensor, bool available);
    void SetSampleValid(Sensor sensor, bool valid);
    static const char* SensorName(Sensor sensor);

    i2c_master_bus_handle_t bus_ = nullptr;
    std::mutex* bus_mutex_ = nullptr;
    Aht20Sensor aht20_;
    Bmp280Sensor bmp280_;
    Bh1750Sensor bh1750_;
    EnvironmentDerived derived_;

    mutable std::mutex status_mutex_;
    EnvironmentStatus status_;
    std::atomic_bool running_{false};
    std::atomic<TaskHandle_t> task_{nullptr};

    Aht20Phase aht20_phase_ = Aht20Phase::kIdle;
    int64_t aht20_ready_us_ = 0;
    int64_t next_aht20_sample_us_ = 0;
    int64_t next_bmp280_sample_us_ = 0;
    int64_t next_bh1750_sample_us_ = 0;
    void* light_context_ = nullptr;
    LightCallback light_callback_ = nullptr;
};
