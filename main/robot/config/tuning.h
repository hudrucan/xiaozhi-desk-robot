#ifndef XIAOZHI_ROBOT_TUNING_H_
#define XIAOZHI_ROBOT_TUNING_H_

// Default status-light behavior.
#define STATUS_LIGHT_DEFAULT_BRIGHTNESS 100

// Downward VL53L0X floor/cliff sampling and safety policy.
#define CLIFF_EDGE_DISTANCE_MM 150
#define CLIFF_CONFIRM_SAMPLES 2
#define CLIFF_AUTO_RETREAT_MS 180
#define DISTANCE_SENSOR_PERIOD_MS 80

// INA219 sampling and battery state-of-charge calibration.
#define INA219_SAMPLE_PERIOD_MS 100
#define BATTERY_SOC_USABLE_CAPACITY_MAH 2862.1f
// Initial diagnostic limit: ten nominal INA219 periods. Hardware logs should be used to confirm
// this still exceeds normal scheduling jitter before it is treated as a final value.
#define BATTERY_SOC_MAX_INTEGRATION_GAP_MS 1000
// Initial quasi-rest parameters derived from the 2026-09-14 FullHD R100 idle/motor captures.
#define BATTERY_SOC_QUASI_REST_MAX_CURRENT_MA 850.0f
#define BATTERY_SOC_QUASI_REST_CURRENT_STDDEV_MA 75.0f
#define BATTERY_SOC_QUASI_REST_VOLTAGE_STDDEV_MV 8.0f
#define BATTERY_SOC_QUASI_REST_CURRENT_TRANSITION_MA 350.0f
#define BATTERY_SOC_QUASI_REST_VOLTAGE_TRANSITION_MV 40.0f
#define BATTERY_SOC_QUASI_REST_QUALIFICATION_MS 60000
#define BATTERY_SOC_QUASI_REST_CORRECTION_INTERVAL_MS 60000
#define BATTERY_SOC_QUASI_REST_CORRECTION_TIME_CONSTANT_MS (6 * 60 * 60 * 1000)
// Full-charge taper measured with the installed charger: four LEDs steady at 4.246 V and -97 mA.
// Require observed charging followed by sustained taper/near-zero current, so high open-circuit
// voltage alone cannot anchor full. One minute filters startup transients without delaying an
// already-full cold boot excessively.
#define BATTERY_SOC_FULL_ANCHOR_MIN_VOLTAGE_V 4.20f
#define BATTERY_SOC_FULL_ANCHOR_TAPER_CURRENT_MA 150.0f
#define BATTERY_SOC_FULL_ANCHOR_QUALIFICATION_MS 60000
// A fully charged cold boot under the robot's idle load measured 94.99% on the OCV curve. This
// one-shot recovery handles charging completed while the ESP32 was switched off.
#define BATTERY_SOC_BOOTSTRAP_FULL_ANCHOR_MIN_VOLTAGE_SOC_PERCENT 94.5f
// Ignore small OCV/load-model differences; startup recovery only repairs a meaningful upward delta.
#define BATTERY_SOC_BOOTSTRAP_VOLTAGE_REBASE_MIN_DELTA_PERCENT 2.0f
// Controlled 2026-09-15 discharge stayed operational at 3.199 V and collapsed around
// 3.11-3.17 V under a 0.64-0.71 A idle load. Anchor before the power disappears.
#define BATTERY_SOC_EMPTY_ANCHOR_MAX_VOLTAGE_V 3.20f
#define BATTERY_SOC_EMPTY_ANCHOR_QUALIFICATION_MS 10000
#define BATTERY_CAPACITY_LOW_VOLTAGE_V 3.20f
#define BATTERY_CAPACITY_LOW_VOLTAGE_DURATION_MS 10000

// MPU6050 sampling, gesture thresholds and reaction timing.
#define MPU6050_SAMPLE_PERIOD_MS 40
#define MPU6050_TILT_THRESHOLD_DEG 28.0f
#define MPU6050_SHAKE_THRESHOLD_DPS 180.0f
#define MPU6050_PRESS_THRESHOLD_G 1.25f
#define MPU6050_IMPACT_THRESHOLD_G 1.75f
#define MPU6050_FREEFALL_THRESHOLD_G 0.45f
#define MPU6050_GESTURE_COOLDOWN_MS 2500

#endif  // XIAOZHI_ROBOT_TUNING_H_
