#include "environment_controller.h"

#include <esp_log.h>

#define TAG "Environment"

bool EnvironmentController::Start(i2c_master_bus_handle_t bus, void* light_context,
                                  LightCallback light_callback) {
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    if (bus == nullptr || task_.load(std::memory_order_acquire) != nullptr) {
        return false;
    }

    bus_ = bus;
    light_context_ = light_context;
    light_callback_ = light_callback;
    running_.store(true, std::memory_order_release);
    TaskHandle_t task = nullptr;
    if (xTaskCreate(TaskEntry, "environment", 6144, this, 1, &task) != pdPASS) {
        running_.store(false, std::memory_order_release);
        bus_ = nullptr;
        ESP_LOGE(TAG, "Failed to create environment task");
        return false;
    }
    task_.store(task, std::memory_order_release);
    return true;
}

void EnvironmentController::Stop() {
    running_.store(false, std::memory_order_release);
    const TaskHandle_t task = task_.load(std::memory_order_acquire);
    if (task != nullptr) {
        xTaskNotifyGive(task);
    }
}

EnvironmentStatus EnvironmentController::GetStatus() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}
