#include "aht20_sensor.h"

#include "config/hardware_config.h"

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <array>
#include <cmath>

namespace {

constexpr uint8_t kStatusCommand = 0x71;
constexpr uint8_t kCalibrationEnabled = 1U << 3;
constexpr uint8_t kBusy = 1U << 7;
constexpr int kI2cTimeoutMs = 100;

}  // namespace

Aht20Sensor::~Aht20Sensor() { Shutdown(); }

bool Aht20Sensor::Initialize(i2c_master_bus_handle_t bus) {
    Shutdown();
    if (bus == nullptr || i2c_master_probe(bus, AHT20_I2C_ADDRESS, kI2cTimeoutMs) != ESP_OK) {
        return false;
    }

    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AHT20_I2C_ADDRESS,
        .scl_speed_hz = 400 * 1000,
        .scl_wait_us = 0,
        .flags = {.disable_ack_check = false},
    };
    if (i2c_master_bus_add_device(bus, &config, &device_) != ESP_OK) {
        device_ = nullptr;
        return false;
    }

    uint8_t status = 0;
    if (!ReadStatus(status)) {
        Shutdown();
        return false;
    }
    if ((status & kCalibrationEnabled) == 0) {
        constexpr uint8_t calibration[] = {0xbe, 0x08, 0x00};
        if (i2c_master_transmit(device_, calibration, sizeof(calibration), kI2cTimeoutMs) != ESP_OK) {
            Shutdown();
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        if (!ReadStatus(status) || (status & kCalibrationEnabled) == 0) {
            Shutdown();
            return false;
        }
    }
    return true;
}

bool Aht20Sensor::TriggerMeasurement() {
    constexpr uint8_t command[] = {0xac, 0x33, 0x00};
    return device_ != nullptr &&
           i2c_master_transmit(device_, command, sizeof(command), kI2cTimeoutMs) == ESP_OK;
}

bool Aht20Sensor::ReadMeasurement(Reading& reading) {
    reading = {};
    std::array<uint8_t, 7> data = {};
    if (device_ == nullptr ||
        i2c_master_receive(device_, data.data(), data.size(), kI2cTimeoutMs) != ESP_OK ||
        (data[0] & kBusy) != 0 || CalculateCrc(data.data(), 6) != data[6]) {
        return false;
    }

    const uint32_t humidity_raw =
        (static_cast<uint32_t>(data[1]) << 12) | (static_cast<uint32_t>(data[2]) << 4) |
        (static_cast<uint32_t>(data[3]) >> 4);
    const uint32_t temperature_raw =
        (static_cast<uint32_t>(data[3] & 0x0f) << 16) |
        (static_cast<uint32_t>(data[4]) << 8) | data[5];
    reading.humidity_percent = static_cast<float>(humidity_raw) * 100.0f / 1048576.0f;
    reading.temperature_c = static_cast<float>(temperature_raw) * 200.0f / 1048576.0f - 50.0f;
    return std::isfinite(reading.temperature_c) && std::isfinite(reading.humidity_percent) &&
           reading.temperature_c >= -50.0f && reading.temperature_c <= 100.0f &&
           reading.humidity_percent >= 0.0f && reading.humidity_percent <= 100.0f;
}

void Aht20Sensor::Shutdown() {
    if (device_ != nullptr) {
        i2c_master_bus_rm_device(device_);
        device_ = nullptr;
    }
}

bool Aht20Sensor::ReadStatus(uint8_t& status) {
    return device_ != nullptr &&
           i2c_master_transmit_receive(device_, &kStatusCommand, 1, &status, 1, kI2cTimeoutMs) ==
               ESP_OK;
}

uint8_t Aht20Sensor::CalculateCrc(const uint8_t* data, size_t length) {
    uint8_t crc = 0xff;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) != 0 ? static_cast<uint8_t>((crc << 1) ^ 0x31)
                                     : static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}
