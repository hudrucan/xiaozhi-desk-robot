#include "environment_controller.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#define TAG "Environment"

bool EnvironmentController::Start(i2c_master_bus_handle_t bus, std::mutex& bus_mutex,
                                  void* light_context, LightCallback light_callback) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    if (bus == nullptr || !task_idle_) {
        return false;
    }

    bus_ = bus;
    bus_mutex_ = &bus_mutex;
    light_context_ = light_context;
    light_callback_ = light_callback;
    task_idle_ = false;
    running_.store(true, std::memory_order_release);
    const TaskHandle_t existing_task = task_.load(std::memory_order_acquire);
    if (existing_task != nullptr) {
        xTaskNotifyGive(existing_task);
        return true;
    }

    TaskHandle_t task = nullptr;
    if (xTaskCreateWithCaps(TaskEntry, "environment", 6144, this, 1, &task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        running_.store(false, std::memory_order_release);
        task_idle_ = true;
        bus_ = nullptr;
        bus_mutex_ = nullptr;
        ESP_LOGE(TAG, "Failed to create environment task in PSRAM");
        return false;
    }
    task_.store(task, std::memory_order_release);
    return true;
}

void EnvironmentController::Stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    const TaskHandle_t task = task_.load(std::memory_order_acquire);
    if (task != nullptr) {
        xTaskNotifyGive(task);
    }
}

EnvironmentStatus EnvironmentController::GetStatus() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}
