#include "secondary_oled.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

constexpr uint8_t kClimateIcon[8] = {
    0x18, 0x24, 0x24, 0x24, 0x24, 0x5a, 0x5a, 0x3c,
};
constexpr uint8_t kPressureIcon[8] = {
    0x00, 0x7e, 0x00, 0x3c, 0x00, 0x18, 0x00, 0x00,
};
constexpr uint8_t kLightIcon[8] = {
    0x18, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x18,
};

std::string FormatTenths(int value, const char* suffix) {
    char text[24] = {};
    const int limited = std::clamp(value, -99999, 99999);
    const int magnitude = std::abs(limited);
    std::snprintf(text, sizeof(text), "%s%d.%d%s", limited < 0 ? "-" : "", magnitude / 10,
                  magnitude % 10, suffix);
    return text;
}

std::string FormatInteger(int value, const char* suffix) {
    char text[24] = {};
    std::snprintf(text, sizeof(text), "%d%s", std::clamp(value, 0, 99999), suffix);
    return text;
}

}  // namespace

void SecondaryOled::RenderEnvironmentSingleLineWidgetLocked(
    const secondary_oled_layout::Placement& placement, const WidgetConfig& widget) {
    std::string primary;
    const uint8_t* icon = nullptr;
    switch (widget.type) {
        case WidgetType::kClimate: {
            const std::string temperature = telemetry_.temperature_valid
                                                ? FormatTenths(telemetry_.temperature_tenths_c, "C")
                                                : "--C";
            const std::string humidity = telemetry_.humidity_valid
                                             ? FormatTenths(telemetry_.humidity_tenths_percent, "%")
                                             : "--%";
            const auto mode = static_cast<ClimateMode>(std::min<uint8_t>(widget.mode, 2));
            primary = mode == ClimateMode::kTemperature
                          ? temperature
                          : mode == ClimateMode::kHumidity ? humidity
                                                           : temperature + " " + humidity;
            icon = kClimateIcon;
            break;
        }
        case WidgetType::kPressure: {
            const auto mode = static_cast<PressureMode>(std::min<uint8_t>(widget.mode, 1));
            if (!telemetry_.pressure_valid) {
                primary = "----hPa";
            } else if (mode == PressureMode::kCompact) {
                primary = FormatInteger((telemetry_.pressure_tenths_hpa + 5) / 10, "hPa");
            } else {
                primary = FormatTenths(telemetry_.pressure_tenths_hpa, "hPa");
            }
            icon = kPressureIcon;
            break;
        }
        case WidgetType::kLight: {
            const std::string lux = telemetry_.illuminance_valid
                                        ? FormatInteger(telemetry_.illuminance_lux, "lx")
                                        : "--lx";
            const std::string level = telemetry_.illuminance_valid
                                          ? telemetry_.light_level
                                          : "unavailable";
            const auto mode = static_cast<LightMode>(std::min<uint8_t>(widget.mode, 2));
            primary = mode == LightMode::kLux
                          ? lux
                          : mode == LightMode::kLevel ? level : lux + " " + level;
            icon = kLightIcon;
            break;
        }
        default:
            return;
    }

    constexpr int kOuterPadding = 2;
    constexpr int kDividerPadding = 4;
    const int left_padding = placement.x == 0 ? kOuterPadding : kDividerPadding;
    const int right_padding = placement.x + placement.width == width_ ? kOuterPadding
                                                                      : kDividerPadding;
    const int left = placement.x + left_padding;
    const int content_width =
        std::max(0, static_cast<int>(placement.width) - left_padding - right_padding);
    const int content_height = placement.height == 16 ? (placement.y == 0 ? 15 : 16)
                                                      : placement.height;
    const FontSize preferred =
        placement.height == 16 ? FontSize::kRegular : FontSize::kEmphasis;
    DrawIconTextFitted(left, placement.y, content_width, content_height, icon, primary, preferred,
                       placement.width > 43, widget.size == WidgetSize::kLarge ? 16 : 12);
}

void SecondaryOled::RenderEnvironmentWidgetLocked(
    const secondary_oled_layout::Placement& placement, const WidgetConfig& widget) {
    constexpr int kOuterPadding = 2;
    constexpr int kDividerPadding = 4;
    const int left_padding = placement.x == 0 ? kOuterPadding : kDividerPadding;
    const int right_padding = placement.x + placement.width == width_ ? kOuterPadding
                                                                      : kDividerPadding;
    const int left = placement.x + left_padding;
    const int content_width =
        std::max(0, static_cast<int>(placement.width) - left_padding - right_padding);
    const bool allow_icon = placement.width > 43;

    if (widget.type == WidgetType::kClimate) {
        const std::string temperature = telemetry_.temperature_valid
                                            ? FormatTenths(telemetry_.temperature_tenths_c, "C")
                                            : "--C";
        const std::string humidity = telemetry_.humidity_valid
                                         ? FormatTenths(telemetry_.humidity_tenths_percent, "%")
                                         : "--%";
        const auto mode = static_cast<ClimateMode>(std::min<uint8_t>(widget.mode, 2));
        if (mode == ClimateMode::kTemperatureAndHumidity) {
            DrawIconTwoLinesFitted(left, placement.y, content_width, placement.height,
                                   kClimateIcon, temperature, humidity, FontSize::kRegular,
                                   allow_icon);
        } else {
            DrawIconTextFitted(left, placement.y, content_width, placement.height, kClimateIcon,
                               mode == ClimateMode::kTemperature ? temperature : humidity,
                               FontSize::kEmphasis, allow_icon);
        }
        return;
    }

    if (widget.type == WidgetType::kPressure) {
        const auto mode = static_cast<PressureMode>(std::min<uint8_t>(widget.mode, 1));
        const std::string pressure = !telemetry_.pressure_valid
                                         ? "----hPa"
                                         : mode == PressureMode::kCompact
                                               ? FormatInteger(
                                                     (telemetry_.pressure_tenths_hpa + 5) / 10,
                                                     "hPa")
                                               : FormatTenths(telemetry_.pressure_tenths_hpa,
                                                              "hPa");
        DrawIconTextFitted(left, placement.y, content_width, placement.height, kPressureIcon,
                           pressure, FontSize::kEmphasis, allow_icon);
        return;
    }

    if (widget.type == WidgetType::kLight) {
        const std::string lux = telemetry_.illuminance_valid
                                    ? FormatInteger(telemetry_.illuminance_lux, "lx")
                                    : "--lx";
        const std::string level =
            telemetry_.illuminance_valid ? telemetry_.light_level : "unavailable";
        const auto mode = static_cast<LightMode>(std::min<uint8_t>(widget.mode, 2));
        if (mode == LightMode::kLuxAndLevel) {
            DrawIconTwoLinesFitted(left, placement.y, content_width, placement.height, kLightIcon,
                                   lux, level, FontSize::kRegular, allow_icon);
        } else {
            DrawIconTextFitted(left, placement.y, content_width, placement.height, kLightIcon,
                               mode == LightMode::kLux ? lux : level, FontSize::kEmphasis,
                               allow_icon);
        }
    }
}
