#pragma once

#include "text_glyph.h"

#include <cstddef>
#include <cstdint>
#include <vector>

struct VietnameseGlyphView {
    uint32_t adv_w = 0;
    uint16_t box_w = 0;
    uint16_t box_h = 0;
    int16_t ofs_x = 0;
    int16_t ofs_y = 0;
    const uint8_t* bitmap = nullptr;
    size_t bitmap_size = 0;
};

// Vietnamese glyphs missing from the xiaozhi-fonts basic 20px/4bpp tier. Keeping this
// small fallback on-device makes Vietnamese rendering independent of whether
// the active server attaches a glyph_push payload.
std::vector<TextGlyph> CreateVietnameseGlyphFallback();

// Zero-copy access for the 1-bit secondary OLED renderer. The source bitmap is the same
// 20px/4bpp flash-resident table used to seed the main display's dynamic fallback.
bool FindVietnameseGlyphFallback(uint32_t codepoint, VietnameseGlyphView& glyph);
