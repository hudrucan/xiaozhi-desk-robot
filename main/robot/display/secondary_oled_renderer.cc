#include "secondary_oled.h"

#include <algorithm>

namespace {

// Compact 5x7 uppercase font. Columns are stored least-significant bit at the top.
constexpr uint8_t kBlank[5] = {};
constexpr uint8_t kHyphen[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
constexpr uint8_t kPeriod[5] = {0x00, 0x60, 0x60, 0x00, 0x00};
constexpr uint8_t kPercent[5] = {0x63, 0x13, 0x08, 0x64, 0x63};
constexpr uint8_t kColon[5] = {0x00, 0x36, 0x36, 0x00, 0x00};
constexpr uint8_t kSlash[5] = {0x60, 0x18, 0x06, 0x01, 0x00};
constexpr uint8_t kPlus[5] = {0x08, 0x08, 0x3e, 0x08, 0x08};
constexpr uint8_t kExclamation[5] = {0x00, 0x00, 0x5f, 0x00, 0x00};
constexpr uint8_t kQuestion[5] = {0x02, 0x01, 0x51, 0x09, 0x06};
constexpr uint8_t kUnderscore[5] = {0x40, 0x40, 0x40, 0x40, 0x40};
constexpr uint8_t kDigits[][5] = {
    {0x3e, 0x51, 0x49, 0x45, 0x3e}, {0x00, 0x42, 0x7f, 0x40, 0x00}, {0x42, 0x61, 0x51, 0x49, 0x46},
    {0x21, 0x41, 0x45, 0x4b, 0x31}, {0x18, 0x14, 0x12, 0x7f, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3c, 0x4a, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03}, {0x36, 0x49, 0x49, 0x49, 0x36},
    {0x06, 0x49, 0x49, 0x29, 0x1e},
};
constexpr uint8_t kLetters[][5] = {
    {0x7e, 0x11, 0x11, 0x11, 0x7e}, {0x7f, 0x49, 0x49, 0x49, 0x36}, {0x3e, 0x41, 0x41, 0x41, 0x22},
    {0x7f, 0x41, 0x41, 0x22, 0x1c}, {0x7f, 0x49, 0x49, 0x49, 0x41}, {0x7f, 0x09, 0x09, 0x09, 0x01},
    {0x3e, 0x41, 0x49, 0x49, 0x7a}, {0x7f, 0x08, 0x08, 0x08, 0x7f}, {0x00, 0x41, 0x7f, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3f, 0x01}, {0x7f, 0x08, 0x14, 0x22, 0x41}, {0x7f, 0x40, 0x40, 0x40, 0x40},
    {0x7f, 0x02, 0x0c, 0x02, 0x7f}, {0x7f, 0x04, 0x08, 0x10, 0x7f}, {0x3e, 0x41, 0x41, 0x41, 0x3e},
    {0x7f, 0x09, 0x09, 0x09, 0x06}, {0x3e, 0x41, 0x51, 0x21, 0x5e}, {0x7f, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7f, 0x01, 0x01}, {0x3f, 0x40, 0x40, 0x40, 0x3f},
    {0x1f, 0x20, 0x40, 0x20, 0x1f}, {0x3f, 0x40, 0x38, 0x40, 0x3f}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43},
};
constexpr uint8_t kLowercase[][5] = {
    {0x20, 0x54, 0x54, 0x54, 0x78}, {0x7f, 0x48, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x20},
    {0x38, 0x44, 0x44, 0x48, 0x7f}, {0x38, 0x54, 0x54, 0x54, 0x18}, {0x08, 0x7e, 0x09, 0x01, 0x02},
    {0x0c, 0x52, 0x52, 0x52, 0x3e}, {0x7f, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7d, 0x40, 0x00},
    {0x20, 0x40, 0x44, 0x3d, 0x00}, {0x7f, 0x10, 0x28, 0x44, 0x00}, {0x00, 0x41, 0x7f, 0x40, 0x00},
    {0x7c, 0x04, 0x18, 0x04, 0x78}, {0x7c, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38},
    {0x7c, 0x14, 0x14, 0x14, 0x08}, {0x08, 0x14, 0x14, 0x18, 0x7c}, {0x7c, 0x08, 0x04, 0x04, 0x08},
    {0x48, 0x54, 0x54, 0x54, 0x20}, {0x04, 0x3f, 0x44, 0x40, 0x20}, {0x3c, 0x40, 0x40, 0x20, 0x7c},
    {0x1c, 0x20, 0x40, 0x20, 0x1c}, {0x3c, 0x40, 0x30, 0x40, 0x3c}, {0x44, 0x28, 0x10, 0x28, 0x44},
    {0x0c, 0x50, 0x50, 0x50, 0x3c}, {0x44, 0x64, 0x54, 0x4c, 0x44},
};

}  // namespace

