#include "mpu6050_motion_sensor.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>
#include <array>
#include <cmath>

#define TAG "Mpu6050Motion"

namespace {

constexpr uint8_t kRegisterSampleRateDivider = 0x19;
constexpr uint8_t kRegisterConfig = 0x1a;
constexpr uint8_t kRegisterGyroConfig = 0x1b;
constexpr uint8_t kRegisterAccelConfig = 0x1c;
constexpr uint8_t kRegisterAccelData = 0x3b;
constexpr uint8_t kRegisterPowerManagement1 = 0x6b;
constexpr uint8_t kRegisterWhoAmI = 0x75;
constexpr int kI2cTimeoutMs = 100;
constexpr float kRadiansToDegrees = 57.2957795f;

float NormalizeAngle(float angle_deg) {
    while (angle_deg > 180.0f) {
        angle_deg -= 360.0f;
    }
    while (angle_deg < -180.0f) {
        angle_deg += 360.0f;
    }
    return angle_deg;
}

int16_t DecodeSigned(const uint8_t* data) {
    return static_cast<int16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

}  // namespace

bool Mpu6050MotionSensor::AddDevice(i2c_master_bus_handle_t bus, uint8_t address) {
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 400 * 1000,
        .scl_wait_us = 0,
        .flags = {.disable_ack_check = false},
    };
    if (i2c_master_bus_add_device(bus, &config, &device_) != ESP_OK) {
        device_ = nullptr;
        return false;
    }
    address_ = address;
    return true;
}

bool Mpu6050MotionSensor::Initialize(i2c_master_bus_handle_t bus, uint8_t preferred_address) {
    if (bus == nullptr) {
        return false;
    }
    const std::array<uint8_t, 2> addresses = {
        preferred_address, static_cast<uint8_t>(preferred_address == 0x68 ? 0x69 : 0x68)};
    uint8_t who_am_i = 0;
    for (const uint8_t address : addresses) {
        if (i2c_master_probe(bus, address, kI2cTimeoutMs) != ESP_OK || !AddDevice(bus, address)) {
            continue;
        }
        if (ReadRegisters(kRegisterWhoAmI, &who_am_i, 1) && (who_am_i & 0x7e) == 0x68) {
            break;
        }
        i2c_master_bus_rm_device(device_);
        device_ = nullptr;
        address_ = 0;
    }
    if (device_ == nullptr) {
        return false;
    }

    // PLL clock, 100 Hz sample generation, 44 Hz DLPF, +/-500 dps gyro, +/-2 g accel.
    if (!WriteRegister(kRegisterPowerManagement1, 0x01) ||
        !WriteRegister(kRegisterSampleRateDivider, 0x09) || !WriteRegister(kRegisterConfig, 0x03) ||
        !WriteRegister(kRegisterGyroConfig, 0x08) || !WriteRegister(kRegisterAccelConfig, 0x00)) {
        i2c_master_bus_rm_device(device_);
        device_ = nullptr;
        address_ = 0;
        return false;
    }

    ESP_LOGI(TAG, "MPU6050 ready at 0x%02x (WHO_AM_I=0x%02x)", address_, who_am_i);
    return true;
}

bool Mpu6050MotionSensor::WriteRegister(uint8_t reg, uint8_t value) {
    const uint8_t payload[] = {reg, value};
    return device_ != nullptr &&
           i2c_master_transmit(device_, payload, sizeof(payload), kI2cTimeoutMs) == ESP_OK;
}

bool Mpu6050MotionSensor::ReadRegisters(uint8_t reg, uint8_t* data, size_t length) {
    return device_ != nullptr && data != nullptr && length > 0 &&
           i2c_master_transmit_receive(device_, &reg, 1, data, length, kI2cTimeoutMs) == ESP_OK;
}

bool Mpu6050MotionSensor::Read(Sample& sample) {
    std::array<uint8_t, 14> data = {};
    if (!ReadRegisters(kRegisterAccelData, data.data(), data.size())) {
        return false;
    }

    sample.acceleration_x_g = DecodeSigned(data.data()) / 16384.0f;
    sample.acceleration_y_g = DecodeSigned(data.data() + 2) / 16384.0f;
    sample.acceleration_z_g = DecodeSigned(data.data() + 4) / 16384.0f;
    sample.gyro_x_dps = DecodeSigned(data.data() + 8) / 65.5f;
    sample.gyro_y_dps = DecodeSigned(data.data() + 10) / 65.5f;
    sample.gyro_z_dps = DecodeSigned(data.data() + 12) / 65.5f;
    sample.acceleration_magnitude_g = std::sqrt(sample.acceleration_x_g * sample.acceleration_x_g +
                                                sample.acceleration_y_g * sample.acceleration_y_g +
                                                sample.acceleration_z_g * sample.acceleration_z_g);
    sample.rotation_magnitude_dps =
        std::sqrt(sample.gyro_x_dps * sample.gyro_x_dps + sample.gyro_y_dps * sample.gyro_y_dps +
                  sample.gyro_z_dps * sample.gyro_z_dps);

    const float accel_roll =
        std::atan2(sample.acceleration_y_g, sample.acceleration_z_g) * kRadiansToDegrees;
    const float accel_pitch =
        std::atan2(-sample.acceleration_x_g,
                   std::sqrt(sample.acceleration_y_g * sample.acceleration_y_g +
                             sample.acceleration_z_g * sample.acceleration_z_g)) *
        kRadiansToDegrees;
    const int64_t now_us = esp_timer_get_time();
    const float dt = previous_sample_us_ == 0
                         ? 0.04f
                         : std::clamp((now_us - previous_sample_us_) / 1000000.0f, 0.005f, 0.2f);
    previous_sample_us_ = now_us;
    if (!filter_initialized_) {
        filtered_roll_deg_ = NormalizeAngle(accel_roll);
        filtered_pitch_deg_ = NormalizeAngle(accel_pitch);
        filter_initialized_ = true;
    } else {
        constexpr float kGyroWeight = 0.94f;
        const float predicted_roll = NormalizeAngle(filtered_roll_deg_ + sample.gyro_x_dps * dt);
        const float predicted_pitch = NormalizeAngle(filtered_pitch_deg_ + sample.gyro_y_dps * dt);
        // Blend using the shortest angular distance. A linear average of +179 and -179 degrees
        // points at zero and causes the fixed under-chassis mounting to drift after every crossing.
        filtered_roll_deg_ = NormalizeAngle(
            predicted_roll + (1.0f - kGyroWeight) * NormalizeAngle(accel_roll - predicted_roll));
        filtered_pitch_deg_ = NormalizeAngle(
            predicted_pitch + (1.0f - kGyroWeight) * NormalizeAngle(accel_pitch - predicted_pitch));
    }
    sample.roll_deg = filtered_roll_deg_;
    sample.pitch_deg = filtered_pitch_deg_;
    return true;
}
