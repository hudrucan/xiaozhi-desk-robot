#include "robot_web_status.h"

#include "config/hardware_config.h"
#include "config/tuning.h"
#include "control/robot_controller.h"

#include <esp_timer.h>
#include <cJSON.h>

cJSON* RobotWebStatus::CreateMotors() {
    const RobotStatus status = controller_.GetStatus();
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return nullptr;
    }
    cJSON_AddNumberToObject(root, "motor_speed", status.motor_speed);
    cJSON_AddNumberToObject(root, "drive_duration_ms", status.drive_duration_ms);
    cJSON* motors = cJSON_AddObjectToObject(root, "motors");
    if (motors != nullptr) {
        cJSON_AddBoolToObject(motors, "available", status.motors.available);
        cJSON_AddBoolToObject(motors, "faulted", status.motors.faulted);
        cJSON_AddBoolToObject(motors, "moving", status.motors.moving);
        cJSON_AddStringToObject(motors, "direction",
                                MotorController::DirectionName(status.motors.direction));
        cJSON_AddNumberToObject(motors, "intensity_percent", status.motors.intensity_percent);
        cJSON_AddNumberToObject(motors, "queued", status.motors.queued);
        cJSON_AddNumberToObject(motors, "remaining_ms", status.motors.remaining_ms);
        cJSON_AddBoolToObject(motors, "sequence_active", status.motors.sequence_active);
        cJSON_AddNumberToObject(motors, "sequence_total", status.motors.sequence_total);
        cJSON_AddNumberToObject(motors, "sequence_completed", status.motors.sequence_completed);
    }
#ifdef MPU6050_I2C_ADDRESS
    cJSON_AddBoolToObject(root, "gyro_turn_available", status.gyro.available);
    cJSON_AddBoolToObject(root, "gyro_turn_pending", status.gyro.pending);
    cJSON_AddBoolToObject(root, "gyro_turn_active", status.gyro.active);
    cJSON_AddNumberToObject(root, "gyro_turn_target_deg", status.gyro.target_deg);
    cJSON_AddNumberToObject(root, "gyro_turn_progress_deg", status.gyro.progress_deg);
    cJSON_AddStringToObject(root, "gyro_turn_stop_reason",
                            GyroTurnController::StopReasonName(status.gyro.stop_reason));
#endif
    return root;
}

cJSON* RobotWebStatus::CreateSensors() {
    const RobotStatus status = controller_.GetStatus();
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return nullptr;
    }
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
    cJSON_AddBoolToObject(root, "distance_available", status.cliff.available);
    cJSON_AddNumberToObject(root, "distance_mm", status.cliff.distance_mm);
    cJSON_AddBoolToObject(root, "distance_valid", status.cliff.valid);
    cJSON_AddBoolToObject(root, "cliff_detected", status.cliff.cliff_detected);
    cJSON_AddNumberToObject(root, "cliff_edge_mm", status.cliff.edge_mm);
#endif
#ifdef MPU6050_I2C_ADDRESS
    cJSON_AddBoolToObject(root, "motion_sensor_available", status.motion_sensor_available);
    cJSON_AddBoolToObject(root, "motion_sensor_valid", status.motion_sensor_valid);
    cJSON_AddBoolToObject(root, "motion_emotions_enabled", status.motion_emotions_enabled);
    cJSON_AddNumberToObject(root, "motion_roll_deg", status.motion_roll_deg);
    cJSON_AddNumberToObject(root, "motion_pitch_deg", status.motion_pitch_deg);
    cJSON_AddNumberToObject(root, "motion_acceleration_g", status.motion_acceleration_g);
    cJSON_AddNumberToObject(root, "motion_rotation_dps", status.motion_rotation_dps);
    cJSON_AddNumberToObject(root, "motion_yaw_rate_dps", status.gyro.yaw_rate_dps);
    cJSON_AddNumberToObject(root, "motion_yaw_bias_dps", status.gyro.yaw_bias_dps);
    const int64_t sample_us = status.gyro.sample_timestamp_us;
    cJSON_AddNumberToObject(
        root, "gyro_sample_age_ms",
        sample_us > 0 ? (esp_timer_get_time() - sample_us) / 1000.0 : -1.0);
    cJSON_AddBoolToObject(root, "gyro_bias_valid", status.gyro.bias_valid);
    cJSON_AddStringToObject(root, "motion_gesture", status.motion_gesture.c_str());
