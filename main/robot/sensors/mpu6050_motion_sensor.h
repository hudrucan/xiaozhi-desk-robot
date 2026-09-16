#pragma once

#include <driver/i2c_master.h>

#include <cstddef>
#include <cstdint>

class Mpu6050MotionSensor {
public:
    struct Sample {
        float acceleration_x_g = 0.0f;
        float acceleration_y_g = 0.0f;
        float acceleration_z_g = 0.0f;
        float gyro_x_dps = 0.0f;
        float gyro_y_dps = 0.0f;
        float gyro_z_dps = 0.0f;
        float roll_deg = 0.0f;
        float pitch_deg = 0.0f;
        float acceleration_magnitude_g = 0.0f;
        float rotation_magnitude_dps = 0.0f;
    };

    bool Initialize(i2c_master_bus_handle_t bus, uint8_t preferred_address);
    bool Read(Sample& sample);
    bool IsAvailable() const { return device_ != nullptr; }
    uint8_t address() const { return address_; }

private:
    bool AddDevice(i2c_master_bus_handle_t bus, uint8_t address);
    bool WriteRegister(uint8_t reg, uint8_t value);
    bool ReadRegisters(uint8_t reg, uint8_t* data, size_t length);

    i2c_master_dev_handle_t device_ = nullptr;
    uint8_t address_ = 0;
    int64_t previous_sample_us_ = 0;
    bool filter_initialized_ = false;
    float filtered_roll_deg_ = 0.0f;
    float filtered_pitch_deg_ = 0.0f;
};
