#include "bmp280_sensor.h"

#include "config/hardware_config.h"

#include <esp_err.h>

#include <array>
#include <cmath>

namespace {

constexpr uint8_t kRegisterCalibration = 0x88;
constexpr uint8_t kRegisterChipId = 0xd0;
constexpr uint8_t kRegisterPressure = 0xf7;
constexpr uint8_t kRegisterControl = 0xf4;
constexpr uint8_t kRegisterConfig = 0xf5;
constexpr uint8_t kExpectedChipId = 0x58;
constexpr uint8_t kControlNormalTemp1Pressure4 = 0x2f;
constexpr uint8_t kConfigStandby1000Filter4 = 0xa8;
constexpr int kI2cTimeoutMs = 100;

uint16_t DecodeUnsigned(const uint8_t* data) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[1]) << 8) | data[0]);
}

int16_t DecodeSigned(const uint8_t* data) {
    return static_cast<int16_t>(DecodeUnsigned(data));
}

}  // namespace

Bmp280Sensor::~Bmp280Sensor() { Shutdown(); }

bool Bmp280Sensor::Initialize(i2c_master_bus_handle_t bus) {
    Shutdown();
    if (bus == nullptr) {
        return false;
    }

    constexpr std::array<uint8_t, 2> addresses = {BMP280_I2C_ADDRESS_PRIMARY,
                                                   BMP280_I2C_ADDRESS_FALLBACK};
    uint8_t chip_id = 0;
    for (const uint8_t address : addresses) {
        if (i2c_master_probe(bus, address, kI2cTimeoutMs) != ESP_OK || !AddDevice(bus, address)) {
            continue;
        }
        if (ReadRegisters(kRegisterChipId, &chip_id, 1) && chip_id == kExpectedChipId) {
            break;
        }
        Shutdown();
    }
    if (device_ == nullptr || !ReadCalibration() ||
        !WriteRegister(kRegisterConfig, kConfigStandby1000Filter4) ||
        !WriteRegister(kRegisterControl, kControlNormalTemp1Pressure4)) {
        Shutdown();
        return false;
    }
    return true;
}

bool Bmp280Sensor::Read(Reading& reading) {
    reading = {};
    std::array<uint8_t, 6> data = {};
    if (!ReadRegisters(kRegisterPressure, data.data(), data.size())) {
        return false;
    }

    const int32_t raw_pressure = (static_cast<int32_t>(data[0]) << 12) |
                                 (static_cast<int32_t>(data[1]) << 4) | (data[2] >> 4);
    const int32_t raw_temperature = (static_cast<int32_t>(data[3]) << 12) |
                                    (static_cast<int32_t>(data[4]) << 4) | (data[5] >> 4);
    if (raw_pressure == 0x80000 || raw_temperature == 0x80000) {
        return false;
    }

    double var1 = raw_temperature / 16384.0 - calibration_.t1 / 1024.0;
    var1 *= calibration_.t2;
    double var2 = raw_temperature / 131072.0 - calibration_.t1 / 8192.0;
    var2 = var2 * var2 * calibration_.t3;
    const double fine_temperature = var1 + var2;
    const double temperature_c = fine_temperature / 5120.0;

    var1 = fine_temperature / 2.0 - 64000.0;
    var2 = var1 * var1 * calibration_.p6 / 32768.0;
    var2 += var1 * calibration_.p5 * 2.0;
    var2 = var2 / 4.0 + calibration_.p4 * 65536.0;
    var1 = (calibration_.p3 * var1 * var1 / 524288.0 + calibration_.p2 * var1) /
           524288.0;
    var1 = (1.0 + var1 / 32768.0) * calibration_.p1;
    if (std::abs(var1) < 0.000001) {
        return false;
    }
    double pressure_pa = 1048576.0 - raw_pressure;
    pressure_pa = (pressure_pa - var2 / 4096.0) * 6250.0 / var1;
    var1 = calibration_.p9 * pressure_pa * pressure_pa / 2147483648.0;
    var2 = pressure_pa * calibration_.p8 / 32768.0;
    pressure_pa += (var1 + var2 + calibration_.p7) / 16.0;

    reading.temperature_c = static_cast<float>(temperature_c);
    reading.pressure_hpa = static_cast<float>(pressure_pa / 100.0);
    return std::isfinite(reading.temperature_c) && std::isfinite(reading.pressure_hpa) &&
           reading.temperature_c >= -50.0f && reading.temperature_c <= 100.0f &&
           reading.pressure_hpa > 0.0f && reading.pressure_hpa <= 1200.0f;
}

void Bmp280Sensor::Shutdown() {
    if (device_ != nullptr) {
        i2c_master_bus_rm_device(device_);
        device_ = nullptr;
    }
    address_ = 0;
    calibration_ = {};
}

bool Bmp280Sensor::AddDevice(i2c_master_bus_handle_t bus, uint8_t address) {
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

bool Bmp280Sensor::WriteRegister(uint8_t reg, uint8_t value) {
    const uint8_t payload[] = {reg, value};
    return device_ != nullptr &&
           i2c_master_transmit(device_, payload, sizeof(payload), kI2cTimeoutMs) == ESP_OK;
}

bool Bmp280Sensor::ReadRegisters(uint8_t reg, uint8_t* data, size_t length) {
    return device_ != nullptr && data != nullptr && length > 0 &&
           i2c_master_transmit_receive(device_, &reg, 1, data, length, kI2cTimeoutMs) == ESP_OK;
}

bool Bmp280Sensor::ReadCalibration() {
    std::array<uint8_t, 24> data = {};
    if (!ReadRegisters(kRegisterCalibration, data.data(), data.size())) {
        return false;
    }
    calibration_ = {
        .t1 = DecodeUnsigned(data.data()),
        .t2 = DecodeSigned(data.data() + 2),
        .t3 = DecodeSigned(data.data() + 4),
        .p1 = DecodeUnsigned(data.data() + 6),
        .p2 = DecodeSigned(data.data() + 8),
        .p3 = DecodeSigned(data.data() + 10),
        .p4 = DecodeSigned(data.data() + 12),
        .p5 = DecodeSigned(data.data() + 14),
        .p6 = DecodeSigned(data.data() + 16),
        .p7 = DecodeSigned(data.data() + 18),
        .p8 = DecodeSigned(data.data() + 20),
        .p9 = DecodeSigned(data.data() + 22),
    };
    return calibration_.t1 != 0 && calibration_.p1 != 0;
}
