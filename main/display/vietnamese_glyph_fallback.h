#pragma once

#include "text_glyph.h"

#include <vector>

// Vietnamese glyphs missing from the xiaozhi-fonts basic 20px/4bpp tier. Keeping this
// small fallback on-device makes Vietnamese rendering independent of whether
// the active server attaches a glyph_push payload.
std::vector<TextGlyph> CreateVietnameseGlyphFallback();
