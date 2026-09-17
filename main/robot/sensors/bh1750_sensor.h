#pragma once

#include <driver/i2c_master.h>

#include <cstdint>

class Bh1750Sensor {
public:
    ~Bh1750Sensor();

    bool Initialize(i2c_master_bus_handle_t bus);
    bool ReadLux(float& lux);
    void Shutdown();

    bool IsAvailable() const { return device_ != nullptr; }
    uint8_t address() const { return address_; }

private:
    bool AddDevice(i2c_master_bus_handle_t bus, uint8_t address);

    i2c_master_dev_handle_t device_ = nullptr;
    uint8_t address_ = 0;
};
