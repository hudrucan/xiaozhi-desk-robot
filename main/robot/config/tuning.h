#ifndef XIAOZHI_ROBOT_TUNING_H_
#define XIAOZHI_ROBOT_TUNING_H_

// Default status-light behavior.
#define STATUS_LIGHT_DEFAULT_BRIGHTNESS 100

// Downward VL53L0X floor/cliff sampling and safety policy.
#define CLIFF_EDGE_DISTANCE_MM 150
#define CLIFF_CONFIRM_SAMPLES 2
#define CLIFF_AUTO_RETREAT_MS 180
#define DISTANCE_SENSOR_PERIOD_MS 80

// Best-effort environment sensing on the primary camera/cliff I2C bus.
#define ENVIRONMENT_START_DELAY_MS 750
#define ENVIRONMENT_SERVICE_PERIOD_MS 50
#define ENVIRONMENT_REPROBE_PERIOD_MS 30000
#define ENVIRONMENT_FAILURE_THRESHOLD 3
#define AHT20_SAMPLE_PERIOD_MS 2000
#define AHT20_CONVERSION_TIME_MS 100
#define BMP280_SAMPLE_PERIOD_MS 1000
#define BH1750_SAMPLE_PERIOD_MS 500
#define BH1750_INITIAL_CONVERSION_TIME_MS 180
// Preliminary environment classifications. Revisit after the final sensor mounting is measured.
#define ENVIRONMENT_LIGHT_DARK_MAX_LUX 15.0f
#define ENVIRONMENT_LIGHT_DIM_MAX_LUX 60.0f
#define ENVIRONMENT_LIGHT_NORMAL_MAX_LUX 350.0f
#define ENVIRONMENT_LIGHT_BRIGHT_MAX_LUX 700.0f

// The upward-facing ambient sensor reads substantially brighter than the scene
// seen by the forward-facing camera. Treat ordinary 100-150 lx indoor readings
// as low light for the OV2640, and require office-like illumination before
// returning to the normal profile. The gap prevents profile flapping.
#define CAMERA_AUTO_LOW_LIGHT_ENTER_LUX 180.0f
#define CAMERA_AUTO_LOW_LIGHT_EXIT_LUX 300.0f
#define CAMERA_AUTO_LIGHT_STALE_MS 5000
#define CAMERA_REACTION_PENDING_MS 30000
#define CAMERA_REACTION_SUCCESS_MS 1200
#define CAMERA_REACTION_FAILURE_MS 1800
#define ENVIRONMENT_COMFORT_COLD_MAX_C 18.0f
#define ENVIRONMENT_COMFORT_HOT_MIN_C 28.0f
#define ENVIRONMENT_COMFORT_DRY_MAX_PERCENT 35.0f
#define ENVIRONMENT_COMFORT_HUMID_MIN_PERCENT 70.0f
#define ENVIRONMENT_PRESSURE_HISTORY_INTERVAL_MS 30000
#define ENVIRONMENT_PRESSURE_HISTORY_WINDOW_MS 300000
#define ENVIRONMENT_PRESSURE_TREND_MIN_SPAN_MS 120000
#define ENVIRONMENT_PRESSURE_TREND_THRESHOLD_HPA 0.6f

// Lux-based TFT backlight policy. Defaults stay disabled until explicitly enabled by the user.
#define AUTO_BRIGHTNESS_DEFAULT_MIN_PERCENT 15
#define AUTO_BRIGHTNESS_DEFAULT_MAX_PERCENT 85
#define AUTO_BRIGHTNESS_FILTER_ALPHA 0.25f
#define AUTO_BRIGHTNESS_HYSTERESIS_PERCENT 5
#define AUTO_BRIGHTNESS_MIN_UPDATE_INTERVAL_MS 5000
#define AUTO_BRIGHTNESS_DARK_MAX_LUX 15.0f
#define AUTO_BRIGHTNESS_DIM_MAX_LUX 60.0f
#define AUTO_BRIGHTNESS_INDOOR_MAX_LUX 150.0f
#define AUTO_BRIGHTNESS_BRIGHT_MAX_LUX 350.0f
#define AUTO_BRIGHTNESS_VERY_BRIGHT_MAX_LUX 700.0f
#define AUTO_BRIGHTNESS_DARK_PERCENT 50
#define AUTO_BRIGHTNESS_DIM_PERCENT 65
#define AUTO_BRIGHTNESS_INDOOR_PERCENT 60
#define AUTO_BRIGHTNESS_BRIGHT_PERCENT 75
#define AUTO_BRIGHTNESS_VERY_BRIGHT_PERCENT 90
#define AUTO_BRIGHTNESS_SUNLIT_PERCENT 100

// BH1750-driven SSD1306 contrast. Values are native SSD1306 contrast units.
#define OLED_AUTO_CONTRAST_DEFAULT_MIN 48
#define OLED_AUTO_CONTRAST_DEFAULT_MAX 224
#define OLED_AUTO_CONTRAST_FILTER_ALPHA 0.25f
#define OLED_AUTO_CONTRAST_HYSTERESIS 8
#define OLED_AUTO_CONTRAST_MIN_UPDATE_INTERVAL_MS 5000
#define OLED_AUTO_CONTRAST_DARK 64
#define OLED_AUTO_CONTRAST_DIM 102
#define OLED_AUTO_CONTRAST_INDOOR 153
#define OLED_AUTO_CONTRAST_BRIGHT 191
#define OLED_AUTO_CONTRAST_VERY_BRIGHT 217
#define OLED_AUTO_CONTRAST_SUNLIT 242

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
