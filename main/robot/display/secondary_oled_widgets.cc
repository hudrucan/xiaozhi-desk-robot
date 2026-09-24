#include "secondary_oled.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

namespace {

constexpr uint8_t kRobotIcon[8] = {
    0x18, 0x18, 0x7e, 0x5a, 0xff, 0x81, 0xff, 0x66,
};
constexpr uint8_t kDistanceIcon[8] = {
    0x08, 0x12, 0x24, 0x49, 0x24, 0x12, 0x08, 0x00,
};
constexpr uint8_t kPowerIcon[8] = {
    0x18, 0x18, 0x30, 0x7e, 0x0c, 0x18, 0x18, 0x00,
};
constexpr uint8_t kMotionIcon[8] = {
    0x18, 0x5a, 0x3c, 0xff, 0x3c, 0x5a, 0x18, 0x00,
};
constexpr uint8_t kCapacityIcon[8] = {
    0x18, 0x7e, 0x42, 0x5a, 0x5a, 0x5a, 0x7e, 0x00,
};
constexpr uint8_t kBatteryIcon[8] = {
    0x00, 0x7e, 0x42, 0x5a, 0x5a, 0x42, 0x7e, 0x18,
};
constexpr uint8_t kWifiIcon[8] = {
    0x02, 0x05, 0x29, 0x55, 0x29, 0x05, 0x02, 0x00,
};

std::pair<std::string, std::string> SplitForTwoLines(const std::string& text) {
    if (text.empty()) {
        return {"", ""};
    }
    if (text.size() <= 10 && text.find(' ') == std::string::npos) {
        return {text, ""};
    }
    // Keep both halves balanced. A distant word boundary can make one half overflow a 42-pixel
    // panel, so only use whitespace immediately adjacent to the midpoint.
    const size_t middle = text.size() / 2;
    size_t split = middle;
    if (middle < text.size() && text[middle] == ' ') {
        split = middle;
    } else if (middle > 0 && text[middle - 1] == ' ') {
        split = middle - 1;
    }
    while (split > 0 && split < text.size() &&
           (static_cast<uint8_t>(text[split]) & 0xc0) == 0x80) {
        --split;
    }
    std::string first = text.substr(0, split);
    std::string second = text.substr(split);
    while (!second.empty() && second.front() == ' ') {
        second.erase(second.begin());
    }
    return {std::move(first), std::move(second)};
}

std::pair<std::string, std::string> SplitIpAddress(const std::string& address) {
    if (address.empty()) {
        return {"No IP", ""};
    }
    const size_t middle = address.size() / 2;
    size_t split = address.rfind('.', middle);
    if (split == std::string::npos) {
        split = address.find('.', middle);
    }
    if (split == std::string::npos) {
        return {address, ""};
    }
    return {address.substr(0, split), address.substr(split + 1)};
}

std::string FormatCapacityMah(uint32_t capacity_uah) {
    char text[16] = {};
    const uint32_t capacity_mah =
        static_cast<uint32_t>((static_cast<uint64_t>(capacity_uah) + 500) / 1000);
    std::snprintf(text, sizeof(text), "%lu mAh", static_cast<unsigned long>(capacity_mah));
    return text;
}

std::string FormatRemainingMah(uint32_t remaining_mah) {
    char text[16] = {};
    std::snprintf(text, sizeof(text), "%lu mAh", static_cast<unsigned long>(remaining_mah));
    return text;
}

std::string FormatElapsed(uint32_t seconds) {
    char text[16] = {};
    const uint32_t hours = seconds / 3600;
    if (hours >= 100000) {
        const uint32_t days = seconds / 86400;
        std::snprintf(text, sizeof(text), "%lud", static_cast<unsigned long>(days));
    } else {
        const uint32_t minutes = (seconds % 3600) / 60;
        std::snprintf(text, sizeof(text), "%luh%02lum", static_cast<unsigned long>(hours),
                      static_cast<unsigned long>(minutes));
    }
    return text;
}

std::string FormatPowerWatts(int power_mw) {
    char text[16] = {};
    const int limited = std::clamp(power_mw, -99999, 99999);
    const int magnitude = std::abs(limited);
    std::snprintf(text, sizeof(text), "%s%d.%02dW", limited < 0 ? "-" : "", magnitude / 1000,
                  (magnitude % 1000) / 10);
    return text;
}

}  // namespace

