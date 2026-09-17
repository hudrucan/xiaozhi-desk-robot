#pragma once

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <mutex>

class SharedI2cBus {
public:
    using DeferredInitializer = void (*)(void* context);

    SharedI2cBus(i2c_port_t port, gpio_num_t sda, gpio_num_t scl, const char* name);

    bool Initialize();
    bool StartDeferredInitialization(void* context, DeferredInitializer initializer);
    i2c_master_bus_handle_t handle() const { return bus_; }

    // Cooperative lock for project-owned multi-step operations and lifecycle work.
    // Camera SCCB does not use this lock; ESP-IDF serializes master transactions.
    std::mutex& mutex() { return mutex_; }

private:
    void RunDeferredInitialization();
    static void DeferredInitializationTask(void* arg);

    i2c_port_t port_;
    gpio_num_t sda_;
    gpio_num_t scl_;
    const char* name_;
    i2c_master_bus_handle_t bus_ = nullptr;
    std::mutex mutex_;
    TaskHandle_t initialization_task_ = nullptr;
    void* initializer_context_ = nullptr;
    DeferredInitializer initializer_ = nullptr;
};
