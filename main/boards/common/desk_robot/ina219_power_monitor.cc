#include "ina219_power_monitor.h"

#include <esp_log.h>

#include <algorithm>
#include <array>
#include <cmath>

#define TAG "Ina219Power"

namespace {

constexpr uint8_t kRegisterConfig = 0x00;
constexpr uint8_t kRegisterShuntVoltage = 0x01;
constexpr uint8_t kRegisterBusVoltage = 0x02;
constexpr uint8_t kRegisterCurrent = 0x04;
constexpr uint8_t kRegisterCalibration = 0x05;

// 32 V range, +/-320 mV shunt range, 12-bit ADCs, continuous shunt+bus conversion.
// The wide shunt range tolerates motor startup current better than the +/-40 mV default.
constexpr uint16_t kConfigContinuous = 0x399f;
constexpr int kI2cTimeoutMs = 100;

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

    // 100 uA/bit gives a signed range of roughly 3.27 A. Calibration follows
    // CAL = 0.04096 / (current_lsb_A * shunt_ohms).
    constexpr float kCurrentLsbA = 0.0001f;
    const uint16_t calibration = static_cast<uint16_t>(
        std::clamp(std::lround(0.04096f / (kCurrentLsbA * shunt_resistance_ohms)), 1L, 65535L));
    current_lsb_ma_ = kCurrentLsbA * 1000.0f;
    if (!WriteRegister(kRegisterConfig, kConfigContinuous) ||
        !WriteRegister(kRegisterCalibration, calibration)) {
        i2c_master_bus_rm_device(device_);
        device_ = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "INA219 ready at 0x%02x, shunt %.3f ohm, calibration %u", address,
             shunt_resistance_ohms, calibration);
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

int Ina219PowerMonitor::EstimateBatteryPercent(float voltage_v) {
    // A conservative resting-voltage curve for one 3.7 V Li-ion cell. The
    // board smooths readings, so motor transients do not immediately move the indicator.
    struct Point {
        float voltage;
        int percent;
    };
    constexpr std::array<Point, 10> curve = {{{3.20f, 0},
                                              {3.45f, 5},
                                              {3.55f, 10},
                                              {3.65f, 20},
                                              {3.75f, 40},
                                              {3.82f, 60},
                                              {3.90f, 75},
                                              {4.00f, 90},
                                              {4.10f, 97},
                                              {4.20f, 100}}};
    if (voltage_v <= curve.front().voltage) {
        return 0;
    }
    if (voltage_v >= curve.back().voltage) {
        return 100;
    }
    for (size_t i = 1; i < curve.size(); ++i) {
        if (voltage_v <= curve[i].voltage) {
            const float ratio =
                (voltage_v - curve[i - 1].voltage) / (curve[i].voltage - curve[i - 1].voltage);
            return std::clamp(
                static_cast<int>(std::lround(curve[i - 1].percent +
                                             ratio * (curve[i].percent - curve[i - 1].percent))),
                0, 100);
        }
    }
    return 100;
}

bool Ina219PowerMonitor::Read(Reading& reading) {
    uint16_t bus_raw = 0;
    uint16_t shunt_raw = 0;
    uint16_t current_raw = 0;
    if (!ReadRegister(kRegisterBusVoltage, bus_raw) ||
        !ReadRegister(kRegisterShuntVoltage, shunt_raw) ||
        !ReadRegister(kRegisterCurrent, current_raw)) {
        return false;
    }

    reading.shunt_voltage_mv = static_cast<float>(static_cast<int16_t>(shunt_raw)) * 0.01f;
    // The INA219 bus register measures VIN-. With the sensor wired high-side as
    // battery+ -> VIN+ -> VIN- -> charger, add the shunt drop to recover battery voltage.
    reading.bus_voltage_v =
        static_cast<float>(bus_raw >> 3) * 0.004f + reading.shunt_voltage_mv / 1000.0f;
    reading.current_ma = static_cast<float>(static_cast<int16_t>(current_raw)) * current_lsb_ma_;
    // Computing this from signed current also reports charging as negative power; the
    // INA219 power register itself is unsigned and is awkward for bidirectional current.
    reading.power_mw = reading.bus_voltage_v * reading.current_ma;
    reading.battery_percent = EstimateBatteryPercent(reading.bus_voltage_v);
    // This robot's high-side wiring reports charger-to-battery current as positive
    // and battery-to-load current as negative.
    reading.charging = reading.current_ma > 20.0f;
    reading.discharging = reading.current_ma < -20.0f;
    return true;
}
