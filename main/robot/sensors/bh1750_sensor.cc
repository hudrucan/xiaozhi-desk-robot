#include "bh1750_sensor.h"

#include "config/hardware_config.h"

#include <esp_err.h>

#include <array>
#include <cmath>

namespace {

constexpr uint8_t kPowerOn = 0x01;
constexpr uint8_t kPowerDown = 0x00;
constexpr uint8_t kContinuousHighResolution = 0x10;
constexpr int kI2cTimeoutMs = 100;

}  // namespace

Bh1750Sensor::~Bh1750Sensor() { Shutdown(); }

bool Bh1750Sensor::Initialize(i2c_master_bus_handle_t bus) {
    Shutdown();
    if (bus == nullptr) {
        return false;
    }

    constexpr std::array<uint8_t, 2> addresses = {BH1750_I2C_ADDRESS_PRIMARY,
                                                   BH1750_I2C_ADDRESS_FALLBACK};
    for (const uint8_t address : addresses) {
        if (i2c_master_probe(bus, address, kI2cTimeoutMs) == ESP_OK && AddDevice(bus, address)) {
            break;
        }
    }
    if (device_ == nullptr) {
        return false;
    }

    if (i2c_master_transmit(device_, &kPowerOn, 1, kI2cTimeoutMs) != ESP_OK ||
        i2c_master_transmit(device_, &kContinuousHighResolution, 1, kI2cTimeoutMs) != ESP_OK) {
        Shutdown();
        return false;
    }
    return true;
}

bool Bh1750Sensor::ReadLux(float& lux) {
    std::array<uint8_t, 2> data = {};
    if (device_ == nullptr ||
        i2c_master_receive(device_, data.data(), data.size(), kI2cTimeoutMs) != ESP_OK) {
        return false;
    }
    const uint16_t raw = static_cast<uint16_t>((data[0] << 8) | data[1]);
    lux = static_cast<float>(raw) / 1.2f;
    return std::isfinite(lux) && lux >= 0.0f;
}

void Bh1750Sensor::Shutdown() {
    if (device_ != nullptr) {
        i2c_master_transmit(device_, &kPowerDown, 1, kI2cTimeoutMs);
        i2c_master_bus_rm_device(device_);
        device_ = nullptr;
    }
    address_ = 0;
}

bool Bh1750Sensor::AddDevice(i2c_master_bus_handle_t bus, uint8_t address) {
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
