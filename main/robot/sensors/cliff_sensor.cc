#include "cliff_sensor.h"

#include "config/hardware_config.h"
#include "config/tuning.h"

#include <esp_err.h>
#include <esp_log.h>

#include <algorithm>

extern "C" {
#include <vl53l0x.h>
}

#define TAG "CliffSensor"

bool CliffSensor::Initialize(i2c_master_bus_handle_t bus, int edge_mm, void* callback_context,
                             CliffCallback callback) {
    SetEdgeMm(edge_mm);
    callback_context_ = callback_context;
    cliff_callback_ = callback;
    if (i2c_master_probe(bus, DISTANCE_SENSOR_I2C_ADDRESS, 100) != ESP_OK) {
        ESP_LOGW(TAG, "VL53L0X not detected at 0x%02x", DISTANCE_SENSOR_I2C_ADDRESS);
        return false;
    }
    vl53l0x_handle_t sensor = sensor_;
    esp_err_t error = vl53l0x_create(&sensor, bus);
    sensor_ = sensor;
    if (error == ESP_OK) {
        error = vl53l0x_init(sensor);
    }
    if (error == ESP_OK) {
        vl53l0x_ref_spad_calibration_t spad_calibration = {};
        error = vl53l0x_perform_ref_spad_management(sensor, &spad_calibration);
        if (error == ESP_OK) {
            error = vl53l0x_set_reference_spads(sensor, &spad_calibration);
        }
    }
    if (error == ESP_OK) {
        vl53l0x_ref_calibration_t reference_calibration = {};
        error = vl53l0x_perform_ref_calibration(sensor, &reference_calibration);
    }
    if (error == ESP_OK) {
        error = vl53l0x_set_profile(sensor, VL53L0X_PROFILE_DEFAULT);
    }
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "VL53L0X initialization failed: %s", esp_err_to_name(error));
        if (sensor != nullptr) {
            vl53l0x_destroy(sensor);
            sensor_ = nullptr;
        }
        return false;
    }
    if (xTaskCreate(TaskEntry, "vl53l0x", 4096, this, 1, &task_) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create VL53L0X task");
        vl53l0x_destroy(sensor);
        sensor_ = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "VL53L0X ready on shared camera I2C bus");
    return true;
}

void CliffSensor::TaskEntry(void* arg) { static_cast<CliffSensor*>(arg)->RunTask(); }

void CliffSensor::RunTask() {
    vl53l0x_handle_t sensor = sensor_;
    TickType_t last_wake_time = xTaskGetTickCount();
    uint8_t unsafe_samples = 0;
    while (true) {
        vl53l0x_data_t reading = {};
        const esp_err_t error = vl53l0x_single_measure(sensor, &reading);
        if (error == ESP_OK) {
            distance_mm_.store(reading.distance_mm);
            distance_valid_.store(reading.valid && reading.distance_mm > 0);
        } else {
            distance_valid_.store(false);
            ESP_LOGW(TAG, "VL53L0X measurement failed: %s", esp_err_to_name(error));
        }

        const int edge_mm = edge_mm_.load(std::memory_order_relaxed);
        const bool floor_detected = error == ESP_OK && reading.valid && reading.distance_mm > 0 &&
                                    reading.distance_mm <= edge_mm;
        if (floor_detected) {
            unsafe_samples = 0;
            cliff_detected_.store(false);
        } else {
            unsafe_samples = std::min<uint8_t>(unsafe_samples + 1, CLIFF_CONFIRM_SAMPLES);
            if (unsafe_samples >= CLIFF_CONFIRM_SAMPLES && !cliff_detected_.exchange(true)) {
                if (reading.valid && reading.distance_mm > 0) {
                    ESP_LOGW(TAG, "Cliff detected: floor is %u mm away", reading.distance_mm);
                } else {
                    ESP_LOGW(TAG, "Cliff detected: no valid floor return");
                }
                if (cliff_callback_ != nullptr) {
                    cliff_callback_(callback_context_);
                }
            }
        }
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(DISTANCE_SENSOR_PERIOD_MS));
    }
}

CliffSensor::Status CliffSensor::GetStatus() const {
    return Status{
        .available = sensor_ != nullptr,
        .valid = distance_valid_.load(),
        .distance_mm = distance_mm_.load(),
        .cliff_detected = IsCliffDetected(),
        .edge_mm = edge_mm_.load(),
    };
}

bool CliffSensor::IsCliffDetected() const {
    return sensor_ != nullptr && cliff_detected_.load();
}

bool CliffSensor::IsFloorSafe() const {
    return distance_valid_.load(std::memory_order_relaxed) && !IsCliffDetected();
}

bool CliffSensor::IsDirectionBlocked(MotorController::Direction direction) const {
    return IsCliffDetected() && direction != MotorController::Direction::kBackward;
}

void CliffSensor::SetEdgeMm(int edge_mm) {
    edge_mm_.store(std::clamp(edge_mm, 50, 500), std::memory_order_relaxed);
}
