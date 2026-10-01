#!/usr/bin/env python3
"""Create the project-owned 16px/4bpp Mochan Latin/Vietnamese font source.

Requires Pillow. Pass the official Noto Sans Regular TTF (the
variable font's default 400 weight also works). This does not run a firmware
build or write managed components. Keep the source font's OFL alongside output.
"""

import argparse
import hashlib
import struct
from pathlib import Path

from PIL import ImageFont


def missing_codepoints(data, codepoints):
    """Read SFNT cmap coverage without installing a separate font compiler."""
    for index in range(struct.unpack_from(">H", data, 4)[0]):
        tag, _, offset, _ = struct.unpack_from(">4sIII", data, 12 + index * 16)
        if tag == b"cmap":
            cmap = offset
            break
    else:
        raise ValueError("Source font has no cmap")
    supported = set()
    for index in range(struct.unpack_from(">H", data, cmap + 2)[0]):
        platform, _, offset = struct.unpack_from(">HHI", data, cmap + 4 + index * 8)
        if platform not in (0, 3):
            continue
        table = cmap + offset
        kind = struct.unpack_from(">H", data, table)[0]
        if kind == 12:
            for group in range(struct.unpack_from(">I", data, table + 12)[0]):
                start, end, glyph = struct.unpack_from(">III", data, table + 16 + group * 12)
                supported.update(cp for cp in codepoints if start <= cp <= end and glyph + cp - start)
        elif kind == 4:
            segments = struct.unpack_from(">H", data, table + 6)[0] // 2
            ends = table + 14
            starts = ends + segments * 2 + 2
            deltas = starts + segments * 2
            ranges = deltas + segments * 2
            for segment in range(segments):
                start = struct.unpack_from(">H", data, starts + segment * 2)[0]
                end = struct.unpack_from(">H", data, ends + segment * 2)[0]
                delta = struct.unpack_from(">h", data, deltas + segment * 2)[0]
                address = ranges + segment * 2
                relative = struct.unpack_from(">H", data, address)[0]
                for cp in codepoints:
                    if not start <= cp <= end:
                        continue
                    glyph = struct.unpack_from(">H", data, address + relative + 2 * (cp - start))[0] if relative else cp
                    if glyph and (glyph + delta) & 0xFFFF:
                        supported.add(cp)
    return codepoints - supported


def generate(font_path, output):
    codepoints = set(range(0x20, 0x7F)) | set(range(0xA0, 0x100))
    codepoints |= {0x102, 0x103, 0x110, 0x111, 0x128, 0x129,
                   0x168, 0x169, 0x1A0, 0x1A1, 0x1AF, 0x1B0}
    codepoints |= set(range(0x1EA0, 0x1EFA))
    codepoints |= {0x300, 0x301, 0x302, 0x303, 0x306, 0x309, 0x31B, 0x323,
                   0x2010, 0x2011, 0x2013, 0x2014, 0x2018, 0x2019,
                   0x201C, 0x201D, 0x2022, 0x2026}
    missing = missing_codepoints(font_path.read_bytes(), codepoints)
    if missing:
        raise ValueError(f"Source font lacks glyphs: {sorted(missing)}")

    font = ImageFont.truetype(str(font_path), 16)
    ascent, descent = font.getmetrics()
    bitmap = bytearray()
    glyphs = ["    {0, 0, 0, 0, 0, 0},"]
    codepoints = sorted(codepoints)
    for codepoint in codepoints:
        mask, offset = font.getmask2(chr(codepoint), mode="L", anchor="ls")
        width, height = mask.size
        pixels = list(mask)
        packed = bytearray((len(pixels) + 1) // 2)
        for index, alpha in enumerate(pixels):
            value = (alpha * 15 + 127) // 255
            packed[index // 2] |= value << (4 if index % 2 == 0 else 0)
        advance = round(font.getlength(chr(codepoint)) * 16)
        offset_y = -offset[1] - height
        # The nominal metrics can clip stacked Vietnamese capital accents.
        # Include every subset glyph in the label's line box.
        ascent = max(ascent, height + offset_y)
        descent = max(descent, -offset_y)
        glyphs.append(
            f"    {{{len(bitmap)}, {advance}, {width}, {height}, "
            f"{offset[0]}, {offset_y}}}, // U+{codepoint:04X}"
        )
        bitmap.extend(packed)

    first = codepoints[0]
    lines = [
        "// Mochan Response, native 16px, 4bpp; Latin/Vietnamese subset of Noto Sans.",
        "// Created by scripts/generate_mochan_response_font.py; SIL OFL:",
        "// fonts/OFL-NotoSans.txt. Other scripts and managed font output are unchanged.",
        f"// Source TTF SHA256: {hashlib.sha256(font_path.read_bytes()).hexdigest()}",
        '#include <lvgl.h>', "", "namespace {", "const uint8_t kBitmap[] = {",
    ]
    for start in range(0, len(bitmap), 16):
        lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in bitmap[start:start + 16]) + ",")
    lines += ["};", "const lv_font_fmt_txt_glyph_dsc_t kGlyphs[] = {", *glyphs, "};",
              "const uint16_t kUnicode[] = {"]
    for start in range(0, len(codepoints), 12):
        lines.append("    " + ", ".join(str(cp - first) for cp in codepoints[start:start + 12]) + ",")
    lines += [
        "};", "const lv_font_fmt_txt_cmap_t kMap = {",
        f"    .range_start = {first}, .range_length = {codepoints[-1] - first + 1}, .glyph_id_start = 1,",
        "    .unicode_list = kUnicode, .glyph_id_ofs_list = nullptr,",
        f"    .list_length = {len(codepoints)}, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY,",
        "};", "const lv_font_fmt_txt_dsc_t kDescriptor = {",
        "    .glyph_bitmap = kBitmap, .glyph_dsc = kGlyphs, .cmaps = &kMap,",
        "    .kern_dsc = nullptr, .kern_scale = 0, .cmap_num = 1, .bpp = 4,",
        "    .kern_classes = 0, .bitmap_format = 0,",
        "};", "}  // namespace", "",
        "extern const lv_font_t mochan_response_font_16 = {",
        "    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,",
        "    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,",
        f"    .line_height = {ascent + descent}, .base_line = {descent}, .dsc = &kDescriptor,",
        "};", "",
    ]
    output.write_text("\n".join(lines))
    print(f"Created {output}: {len(codepoints)} glyphs, {len(bitmap)} bitmap bytes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("font", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    generate(args.font, args.output)
