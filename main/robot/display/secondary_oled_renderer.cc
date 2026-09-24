#include "secondary_oled.h"

#include "display/vietnamese_glyph_fallback.h"

#include <lvgl.h>

#include <algorithm>
#include <array>

extern "C" {
LV_FONT_DECLARE(font_noto_sans_basic_20_4);
}

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

constexpr int kNotoLineHeight = 16;
constexpr int kFallbackLineHeight = 28;
constexpr int kFallbackBaseLine = 8;
constexpr int kNotoScaleNumerator = 4;
constexpr int kNotoScaleDenominator = 7;

struct NotoGlyph {
    uint16_t advance = 0;
    uint16_t box_w = 0;
    uint16_t box_h = 0;
    int16_t ofs_x = 0;
    int16_t ofs_y = 0;
    uint16_t stride = 0;
    const uint8_t* bitmap = nullptr;
    uint8_t bpp = 0;
    int line_height = kFallbackLineHeight;
    int base_line = kFallbackBaseLine;
    int scale_numerator = kNotoScaleNumerator;
    int scale_denominator = kNotoScaleDenominator;
};

int ScaleRounded(int value, int numerator, int denominator) {
    if (value >= 0) {
        return (value * numerator + denominator / 2) / denominator;
    }
    return -((-value * numerator + denominator / 2) / denominator);
}

bool ResolveNotoGlyph(uint32_t codepoint, NotoGlyph& glyph) {
    // The main LCD already links this 20px font. Read its immutable raw bitmap directly and
    // downscale to the OLED's 16px line box, avoiding another font asset or an LVGL draw buffer.
    lv_font_glyph_dsc_t descriptor = {};
    if (lv_font_get_glyph_dsc(&font_noto_sans_basic_20_4, &descriptor, codepoint, 0)) {
        descriptor.req_raw_bitmap = 1;
        glyph.advance = static_cast<uint16_t>(std::max(
            1, ScaleRounded(descriptor.adv_w, kNotoScaleNumerator, kNotoScaleDenominator)));
        glyph.box_w = descriptor.box_w;
        glyph.box_h = descriptor.box_h;
        glyph.ofs_x = descriptor.ofs_x;
        glyph.ofs_y = descriptor.ofs_y;
        glyph.stride = descriptor.stride;
        glyph.bitmap = static_cast<const uint8_t*>(
            descriptor.resolved_font->get_glyph_bitmap(&descriptor, nullptr));
        glyph.bpp = 4;
        glyph.line_height = descriptor.resolved_font->line_height;
        glyph.base_line = descriptor.resolved_font->base_line;
        return glyph.box_w == 0 || glyph.box_h == 0 || glyph.bitmap != nullptr;
    }

    VietnameseGlyphView fallback;
    if (!FindVietnameseGlyphFallback(codepoint, fallback)) {
        return false;
    }
    const int source_advance = static_cast<int>((fallback.adv_w + 8) >> 4);
    glyph.advance = static_cast<uint16_t>(std::max(
        1, ScaleRounded(source_advance, kNotoScaleNumerator, kNotoScaleDenominator)));
    glyph.box_w = fallback.box_w;
    glyph.box_h = fallback.box_h;
    glyph.ofs_x = fallback.ofs_x;
    glyph.ofs_y = fallback.ofs_y;
    glyph.bitmap = fallback.bitmap;
    glyph.bpp = 4;
    glyph.line_height = kFallbackLineHeight;
    glyph.base_line = kFallbackBaseLine;
    glyph.scale_numerator = kNotoScaleNumerator;
    glyph.scale_denominator = kNotoScaleDenominator;
    return true;
}

bool NotoGlyphPixel(const NotoGlyph& glyph, int x, int y) {
    if (glyph.bitmap == nullptr || x < 0 || y < 0 || x >= glyph.box_w || y >= glyph.box_h) {
        return false;
    }
    if (glyph.bpp == 1) {
        const size_t bit = glyph.stride == 0
                               ? static_cast<size_t>(y) * glyph.box_w + x
                               : static_cast<size_t>(y) * glyph.stride * 8 + x;
        return (glyph.bitmap[bit / 8] & (0x80U >> (bit & 7))) != 0;
    }
    const size_t pixel = static_cast<size_t>(y) * glyph.box_w + x;
    const uint8_t packed = glyph.bitmap[pixel / 2];
    const uint8_t value = (pixel & 1) == 0 ? packed >> 4 : packed & 0x0f;
    return value >= 8;
}

uint32_t DecodeUtf8(const std::string& text, size_t& offset) {
    const auto first = static_cast<uint8_t>(text[offset++]);
    if (first < 0x80) {
        return first;
    }
    int continuation_count = 0;
    uint32_t codepoint = 0;
    if ((first & 0xe0) == 0xc0) {
        continuation_count = 1;
        codepoint = first & 0x1f;
    } else if ((first & 0xf0) == 0xe0) {
        continuation_count = 2;
        codepoint = first & 0x0f;
    } else if ((first & 0xf8) == 0xf0) {
        continuation_count = 3;
        codepoint = first & 0x07;
    } else {
        return '?';
    }
    if (offset + continuation_count > text.size()) {
        offset = text.size();
        return '?';
    }
    for (int index = 0; index < continuation_count; ++index) {
        const auto continuation = static_cast<uint8_t>(text[offset]);
        if ((continuation & 0xc0) != 0x80) {
            return '?';
        }
        ++offset;
        codepoint = (codepoint << 6) | (continuation & 0x3f);
    }
    return codepoint;
}

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

bool SecondaryOled::HasNonAscii(const std::string& text) {
    return std::any_of(text.begin(), text.end(),
                       [](unsigned char character) { return character >= 0x80; });
}

