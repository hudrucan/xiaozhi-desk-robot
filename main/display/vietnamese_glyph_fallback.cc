#include "vietnamese_glyph_fallback.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>

namespace {

struct VietnameseGlyphSeed {
    uint32_t codepoint;
    uint32_t adv_w;
    uint16_t box_w;
    uint16_t box_h;
    int16_t ofs_x;
    int16_t ofs_y;
    size_t bitmap_offset;
    size_t bitmap_size;
};

#include "vietnamese_glyph_fallback.inc"

}  // namespace

std::vector<TextGlyph> CreateVietnameseGlyphFallback() {
    std::vector<TextGlyph> result;
    result.reserve(std::size(kVietnameseFallbackGlyphs));
    for (const auto& seed : kVietnameseFallbackGlyphs) {
        TextGlyph glyph;
        glyph.codepoint = seed.codepoint;
        glyph.adv_w = seed.adv_w;
        glyph.box_w = seed.box_w;
        glyph.box_h = seed.box_h;
        glyph.ofs_x = seed.ofs_x;
        glyph.ofs_y = seed.ofs_y;
        glyph.bitmap.assign(kVietnameseFallbackBitmap + seed.bitmap_offset,
                            kVietnameseFallbackBitmap + seed.bitmap_offset + seed.bitmap_size);
        result.push_back(std::move(glyph));
    }
    return result;
}
