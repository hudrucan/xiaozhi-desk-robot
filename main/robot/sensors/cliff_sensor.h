#pragma once

#include "motion/motor_controller.h"

#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>

struct vl53l0x;

class CliffSensor {
public:
    struct Status {
        bool available = false;
        bool valid = false;
        int distance_mm = -1;
        bool cliff_detected = false;
        int edge_mm = 0;
    };

    using CliffCallback = void (*)(void* context);

    bool Initialize(i2c_master_bus_handle_t bus, int edge_mm, void* callback_context,
                    CliffCallback callback);
    Status GetStatus() const;
    bool IsCliffDetected() const;
    bool IsFloorSafe() const;
    bool IsDirectionBlocked(MotorController::Direction direction) const;
    void SetEdgeMm(int edge_mm);

private:
    static void TaskEntry(void* arg);
    void RunTask();

    vl53l0x* sensor_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::atomic_int distance_mm_{-1};
    std::atomic_bool distance_valid_{false};
    std::atomic_bool cliff_detected_{false};
    std::atomic_int edge_mm_{0};
    void* callback_context_ = nullptr;
    CliffCallback cliff_callback_ = nullptr;
};
