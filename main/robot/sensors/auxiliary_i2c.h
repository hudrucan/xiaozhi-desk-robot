#pragma once

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <mutex>

class AuxiliaryI2c {
public:
    using DeferredInitializer = void (*)(void* context);

    AuxiliaryI2c(i2c_port_t port, gpio_num_t sda, gpio_num_t scl);

    bool StartDeferredInitialization(void* context, DeferredInitializer initializer);
    i2c_master_bus_handle_t handle() const { return bus_; }
    std::mutex& mutex() { return mutex_; }

private:
    bool InitializeBus();
    void RunDeferredInitialization();
    static void DeferredInitializationTask(void* arg);

    i2c_port_t port_;
    gpio_num_t sda_;
    gpio_num_t scl_;
    i2c_master_bus_handle_t bus_ = nullptr;
    std::mutex mutex_;
    TaskHandle_t initialization_task_ = nullptr;
    void* initializer_context_ = nullptr;
    DeferredInitializer initializer_ = nullptr;
};