void SecondaryOled::RenderSingleLineWidgetLocked(
    const secondary_oled_layout::Placement& placement, const WidgetConfig& widget) {
    std::string primary;
    std::string secondary;
    const uint8_t* icon = nullptr;

    switch (widget.type) {
        case WidgetType::kBranding:
            primary = config_.brand;
            icon = kRobotIcon;
            break;
        case WidgetType::kDistance: {
            char distance[32] = {};
            if (telemetry_.distance_valid) {
                std::snprintf(distance, sizeof(distance), "%dmm",
                              std::clamp(telemetry_.distance_mm, 0, 9999));
            } else {
                std::snprintf(distance, sizeof(distance), "--mm");
            }
            primary = config_.distance_prefix + " " + distance;
            icon = kDistanceIcon;
            break;
        }
        case WidgetType::kPower: {
            if (telemetry_.power_valid) {
                const int current_ma = std::clamp(telemetry_.current_ma, -99999, 99999);
                char current[16] = {};
                std::snprintf(current, sizeof(current), "%dmA", current_ma);
                primary = current;
                secondary = FormatPowerWatts(telemetry_.power_mw);
            } else {
                primary = "--mA";
                secondary = "--W";
            }
            const auto mode = static_cast<PowerMode>(std::min<uint8_t>(widget.mode, 2));
            if (mode == PowerMode::kPower) {
                primary = secondary;
            } else if (mode == PowerMode::kCurrentAndPower) {
                primary += " " + secondary;
            }
            icon = kPowerIcon;
            break;
        }
        case WidgetType::kMotion: {
            const std::string state =
                telemetry_.motion_valid ? telemetry_.motion_state : "calibrating";
            if (telemetry_.motion_valid) {
                char tilt[32] = {};
                std::snprintf(tilt, sizeof(tilt), "%d/%d", telemetry_.roll_deg,
                              telemetry_.pitch_deg);
                secondary = tilt;
            } else {
                secondary = "--/--";
            }
            const auto mode = static_cast<MotionMode>(std::min<uint8_t>(widget.mode, 2));
            if (mode == MotionMode::kState) {
                primary = state;
            } else if (mode == MotionMode::kTilt) {
                primary = secondary;
            } else {
                primary = state + " " + secondary;
            }
            icon = kMotionIcon;
            break;
        }
        case WidgetType::kCapacity: {
            const std::string capacity = FormatCapacityMah(telemetry_.capacity_uah);
            const std::string elapsed = FormatElapsed(telemetry_.capacity_seconds);
            primary = capacity;
            secondary = elapsed;
            const auto mode = static_cast<CapacityMode>(std::min<uint8_t>(widget.mode, 2));
            if (mode == CapacityMode::kElapsed) {
                primary = secondary;
            } else if (mode == CapacityMode::kMahAndElapsed) {
                primary += " " + secondary;
            }
            icon = kCapacityIcon;
            break;
        }
        case WidgetType::kBatteryRemaining:
            primary = telemetry_.power_valid
                          ? FormatRemainingMah(telemetry_.battery_remaining_mah)
                          : "--mAh";
            icon = kBatteryIcon;
            break;
        case WidgetType::kNetwork:
            primary = telemetry_.network_state == NetworkState::kConnected &&
                              !telemetry_.ip_address.empty()
                          ? telemetry_.ip_address
                          : "No IP";
            icon = kWifiIcon;
            break;
        case WidgetType::kClimate:
        case WidgetType::kPressure:
        case WidgetType::kLight:
            RenderEnvironmentSingleLineWidgetLocked(placement, widget);
            return;
    }

    constexpr int kOuterPadding = 2;
    constexpr int kDividerPadding = 4;
    const int left_padding = placement.x == 0 ? kOuterPadding : kDividerPadding;
    const int right_padding = placement.x + placement.width == width_ ? kOuterPadding
                                                                      : kDividerPadding;
    const int left = placement.x + left_padding;
    const int top = placement.y;
    const int content_width =
        std::max(0, static_cast<int>(placement.width) - left_padding - right_padding);
    const int content_height = placement.height == 16 ? (placement.y == 0 ? 15 : 16)
                                                      : placement.height;
    const FontSize preferred =
        placement.height == 16 ? FontSize::kRegular : FontSize::kEmphasis;

    if (HasNonAscii(primary)) {
        DrawNotoTextFitted(left, top, content_width, content_height, primary);
        return;
    }

    // Large full-width widgets need more breathing room between the 8px icon
    // and the text. Keep the existing 12px icon/text offset for S/M widgets,
    // but use a 16px offset for L so the icon does not visually touch the text.
    if (widget.size == WidgetSize::kLarge && placement.width == 128 && icon != nullptr &&
        content_width > 16) {
        constexpr int kLargeIconTextOffset = 16;
        const int text_area_width = content_width - kLargeIconTextOffset;
        const FontSize selected =
            SelectSingleLineFont(primary, text_area_width, content_height, preferred);
        const int text_width = MeasureTextWidth(primary, selected);
        if (text_width <= text_area_width && FontHeight(selected) <= content_height) {
            const int group_width = kLargeIconTextOffset + text_width;
            const int group_left = left + std::max(0, (content_width - group_width) / 2);
            DrawBitmap(group_left, top + std::max(0, (content_height - 8) / 2), icon, 8, 8);
            DrawTextFitted(group_left + kLargeIconTextOffset, top, text_width, content_height,
                           primary, selected, false);
            return;
        }
    }

    DrawIconTextFitted(left, top, content_width, content_height, icon, primary, preferred,
                       placement.width > 43);
}

