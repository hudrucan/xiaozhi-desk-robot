#pragma once

#include <driver/i2c_master.h>

#include <cstdint>

class Ina219PowerMonitor {
public:
    struct Reading {
        float bus_voltage_v = 0.0f;
        float battery_voltage_v = 0.0f;
        float shunt_voltage_mv = 0.0f;
        float current_ma = 0.0f;
        float power_mw = 0.0f;
        bool charging = false;
        bool discharging = false;
        bool conversion_ready = false;
        bool math_overflow = false;
        bool valid = false;
    };

    bool Initialize(i2c_master_bus_handle_t bus, uint8_t address, float shunt_resistance_ohms);
    bool Read(Reading& reading);
    bool IsAvailable() const { return device_ != nullptr; }

private:
    bool WriteRegister(uint8_t reg, uint16_t value);
    bool ReadRegister(uint8_t reg, uint16_t& value);

    i2c_master_dev_handle_t device_ = nullptr;
    float shunt_resistance_ohms_ = 0.1f;
};