#endif
    return root;
}

cJSON* RobotWebStatus::CreateBattery() {
    const RobotStatus status = controller_.GetStatus();
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return nullptr;
    }
#ifdef INA219_I2C_ADDRESS
    const auto& battery = status.battery;
    cJSON_AddBoolToObject(root, "battery_available", battery.available);
    cJSON_AddBoolToObject(root, "battery_valid", battery.valid);
    cJSON_AddNumberToObject(root, "battery_percent", battery.percent);
    cJSON_AddNumberToObject(root, "battery_voltage_v", battery.voltage_v);
    cJSON_AddNumberToObject(root, "battery_current_ma", battery.current_ma);
    cJSON_AddNumberToObject(root, "battery_power_mw", battery.power_mw);
    cJSON_AddNumberToObject(root, "battery_signed_current_ma", battery.signed_current_ma);
    cJSON_AddNumberToObject(root, "battery_shunt_voltage_mv", battery.shunt_voltage_mv);
    cJSON_AddNumberToObject(root, "battery_bus_voltage_v", battery.bus_voltage_v);
    cJSON_AddNumberToObject(root, "battery_remaining_mah", battery.remaining_mah);
    cJSON_AddNumberToObject(root, "battery_capacity_mah", BATTERY_SOC_USABLE_CAPACITY_MAH);
    cJSON_AddStringToObject(root, "battery_soc_method", "coulomb_quasi_rest_anchors");
    cJSON_AddBoolToObject(root, "battery_soc_tracking_degraded",
                          battery.soc_tracking_degraded);
    cJSON_AddBoolToObject(root, "battery_soc_quasi_resting", battery.soc_quasi_resting);
    cJSON_AddNumberToObject(root, "battery_soc_voltage_reference_percent",
                            battery.soc_voltage_reference_percent);
    cJSON_AddNumberToObject(root, "battery_soc_voltage_correction_mah",
                            battery.soc_voltage_correction_mah);
    cJSON_AddBoolToObject(root, "battery_soc_full_anchored", battery.soc_full_anchored);
    cJSON_AddBoolToObject(root, "battery_soc_bootstrap_voltage_rebased",
                          battery.soc_bootstrap_voltage_rebased);
    cJSON_AddBoolToObject(root, "battery_soc_empty_anchored", battery.soc_empty_anchored);
    cJSON_AddBoolToObject(root, "battery_conversion_ready", battery.conversion_ready);
    cJSON_AddBoolToObject(root, "battery_math_overflow", battery.math_overflow);
    const char* flow_state = !battery.valid                       ? "unknown"
                             : battery.signed_current_ma < -20.0f ? "charging"
                             : battery.signed_current_ma > 20.0f  ? "discharging"
                                                                   : "near_zero";
    cJSON_AddStringToObject(root, "battery_flow_state", flow_state);
    cJSON_AddStringToObject(root, "external_power", "unknown");
    cJSON_AddBoolToObject(root, "battery_charging", battery.charging);
    cJSON_AddBoolToObject(root, "battery_discharging", battery.discharging);
    cJSON_AddBoolToObject(root, "battery_capacity_test_active", battery.capacity_test_active);
    cJSON_AddBoolToObject(root, "battery_capacity_test_measuring",
                          battery.capacity_test_measuring);
    cJSON_AddNumberToObject(root, "battery_capacity_test_mah", battery.capacity_test_uah / 1000.0);
    cJSON_AddNumberToObject(root, "battery_capacity_test_seconds",
                            battery.capacity_test_seconds);
#endif
    return root;
}
