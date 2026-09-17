#pragma once

#include <driver/i2c_master.h>

#include <cstddef>
#include <cstdint>

class Aht20Sensor {
public:
    struct Reading {
        float temperature_c = 0.0f;
        float humidity_percent = 0.0f;
    };

    ~Aht20Sensor();

    bool Initialize(i2c_master_bus_handle_t bus);
    bool TriggerMeasurement();
    bool ReadMeasurement(Reading& reading);
    void Shutdown();

    bool IsAvailable() const { return device_ != nullptr; }

private:
    bool ReadStatus(uint8_t& status);
    static uint8_t CalculateCrc(const uint8_t* data, size_t length);

    i2c_master_dev_handle_t device_ = nullptr;
};