void SecondaryOled::RenderWidgetLocked(const secondary_oled_layout::Placement& placement) {
    const WidgetType type = static_cast<WidgetType>(placement.id);
    const WidgetConfig* widget = nullptr;
    for (const auto& candidate : config_.widgets) {
        if (candidate.type == type) {
            widget = &candidate;
            break;
        }
    }
    if (widget == nullptr) {
        return;
    }

    // Climate owns its combined-value layout. Keep the large variant on one
    // fitted line instead of relying on the generic single-line dispatch.
    if (widget->type == WidgetType::kClimate && widget->size == WidgetSize::kLarge) {
        RenderEnvironmentWidgetLocked(placement, *widget);
        return;
    }

    if (placement.height == 16 ||
        (placement.width == 128 && widget->size == WidgetSize::kLarge)) {
        RenderSingleLineWidgetLocked(placement, *widget);
        return;
    }

    constexpr int kOuterPadding = 2;
    constexpr int kDividerPadding = 4;
    const int left_padding = placement.x == 0 ? kOuterPadding : kDividerPadding;
    const int right_padding = placement.x + placement.width == width_ ? kOuterPadding
                                                                      : kDividerPadding;
    const int left = placement.x + left_padding;
    const int top = placement.y;
    const int content_width =
        std::max(0, static_cast<int>(placement.width) - left_padding - right_padding);
    const int content_height = placement.height;
    const bool roomy = content_width >= 54;
    const bool allow_icon = placement.width > 43;
    char first[24] = {};
    char second[24] = {};

    switch (type) {
        case WidgetType::kBranding: {
            if (HasNonAscii(config_.brand)) {
                DrawNotoTextFitted(left, top, content_width, content_height, config_.brand);
                break;
            }
            const auto lines = SplitForTwoLines(config_.brand);
            if (!lines.second.empty()) {
                DrawIconTwoLinesFitted(left, top, content_width, content_height, kRobotIcon,
                                       lines.first, lines.second, FontSize::kRegular, allow_icon);
            } else {
                DrawIconTextFitted(left, top, content_width, content_height, kRobotIcon,
                                   lines.first, FontSize::kEmphasis, allow_icon);
            }
            break;
        }
        case WidgetType::kDistance: {
            if (telemetry_.distance_valid) {
                std::snprintf(first, sizeof(first), "%dmm",
                              std::clamp(telemetry_.distance_mm, 0, 9999));
            } else {
                std::snprintf(first, sizeof(first), "--mm");
            }
            if (HasNonAscii(config_.distance_prefix)) {
                DrawNotoTextFitted(left, top, content_width, content_height,
                                   config_.distance_prefix + " " + first);
                break;
            }
            if (roomy) {
                DrawIconTwoLinesFitted(left, top, content_width, content_height, kDistanceIcon,
                                       config_.distance_prefix, first, FontSize::kRegular,
                                       allow_icon);
            } else {
                DrawIconTextFitted(left, top, content_width, content_height, kDistanceIcon, first,
                                   FontSize::kEmphasis, allow_icon);
            }
            break;
        }
        case WidgetType::kPower: {
            if (telemetry_.power_valid) {
                const int current_ma = std::clamp(telemetry_.current_ma, -99999, 99999);
                std::snprintf(first, sizeof(first), "%dmA", current_ma);
                const std::string power = FormatPowerWatts(telemetry_.power_mw);
                std::snprintf(second, sizeof(second), "%s", power.c_str());
            } else {
                std::snprintf(first, sizeof(first), "--mA");
                std::snprintf(second, sizeof(second), "--W");
            }
            const auto mode = static_cast<PowerMode>(std::min<uint8_t>(widget->mode, 2));
            if (mode == PowerMode::kCurrentAndPower) {
                DrawTwoLinesFitted(left, top, content_width, content_height, first, second,
                                   FontSize::kRegular);
            } else {
                const std::string value = mode == PowerMode::kCurrent ? first : second;
                DrawIconTextFitted(left, top, content_width, content_height, kPowerIcon, value,
                                   FontSize::kEmphasis, allow_icon);
            }
            break;
        }
        case WidgetType::kMotion: {
            if (telemetry_.motion_valid) {
                std::snprintf(second, sizeof(second), "%d/%d", telemetry_.roll_deg,
                              telemetry_.pitch_deg);
            } else {
                std::snprintf(second, sizeof(second), "--/--");
            }
            std::string state = telemetry_.motion_valid ? telemetry_.motion_state : "calibrating";
            if (state == "calibrating") {
                state = "calib";
            }
            const auto mode = static_cast<MotionMode>(std::min<uint8_t>(widget->mode, 2));
            if (mode == MotionMode::kStateAndTilt) {
                DrawIconTwoLinesFitted(left, top, content_width, content_height, kMotionIcon, state,
                                       second, FontSize::kRegular, allow_icon);
            } else {
                DrawIconTextFitted(left, top, content_width, content_height, kMotionIcon,
                                   mode == MotionMode::kState ? state : second,
                                   FontSize::kEmphasis, allow_icon);
            }
            break;
        }
        case WidgetType::kCapacity: {
            const std::string capacity = FormatCapacityMah(telemetry_.capacity_uah);
            const std::string elapsed = FormatElapsed(telemetry_.capacity_seconds);
            std::snprintf(first, sizeof(first), "%s", capacity.c_str());
            std::snprintf(second, sizeof(second), "%s", elapsed.c_str());
            const auto mode = static_cast<CapacityMode>(std::min<uint8_t>(widget->mode, 2));
            if (mode == CapacityMode::kMahAndElapsed) {
                DrawIconTwoLinesFitted(left, top, content_width, content_height, kCapacityIcon,
                                       first, second, FontSize::kRegular, allow_icon);
            } else {
                DrawIconTextFitted(left, top, content_width, content_height, kCapacityIcon,
                                   mode == CapacityMode::kMah ? first : second,
                                   FontSize::kEmphasis, allow_icon);
            }
            break;
        }
        case WidgetType::kBatteryRemaining: {
            const std::string remaining = telemetry_.power_valid
                                              ? FormatRemainingMah(telemetry_.battery_remaining_mah)
                                              : "--mAh";
            if (roomy) {
                DrawIconTwoLinesFitted(left, top, content_width, content_height, kBatteryIcon,
                                       "Rmn", remaining, FontSize::kRegular, allow_icon);
            } else {
                DrawIconTextFitted(left, top, content_width, content_height, kBatteryIcon,
                                   remaining, FontSize::kEmphasis, allow_icon);
            }
            break;
        }
        case WidgetType::kNetwork: {
            const std::string address =
                telemetry_.network_state == NetworkState::kConnected &&
                        !telemetry_.ip_address.empty()
                    ? telemetry_.ip_address
                    : "No IP";
            if (roomy) {
                DrawIconTwoLinesFitted(left, top, content_width, content_height, kWifiIcon, "IP",
                                       address, FontSize::kRegular, allow_icon);
            } else {
                const auto lines = SplitIpAddress(address);
                DrawTwoLinesFitted(left, top, content_width, content_height, lines.first,
                                   lines.second, FontSize::kRegular);
            }
            break;
        }
        case WidgetType::kClimate:
        case WidgetType::kPressure:
        case WidgetType::kLight:
            RenderEnvironmentWidgetLocked(placement, *widget);
            break;
    }
}
