#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace secondary_oled_layout {

constexpr uint8_t kDisplayWidth = 128;
constexpr uint8_t kDisplayHeight = 32;
constexpr size_t kMaxWidgets = 10;
constexpr size_t kMaxPanelsPerPage = 3;
constexpr size_t kMaxPages = kMaxWidgets;

enum class WidgetSize : uint8_t {
    kSmall,
    kMedium,
    kLarge,
};

enum Capability : uint8_t {
    kFits42x32 = 1 << 0,
    kFits64x32 = 1 << 1,
    kFits128x16 = 1 << 2,
    kFits128x32 = 1 << 3,
};

struct WidgetSpec {
    uint8_t id = 0;
    WidgetSize size = WidgetSize::kSmall;
    bool enabled = false;
    uint8_t capabilities = kFits128x32;
};

struct Placement {
    uint8_t id = 0;
    uint8_t page = 0;
    uint8_t x = 0;
    uint8_t y = 0;
    uint8_t width = 0;
    uint8_t height = 0;
};

struct Layout {
    std::array<Placement, kMaxWidgets> placements = {};
    uint8_t placement_count = 0;
    uint8_t page_count = 0;
};

constexpr bool Supports(const WidgetSpec& widget, Capability capability) {
    return (widget.capabilities & capability) != 0;
}

constexpr void AddPlacement(Layout& layout, const WidgetSpec& widget, uint8_t page, uint8_t x,
                            uint8_t y, uint8_t width, uint8_t height) {
    layout.placements[layout.placement_count++] = {widget.id, page, x, y, width, height};
}

constexpr Layout BuildLayout(const std::array<WidgetSpec, kMaxWidgets>& widgets) {
    std::array<WidgetSpec, kMaxWidgets> enabled = {};
    size_t enabled_count = 0;
    for (const auto& widget : widgets) {
        if (widget.enabled) {
            enabled[enabled_count++] = widget;
        }
    }

    Layout layout;
    size_t cursor = 0;
    uint8_t page = 0;
    while (cursor < enabled_count) {
        const size_t remaining = enabled_count - cursor;

        // Three panels are valid only as three full-height columns. A 42-pixel capability is
        // conservative for the actual 42/43/43-pixel rectangles.
        if (remaining >= 3 && Supports(enabled[cursor], kFits42x32) &&
            Supports(enabled[cursor + 1], kFits42x32) &&
            Supports(enabled[cursor + 2], kFits42x32)) {
            AddPlacement(layout, enabled[cursor], page, 0, 0, 42, 32);
            AddPlacement(layout, enabled[cursor + 1], page, 42, 0, 43, 32);
            AddPlacement(layout, enabled[cursor + 2], page, 85, 0, 43, 32);
            cursor += 3;
            ++page;
            continue;
        }

        if (remaining >= 2) {
            // Full-height columns are preferred over two 16-pixel rows.
            if (Supports(enabled[cursor], kFits64x32) &&
                Supports(enabled[cursor + 1], kFits64x32)) {
                AddPlacement(layout, enabled[cursor], page, 0, 0, 64, 32);
                AddPlacement(layout, enabled[cursor + 1], page, 64, 0, 64, 32);
                cursor += 2;
                ++page;
                continue;
            }
            if (Supports(enabled[cursor], kFits128x16) &&
                Supports(enabled[cursor + 1], kFits128x16)) {
                AddPlacement(layout, enabled[cursor], page, 0, 0, 128, 16);
                AddPlacement(layout, enabled[cursor + 1], page, 0, 16, 128, 16);
                cursor += 2;
                ++page;
                continue;
            }
        }

        // Every supported base panel must provide a complete full-screen renderer. Falling back
        // to one panel preserves order and readability when no multi-panel template is valid.
        if (!Supports(enabled[cursor], kFits128x32)) {
            return {};
        }
        AddPlacement(layout, enabled[cursor], page, 0, 0, 128, 32);
        ++cursor;
        ++page;
    }
    layout.page_count = page;
    return layout;
}

constexpr bool RectanglesOverlap(const Placement& left, const Placement& right) {
    return left.page == right.page && left.x < right.x + right.width &&
           right.x < left.x + left.width && left.y < right.y + right.height &&
           right.y < left.y + left.height;
}

constexpr bool IsValid(const Layout& layout) {
    if (layout.placement_count > kMaxWidgets || layout.page_count > kMaxPages ||
        (layout.placement_count == 0) != (layout.page_count == 0)) {
        return false;
    }

    for (size_t index = 0; index < layout.placement_count; ++index) {
        const auto& placement = layout.placements[index];
        if (placement.page >= layout.page_count || placement.width == 0 || placement.height == 0 ||
            placement.x + placement.width > kDisplayWidth ||
            placement.y + placement.height > kDisplayHeight) {
            return false;
        }
        size_t panels_on_page = 0;
        uint16_t area = 0;
        for (size_t other = 0; other < layout.placement_count; ++other) {
            if (layout.placements[other].page == placement.page) {
                ++panels_on_page;
                area += static_cast<uint16_t>(layout.placements[other].width) *
                        layout.placements[other].height;
            }
            if (other > index && RectanglesOverlap(placement, layout.placements[other])) {
                return false;
            }
        }
        if (panels_on_page > kMaxPanelsPerPage || area != kDisplayWidth * kDisplayHeight) {
            return false;
        }
    }
    return true;
}

}  // namespace secondary_oled_layout