int SecondaryOled::MeasureNotoTextWidth(const std::string& text) {
    int width = 0;
    for (size_t offset = 0; offset < text.size();) {
        uint32_t codepoint = DecodeUtf8(text, offset);
        NotoGlyph glyph;
        if (!ResolveNotoGlyph(codepoint, glyph) && !ResolveNotoGlyph('?', glyph)) {
            continue;
        }
        width += glyph.advance;
    }
    return width;
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

void SecondaryOled::DrawNotoText(int x, int y, const std::string& text, int max_width) {
    const int right = x + max_width;
    for (size_t offset = 0; offset < text.size();) {
        uint32_t codepoint = DecodeUtf8(text, offset);
        NotoGlyph glyph;
        if (!ResolveNotoGlyph(codepoint, glyph) && !ResolveNotoGlyph('?', glyph)) {
            continue;
        }
        if (x + glyph.advance > right) {
            break;
        }

        const int source_top = glyph.line_height - glyph.base_line - glyph.box_h - glyph.ofs_y;
        const int output_top = ScaleRounded(source_top, glyph.scale_numerator,
                                            glyph.scale_denominator);
        const int output_x = x + ScaleRounded(glyph.ofs_x, glyph.scale_numerator,
                                              glyph.scale_denominator);
        const int output_width = std::max(
            0, ScaleRounded(glyph.box_w, glyph.scale_numerator, glyph.scale_denominator));
        const int output_height = std::max(
            0, ScaleRounded(glyph.box_h, glyph.scale_numerator, glyph.scale_denominator));
        for (int row = 0; row < output_height; ++row) {
            const int source_row = std::min<int>(
                glyph.box_h - 1, row * glyph.scale_denominator / glyph.scale_numerator);
            for (int column = 0; column < output_width; ++column) {
                const int source_column = std::min<int>(
                    glyph.box_w - 1, column * glyph.scale_denominator / glyph.scale_numerator);
                if (NotoGlyphPixel(glyph, source_column, source_row)) {
                    SetPixel(output_x + column, y + output_top + row);
                }
            }
        }
        x += glyph.advance;
    }
}

void SecondaryOled::DrawNotoTextFitted(int x, int y, int width, int height,
                                       const std::string& text) {
    if (width <= 0 || height < kNotoLineHeight || text.empty()) {
        return;
    }

    struct Character {
        size_t begin = 0;
        size_t end = 0;
        int width = 0;
        bool whitespace = false;
    };
    // MCP text is capped at 48 code points; keep this scratch storage bounded on the
    // PSRAM-backed OLED task stack.
    std::array<Character, 48> characters = {};
    size_t character_count = 0;
    for (size_t offset = 0; offset < text.size() && character_count < characters.size();) {
        const size_t begin = offset;
        const uint32_t codepoint = DecodeUtf8(text, offset);
        NotoGlyph glyph;
        if (!ResolveNotoGlyph(codepoint, glyph) && !ResolveNotoGlyph('?', glyph)) {
            continue;
        }
        characters[character_count++] = {
            .begin = begin,
            .end = offset,
            .width = glyph.advance,
            .whitespace = codepoint == ' ',
        };
    }
    if (character_count == 0) {
        return;
    }

    struct Line {
        size_t begin = 0;
        size_t end = 0;
        int width = 0;
        bool ellipsis = false;
    };
    std::array<Line, 2> lines = {};
    const size_t maximum_lines = std::min<size_t>(lines.size(), height / kNotoLineHeight);
    size_t line_count = 0;
    size_t cursor = 0;
    while (cursor < character_count && line_count < maximum_lines) {
        while (cursor < character_count && characters[cursor].whitespace) {
            ++cursor;
        }
        if (cursor >= character_count) {
            break;
        }
        const size_t line_begin = cursor;
        size_t last_space = character_count;
        int used_width = 0;
        while (cursor < character_count &&
               (used_width + characters[cursor].width <= width || cursor == line_begin)) {
            used_width += characters[cursor].width;
            if (characters[cursor].whitespace) {
                last_space = cursor;
            }
            ++cursor;
        }
        size_t line_end = cursor;
        if (cursor < character_count && last_space != character_count && last_space > line_begin) {
            line_end = last_space;
            cursor = last_space + 1;
        }
        while (line_end > line_begin && characters[line_end - 1].whitespace) {
            --line_end;
        }
        int line_width = 0;
        for (size_t index = line_begin; index < line_end; ++index) {
            line_width += characters[index].width;
        }
        lines[line_count++] = {.begin = line_begin, .end = line_end, .width = line_width};
    }

    if (cursor < character_count && line_count > 0) {
        Line& last = lines[line_count - 1];
        const int ellipsis_width = 3 * MeasureNotoTextWidth(".");
        while (last.end > last.begin && last.width + ellipsis_width > width) {
            --last.end;
            last.width -= characters[last.end].width;
        }
        last.width += ellipsis_width;
        last.ellipsis = true;
    }

    const int top = y + std::max(0, (height - static_cast<int>(line_count) * kNotoLineHeight) / 2);
    for (size_t line_index = 0; line_index < line_count; ++line_index) {
        const Line& line = lines[line_index];
        std::string line_text;
        if (line.end > line.begin) {
            const size_t byte_begin = characters[line.begin].begin;
            const size_t byte_end = characters[line.end - 1].end;
            line_text = text.substr(byte_begin, byte_end - byte_begin);
        }
        if (line.ellipsis) {
            line_text += "...";
        }
        const int draw_x = x + std::max(0, (width - line.width) / 2);
        DrawNotoText(draw_x, top + static_cast<int>(line_index) * kNotoLineHeight, line_text,
                     width - (draw_x - x));
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
