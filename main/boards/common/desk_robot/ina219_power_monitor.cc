#include "ina219_power_monitor.h"

#include <esp_log.h>

#include <array>
#include <cmath>
#include <limits>

#define TAG "Ina219Power"

namespace {

constexpr uint8_t kRegisterConfig = 0x00;
constexpr uint8_t kRegisterShuntVoltage = 0x01;
constexpr uint8_t kRegisterBusVoltage = 0x02;
constexpr uint8_t kRegisterCalibration = 0x05;

constexpr uint16_t kBusRange32V = 1U << 13;
constexpr uint16_t kPgaRange320Mv = 3U << 11;
constexpr uint16_t kAdc12Bit16Samples = 12U;
constexpr uint16_t kBusAdc16Samples = kAdc12Bit16Samples << 7;
constexpr uint16_t kShuntAdc16Samples = kAdc12Bit16Samples << 3;
constexpr uint16_t kContinuousShuntAndBus = 7U;
constexpr uint16_t kConfigContinuous =
    kBusRange32V | kPgaRange320Mv | kBusAdc16Samples | kShuntAdc16Samples | kContinuousShuntAndBus;
static_assert(kConfigContinuous == 0x3e67);

constexpr uint16_t kBusConversionReady = 1U << 1;
constexpr uint16_t kBusMathOverflow = 1U;
constexpr float kShuntVoltageLsbV = 10e-6f;
constexpr int kI2cTimeoutMs = 100;

constexpr float kCurrentThresholdMa = 20.0f;

}  // namespace

bool Ina219PowerMonitor::Initialize(i2c_master_bus_handle_t bus, uint8_t address,
                                    float shunt_resistance_ohms) {
    if (bus == nullptr || shunt_resistance_ohms <= 0.0f) {
        return false;
    }

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

    // Derive every shunt-dependent scale from the configured effective resistance. Choosing the
    // ADC's 10 uV shunt-voltage LSB as Current_LSB makes CAL independent of the installed shunt:
    // CAL = 0.04096 / ((10e-6 / Rshunt) * Rshunt) = 4096.
    const float current_lsb_a = kShuntVoltageLsbV / shunt_resistance_ohms;
    const long calibration_value = std::lround(0.04096f / (current_lsb_a * shunt_resistance_ohms));
    if (calibration_value <= 0 || calibration_value > std::numeric_limits<uint16_t>::max()) {
        i2c_master_bus_rm_device(device_);
        device_ = nullptr;
        return false;
    }
    const uint16_t calibration = static_cast<uint16_t>(calibration_value);
    shunt_resistance_ohms_ = shunt_resistance_ohms;
    if (!WriteRegister(kRegisterConfig, kConfigContinuous) ||
        !WriteRegister(kRegisterCalibration, calibration)) {
        i2c_master_bus_rm_device(device_);
        device_ = nullptr;
        return false;
    }

    ESP_LOGI(TAG,
             "INA219 ready at 0x%02x: shunt=%.6f ohm current_lsb=%.3f mA "
             "calibration=%u config=0x%04x",
             address, shunt_resistance_ohms, current_lsb_a * 1000.0f, calibration,
             kConfigContinuous);
    return true;
}

bool Ina219PowerMonitor::WriteRegister(uint8_t reg, uint16_t value) {
    const uint8_t payload[] = {reg, static_cast<uint8_t>(value >> 8),
                               static_cast<uint8_t>(value & 0xff)};
    return device_ != nullptr &&
           i2c_master_transmit(device_, payload, sizeof(payload), kI2cTimeoutMs) == ESP_OK;
}

bool Ina219PowerMonitor::ReadRegister(uint8_t reg, uint16_t& value) {
    std::array<uint8_t, 2> data = {};
    if (device_ == nullptr || i2c_master_transmit_receive(device_, &reg, 1, data.data(),
                                                          data.size(), kI2cTimeoutMs) != ESP_OK) {
        return false;
    }
    value = static_cast<uint16_t>((data[0] << 8) | data[1]);
    return true;
}

bool Ina219PowerMonitor::Read(Reading& reading) {
    reading = {};
    uint16_t bus_raw = 0;
    uint16_t shunt_raw = 0;
    if (!ReadRegister(kRegisterBusVoltage, bus_raw) ||
        !ReadRegister(kRegisterShuntVoltage, shunt_raw)) {
        return false;
    }

    reading.conversion_ready = (bus_raw & kBusConversionReady) != 0;
    reading.math_overflow = (bus_raw & kBusMathOverflow) != 0;
    reading.shunt_voltage_mv = static_cast<float>(static_cast<int16_t>(shunt_raw)) * 0.01f;
    reading.bus_voltage_v = static_cast<float>(bus_raw >> 3) * 0.004f;
    // Preserve the repository's existing voltage semantics: VIN- bus voltage plus the signed
    // shunt drop. Hardware validation still needs to confirm this is the battery-terminal voltage.
    reading.battery_voltage_v = reading.bus_voltage_v + reading.shunt_voltage_mv / 1000.0f;
    reading.current_ma = reading.shunt_voltage_mv / shunt_resistance_ohms_;
    reading.power_mw = reading.battery_voltage_v * reading.current_ma;
    reading.charging = reading.current_ma < -kCurrentThresholdMa;
    reading.discharging = reading.current_ma > +kCurrentThresholdMa;
    reading.valid = reading.conversion_ready && !reading.math_overflow &&
                    std::isfinite(reading.bus_voltage_v) &&
                    std::isfinite(reading.battery_voltage_v) &&
                    std::isfinite(reading.shunt_voltage_mv) && std::isfinite(reading.current_ma) &&
                    std::isfinite(reading.power_mw);
    return true;
}