const uint8_t* SecondaryOled::Glyph(char character) {
    if (character == '-') {
        return kHyphen;
    }
    if (character == '.') {
        return kPeriod;
    }
    if (character == '%') {
        return kPercent;
    }
    if (character == ':') {
        return kColon;
    }
    if (character == '/') {
        return kSlash;
    }
    if (character == '+') {
        return kPlus;
    }
    if (character == '!') {
        return kExclamation;
    }
    if (character == '?') {
        return kQuestion;
    }
    if (character == '_') {
        return kUnderscore;
    }
    if (character >= '0' && character <= '9') {
        return kDigits[character - '0'];
    }
    if (character >= 'a' && character <= 'z') {
        return kLowercase[character - 'a'];
    }
    if (character >= 'A' && character <= 'Z') {
        return kLetters[character - 'A'];
    }
    return kBlank;
}

void SecondaryOled::Clear() { framebuffer_.fill(0); }

void SecondaryOled::SetPixel(int x, int y) {
    if (x >= 0 && x < width_ && y >= 0 && y < height_) {
        framebuffer_[x + (y / 8) * width_] |= 1U << (y & 7);
    }
}

int SecondaryOled::FontWidth(FontSize font) {
    switch (font) {
        case FontSize::kMicro:
            return 3;
        case FontSize::kCompact:
            return 4;
        case FontSize::kRegular:
            return 5;
        case FontSize::kEmphasis:
            return 6;
    }
    return 5;
}

int SecondaryOled::FontHeight(FontSize font) {
    switch (font) {
        case FontSize::kMicro:
            return 6;
        case FontSize::kCompact:
            return 8;
        case FontSize::kRegular:
            return 10;
        case FontSize::kEmphasis:
            return 12;
    }
    return 10;
}

int SecondaryOled::MeasureTextWidth(const std::string& text, FontSize font) {
    if (text.empty()) {
        return 0;
    }
    return static_cast<int>(text.size()) * (FontWidth(font) + 1) - 1;
}

SecondaryOled::FontSize SecondaryOled::SelectSingleLineFont(const std::string& text, int width,
                                                            int height, FontSize preferred) {
    FontSize selected = preferred;
    while ((MeasureTextWidth(text, selected) > width || FontHeight(selected) > height) &&
           selected != FontSize::kMicro) {
        selected = static_cast<FontSize>(static_cast<uint8_t>(selected) - 1);
    }
    return selected;
}

SecondaryOled::FontSize SecondaryOled::SelectTwoLineFont(const std::string& first,
                                                         const std::string& second, int width,
                                                         int height, FontSize preferred) {
    FontSize selected = preferred;
    while ((MeasureTextWidth(first, selected) > width ||
            MeasureTextWidth(second, selected) > width || 2 * FontHeight(selected) + 5 > height) &&
           selected != FontSize::kMicro) {
        selected = static_cast<FontSize>(static_cast<uint8_t>(selected) - 1);
    }
    return selected;
}

void SecondaryOled::DrawBitmap(int x, int y, const uint8_t* bitmap, int width, int height) {
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            if ((bitmap[row] & (1U << (width - column - 1))) != 0) {
                SetPixel(x + column, y + row);
            }
        }
    }
}

void SecondaryOled::DrawHorizontalLine(int x, int y, int width) {
    for (int offset = 0; offset < width; ++offset) {
        SetPixel(x + offset, y);
    }
}

void SecondaryOled::DrawIconTextFitted(int x, int y, int width, int height, const uint8_t* icon,
                                       const std::string& text, FontSize preferred,
                                       bool allow_icon, int icon_text_offset) {
    if (allow_icon && icon != nullptr && width > icon_text_offset) {
        const int text_area_width = width - icon_text_offset;
        const FontSize selected = SelectSingleLineFont(text, text_area_width, height, preferred);
        const int text_width = MeasureTextWidth(text, selected);
        if (text_width <= text_area_width && FontHeight(selected) <= height) {
            const int group_width = icon_text_offset + text_width;
            const int group_left = x + std::max(0, (width - group_width) / 2);
            DrawBitmap(group_left, y + std::max(0, (height - 8) / 2), icon, 8, 8);
            DrawTextFitted(group_left + icon_text_offset, y, text_width, height, text, selected,
                           false);
            return;
        }
    }
    DrawTextFitted(x, y, width, height, text, preferred);
}

