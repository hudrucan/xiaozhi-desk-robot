#include "robot_mcp_tools.h"

#include "config/hardware_config.h"
#include "config/tuning.h"
#include "control/robot_controller.h"
#include "mcp_server.h"
#include "sensors/environment_derived.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>

#include <string>

#define TAG "RobotMcpTools"

namespace {

bool ParseDirection(const std::string& direction, MotorController::Direction& command) {
    if (direction == "forward") {
        command = MotorController::Direction::kForward;
    } else if (direction == "backward") {
        command = MotorController::Direction::kBackward;
    } else if (direction == "left") {
        command = MotorController::Direction::kLeft;
    } else if (direction == "right") {
        command = MotorController::Direction::kRight;
    } else {
        return false;
    }
    return true;
}

}  // namespace

void RobotMcpTools::Register(RobotController& controller) {
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddTool(
        "self.robot.drive",
        "Move forward, backward, left, or right, then stop after `duration_ms`. Cliff safety may "
        "block movement.",
        PropertyList({
            Property("direction", kPropertyTypeString),
            Property("duration_ms", kPropertyTypeInteger, 250, 50, 5000),
        }),
        [&controller](const PropertyList& properties) -> ToolResult {
            const auto direction = properties["direction"].value<std::string>();
            MotorController::Direction command;
            if (!ParseDirection(direction, command)) {
                return std::string("direction must be forward, backward, left, or right");
            }
            const int duration_ms = properties["duration_ms"].value<int>();
            if (!controller.Move(command, duration_ms,
                                 RobotController::MovePolicy::kPreserveQueued)) {
                return std::string(
                    "Movement blocked: table edge detected; reverse remains available");
            }
            return true;
        });
    mcp_server.AddTool("self.robot.stop", "Stop both drive motors now.", PropertyList(),
                       [&controller](const PropertyList&) -> ReturnValue {
                           controller.Stop();
                           return true;
                       });
    mcp_server.AddTool(
        "self.robot.get_status", "Get current drive-motor state.", PropertyList(),
        [&controller](const PropertyList&) -> ReturnValue {
            return MotorController::StatusJson(controller.GetStatus().motors);
        });
    mcp_server.AddTool("self.robot.dance", "Run one bounded random dance.",
                       PropertyList(), [&controller](const PropertyList&) -> ReturnValue {
                           return controller.Dance();
                       });
    mcp_server.AddTool(
        "self.face.set_emotion",
        "Show a temporary face emotion: neutral, happy, bored, laughing, funny, sad, angry, "
        "crying, loving, "
        "embarrassed, surprised, shocked, thinking, winking, cool, relaxed, delicious, "
        "kissy, confident, sleepy, silly, confused, suspicious, and shake.",
        PropertyList({
            Property("emotion", kPropertyTypeString, "neutral"),
            Property("duration_ms", kPropertyTypeInteger, 5000, 250, 30000),
        }),
        [&controller](const PropertyList& properties) -> ToolResult {
            const std::string emotion = properties["emotion"].value<std::string>();
            if (!controller.ShowEmotion(emotion, properties["duration_ms"].value<int>())) {
                return std::string("Unsupported face emotion");
            }
            return true;
        });
    mcp_server.AddTool(
        "self.face.look",
        "Look temporarily: center, left, right, up, down, up_left, up_right, down_left, or "
        "down_right.",
        PropertyList({
            Property("direction", kPropertyTypeString, "center"),
            Property("duration_ms", kPropertyTypeInteger, 2500, 250, 15000),
        }),
        [&controller](const PropertyList& properties) -> ToolResult {
            std::string direction = properties["direction"].value<std::string>();
            if (direction == "center") {
                direction = "neutral";
            }
            if (!controller.ShowEmotion(direction, properties["duration_ms"].value<int>())) {
                return std::string("Unsupported look direction");
            }
            return true;
        });
#ifdef SECONDARY_OLED_I2C_ADDRESS
    mcp_server.AddTool(
        "self.secondary_display.show_text",
        "Show short UTF-8 text, including Vietnamese, on the secondary OLED; telemetry returns "
        "afterward.",
        PropertyList({
            Property("text", kPropertyTypeString),
            Property("duration_ms", kPropertyTypeInteger, 5000, 500, 60000),
        }),
        [&controller](const PropertyList& properties) -> ToolResult {
            if (!controller.ShowSecondaryText(properties["text"].value<std::string>(),
                                              properties["duration_ms"].value<int>())) {
                return std::string("OLED is unavailable or text is empty");
            }
            return true;
        });
#endif
    mcp_server.AddTool(
        "self.status_light.set_effect",
        "Temporarily set the status lights to steady, breathe, blink, or off. Motor indication "
        "has priority.",
        PropertyList({
            Property("effect", kPropertyTypeString, "steady"),
            Property("duration_ms", kPropertyTypeInteger, 5000, 250, 30000),
        }),
        [&controller](const PropertyList& properties) -> ToolResult {
            if (!controller.SetStatusLightEffect(properties["effect"].value<std::string>(),
                                                 properties["duration_ms"].value<int>())) {
                return std::string("Unsupported status-light effect");
            }
            return true;
        });
#ifdef DISTANCE_SENSOR_I2C_ADDRESS
    mcp_server.AddTool(
        "self.distance.get",
        "Get downward floor distance and cliff-detection state.", PropertyList(),
        [&controller](const PropertyList&) -> ReturnValue {
            const auto cliff = controller.GetStatus().cliff;
            if (!cliff.available) {
                return std::string(R"({"available":false})");
            }
            return std::string("{\"available\":true,\"valid\":") +
                   (cliff.valid ? "true" : "false") +
                   ",\"distance_mm\":" + std::to_string(cliff.distance_mm) +
                   ",\"cliff_detected\":" + (cliff.cliff_detected ? "true" : "false") +
                   ",\"edge_mm\":" + std::to_string(cliff.edge_mm) + "}";
        });
#endif
    mcp_server.AddTool(
        "self.environment.get",
        "Get cached temperature, humidity, pressure, and illuminance with per-sensor validity.",
        PropertyList(), [&controller](const PropertyList&) -> ToolResult {
            cJSON* result = cJSON_CreateObject();
            if (result == nullptr) {
                return std::unexpected("Out of memory");
            }
            const EnvironmentStatus environment = controller.GetStatus().environment;
            cJSON_AddBoolToObject(result, "aht20_available", environment.aht20_available);
            cJSON_AddBoolToObject(result, "bmp280_available", environment.bmp280_available);
            cJSON_AddBoolToObject(result, "bh1750_available", environment.bh1750_available);
            cJSON_AddBoolToObject(result, "temperature_valid", environment.temperature_valid);
            cJSON_AddBoolToObject(result, "humidity_valid", environment.humidity_valid);
            cJSON_AddBoolToObject(result, "pressure_valid", environment.pressure_valid);
            cJSON_AddBoolToObject(result, "illuminance_valid", environment.illuminance_valid);
            cJSON_AddStringToObject(result, "light_level",
                                    LightLevelName(environment.light_level));
            cJSON_AddStringToObject(result, "comfort_level",
                                    ComfortLevelName(environment.comfort_level));
            cJSON_AddStringToObject(result, "pressure_trend",
                                    PressureTrendName(environment.pressure_trend));
            if (environment.temperature_valid) {
                cJSON_AddNumberToObject(result, "temperature_c", environment.temperature_c);
            }
            if (environment.humidity_valid) {
                cJSON_AddNumberToObject(result, "humidity_percent",
                                        environment.humidity_percent);
            }
            if (environment.pressure_valid) {
                cJSON_AddNumberToObject(result, "pressure_hpa", environment.pressure_hpa);
            }
            if (environment.illuminance_valid) {
                cJSON_AddNumberToObject(result, "illuminance_lux",
                                        environment.illuminance_lux);
            }
            return result;
        });
#ifdef INA219_I2C_ADDRESS
    mcp_server.AddTool(
        "self.battery.get_status",
        "Get detailed battery charge, voltage, current, power, and SoC diagnostics.",
        PropertyList(), [&controller](const PropertyList&) -> ToolResult {
            cJSON* result = cJSON_CreateObject();
            if (result == nullptr) {
                return std::unexpected("Out of memory");
            }
            const auto battery = controller.GetStatus().battery;
            cJSON_AddBoolToObject(result, "available", battery.available);
            cJSON_AddBoolToObject(result, "valid", battery.valid);
            if (battery.valid) {
                cJSON_AddNumberToObject(result, "percent", battery.percent);
                cJSON_AddNumberToObject(result, "voltage_v", battery.voltage_v);
                cJSON_AddNumberToObject(result, "current_ma", battery.current_ma);
                cJSON_AddNumberToObject(result, "power_mw", battery.power_mw);
                cJSON_AddNumberToObject(result, "signed_current_ma", battery.signed_current_ma);
                cJSON_AddNumberToObject(result, "shunt_voltage_mv", battery.shunt_voltage_mv);
                cJSON_AddNumberToObject(result, "bus_voltage_v", battery.bus_voltage_v);
                cJSON_AddNumberToObject(result, "remaining_mah", battery.remaining_mah);
                cJSON_AddNumberToObject(result, "capacity_mah", BATTERY_SOC_USABLE_CAPACITY_MAH);
                cJSON_AddStringToObject(result, "soc_method", "coulomb_quasi_rest_anchors");
                cJSON_AddBoolToObject(result, "soc_tracking_degraded",
                                      battery.soc_tracking_degraded);
                cJSON_AddBoolToObject(result, "soc_quasi_resting", battery.soc_quasi_resting);
                cJSON_AddNumberToObject(result, "soc_voltage_reference_percent",
                                        battery.soc_voltage_reference_percent);
                cJSON_AddNumberToObject(result, "soc_voltage_correction_mah",
                                        battery.soc_voltage_correction_mah);
                cJSON_AddBoolToObject(result, "soc_full_anchored", battery.soc_full_anchored);
                cJSON_AddBoolToObject(result, "soc_bootstrap_voltage_rebased",
                                      battery.soc_bootstrap_voltage_rebased);
                cJSON_AddBoolToObject(result, "soc_empty_anchored", battery.soc_empty_anchored);
                cJSON_AddBoolToObject(result, "conversion_ready", battery.conversion_ready);
                cJSON_AddBoolToObject(result, "math_overflow", battery.math_overflow);
                cJSON_AddBoolToObject(result, "charging", battery.charging);
                cJSON_AddBoolToObject(result, "discharging", battery.discharging);
            }
            return result;
        });
#endif
#ifdef MPU6050_I2C_ADDRESS
    mcp_server.AddTool(
        "self.motion.get_orientation",
        "Get orientation, acceleration, rotation, gesture, and gyro-turn state.",
        PropertyList(), [&controller](const PropertyList&) -> ToolResult {
            cJSON* result = cJSON_CreateObject();
            if (result == nullptr) {
                return std::unexpected("Out of memory");
            }
            const RobotStatus status = controller.GetStatus();
            cJSON_AddBoolToObject(result, "available", status.motion_sensor_available);
            cJSON_AddBoolToObject(result, "valid", status.motion_sensor_valid);
            cJSON_AddBoolToObject(result, "emotion_control", status.motion_emotions_enabled);
            const auto& gyro = status.gyro;
            cJSON_AddBoolToObject(result, "gyro_bias_valid", gyro.bias_valid);
            const int64_t gyro_sample_us = gyro.sample_timestamp_us;
            cJSON_AddNumberToObject(
                result, "gyro_sample_age_ms",
                gyro_sample_us > 0 ? (esp_timer_get_time() - gyro_sample_us) / 1000.0 : -1.0);
            cJSON_AddBoolToObject(result, "gyro_turn_available", gyro.available);
            cJSON_AddBoolToObject(result, "gyro_turn_pending", gyro.pending);
            cJSON_AddBoolToObject(result, "gyro_turn_active", gyro.active);
            cJSON_AddNumberToObject(result, "gyro_turn_target_deg", gyro.target_deg);
            cJSON_AddNumberToObject(result, "gyro_turn_progress_deg", gyro.progress_deg);
            cJSON_AddStringToObject(result, "gyro_turn_stop_reason",
                                    GyroTurnController::StopReasonName(gyro.stop_reason));
            cJSON_AddNumberToObject(result, "yaw_rate_dps", gyro.yaw_rate_dps);
            cJSON_AddNumberToObject(result, "yaw_bias_dps", gyro.yaw_bias_dps);
            if (status.motion_sensor_valid) {
                cJSON_AddNumberToObject(result, "roll_deg", status.motion_roll_deg);
                cJSON_AddNumberToObject(result, "pitch_deg", status.motion_pitch_deg);
                cJSON_AddNumberToObject(result, "acceleration_g", status.motion_acceleration_g);
                cJSON_AddNumberToObject(result, "rotation_dps", status.motion_rotation_dps);
                cJSON_AddStringToObject(result, "gesture", status.motion_gesture.c_str());
            }
            return result;
        });
    mcp_server.AddTool(
        "self.robot.turn_relative",
        "Turn by a gyro-measured angle: positive is right, negative is left. Requires calibrated "
        "MPU6050, idle motors, and a safe floor.",
        PropertyList({Property("degrees", kPropertyTypeInteger, 90, -180, 180)}),
        [&controller](const PropertyList& properties) -> ToolResult {
            std::string message;
            if (!controller.TurnRelative(properties["degrees"].value<int>(), message)) {
                return std::unexpected(message);
            }
            return true;
        });
    ESP_LOGI(TAG, "MPU6050 MCP tools registered");
#endif
}
