#pragma once

#include <driver/i2c_master.h>

#include <cstddef>
#include <cstdint>

class Bmp280Sensor {
public:
    struct Reading {
        float temperature_c = 0.0f;
        float pressure_hpa = 0.0f;
    };

    ~Bmp280Sensor();

    bool Initialize(i2c_master_bus_handle_t bus);
    bool Read(Reading& reading);
    void Shutdown();

    bool IsAvailable() const { return device_ != nullptr; }
    uint8_t address() const { return address_; }

private:
    struct Calibration {
        uint16_t t1 = 0;
        int16_t t2 = 0;
        int16_t t3 = 0;
        uint16_t p1 = 0;
        int16_t p2 = 0;
        int16_t p3 = 0;
        int16_t p4 = 0;
        int16_t p5 = 0;
        int16_t p6 = 0;
        int16_t p7 = 0;
        int16_t p8 = 0;
        int16_t p9 = 0;
    };

    bool AddDevice(i2c_master_bus_handle_t bus, uint8_t address);
    bool WriteRegister(uint8_t reg, uint8_t value);
    bool ReadRegisters(uint8_t reg, uint8_t* data, size_t length);
    bool ReadCalibration();

    i2c_master_dev_handle_t device_ = nullptr;
    uint8_t address_ = 0;
    Calibration calibration_;
};