void SecondaryOled::DrawIconTwoLinesFitted(int x, int y, int width, int height,
                                           const uint8_t* icon, const std::string& first,
                                           const std::string& second, FontSize preferred,
                                           bool allow_icon, int icon_text_offset) {
    if (allow_icon && icon != nullptr && width > icon_text_offset) {
        const int text_area_width = width - icon_text_offset;
        const FontSize selected =
            SelectTwoLineFont(first, second, text_area_width, height, preferred);
        const int first_width = MeasureTextWidth(first, selected);
        const int second_width = MeasureTextWidth(second, selected);
        const int text_width = std::max(first_width, second_width);
        if (text_width <= text_area_width && 2 * FontHeight(selected) + 5 <= height) {
            const int group_width = icon_text_offset + text_width;
            const int group_left = x + std::max(0, (width - group_width) / 2);
            DrawBitmap(group_left, y + std::max(0, (height - 8) / 2), icon, 8, 8);
            DrawTwoLinesFitted(group_left + icon_text_offset, y, text_width, height, first, second,
                               selected, true);
            return;
        }
    }
    DrawTwoLinesFitted(x, y, width, height, first, second, preferred);
}

void SecondaryOled::DrawEventMessageFitted(const uint8_t* icon, const std::string& first,
                                           const std::string& second) {
    constexpr int kOuterPadding = 2;
    constexpr int kEventIconTextOffset = 16;
    const int content_width = width_ - 2 * kOuterPadding;
    const std::string combined = second.empty() ? first : first + " " + second;
    if (MeasureTextWidth(combined, FontSize::kRegular) <= content_width - kEventIconTextOffset) {
        DrawIconTextFitted(kOuterPadding, 0, content_width, height_, icon, combined,
                           FontSize::kEmphasis, true, kEventIconTextOffset);
        return;
    }
    DrawIconTwoLinesFitted(kOuterPadding, 0, content_width, height_, icon, first, second,
                           FontSize::kRegular, true, kEventIconTextOffset);
}

void SecondaryOled::DrawVerticalLine(int x, int y, int height) {
    for (int offset = 0; offset < height; ++offset) {
        SetPixel(x, y + offset);
    }
}

void SecondaryOled::DrawText(int x, int y, const std::string& text, FontSize font, int max_width) {
    const int glyph_width = FontWidth(font);
    const int glyph_height = FontHeight(font);
    const int right = x + max_width;
    for (char character : text) {
        if (x + glyph_width > right) {
            break;
        }
        const uint8_t* glyph = Glyph(character);
        for (int column = 0; column < glyph_width; ++column) {
            const int source_column = column * 5 / glyph_width;
            for (int row = 0; row < glyph_height; ++row) {
                const int source_row = row * 7 / glyph_height;
                if ((glyph[source_column] & (1U << source_row)) != 0) {
                    SetPixel(x + column, y + row);
                }
            }
        }
        x += glyph_width + 1;
    }
}

void SecondaryOled::DrawTextFitted(int x, int y, int width, int height, const std::string& text,
                                   FontSize preferred, bool center_horizontal) {
    if (width <= 0 || height <= 0 || text.empty()) {
        return;
    }
    const FontSize selected = SelectSingleLineFont(text, width, height, preferred);

    // Base-panel capabilities guarantee that their complete text fits at micro size. Refuse an
    // invalid string instead of clipping an important measured value at the rectangle boundary.
    if (MeasureTextWidth(text, selected) > width || FontHeight(selected) > height) {
        return;
    }

    const int text_width = MeasureTextWidth(text, selected);
    DrawText(x + (center_horizontal ? std::max(0, (width - text_width) / 2) : 0),
             y + std::max(0, (height - FontHeight(selected)) / 2), text, selected, width);
}

void SecondaryOled::DrawTwoLinesFitted(int x, int y, int width, int height,
                                       const std::string& first, const std::string& second,
                                       FontSize preferred, bool center_horizontal) {
    if (second.empty()) {
        DrawTextFitted(x, y, width, height, first, preferred, center_horizontal);
        return;
    }
    const FontSize selected = SelectTwoLineFont(first, second, width, height, preferred);
    const int line_height = FontHeight(selected);
    const int line_gap = height >= 2 * line_height + 5 ? 5 : 2;
    const int top = y + std::max(0, (height - (2 * line_height + line_gap)) / 2);
    DrawTextFitted(x, top, width, line_height, first, selected, center_horizontal);
    DrawTextFitted(x, top + line_height + line_gap, width, line_height, second, selected,
                   center_horizontal);
}
