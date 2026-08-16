"""Render the clock face's digits into a flash-resident 1bpp header.

Clock mode draws with GfxRenderer::drawImage, not with a font: the glyphs it
needs are eleven ASCII characters that never change, and going through the font
path would mean either an SD-card font (the timer-wake path never mounts the
card) or a built-in font capped at 18 pt, which is far too small for a clock
read across a room.

Output format matches FreeInkDisplay::blitImage: one bit per pixel, MSB is the
leftmost pixel, each row padded to a whole number of bytes, and a 0 bit is black
ink. That is the same convention as src/images/Logo120.h.

drawImage rotates only the origin, never the bits (see the TODO in
GfxRenderer::drawImage), so the rotation has to be baked in here. --rotate takes
the quarter-turns to apply; regenerate with a different value if the face comes
out sideways on the device.

Usage:
  python3 scripts/build_clock_digits.py [--font PATH] [--large N] [--small N]
                                        [--rotate 0|1|2|3] [--out PATH]
"""

import argparse
import os
import sys

try:
    import freetype
except ImportError:
    sys.exit("freetype-py is required: pip install freetype-py")

# The clock face draws dates and times, so these eleven and nothing else.
GLYPHS = "0123456789:/"

# C identifiers for characters that cannot appear in one.
NAMES = {":": "Colon", "/": "Slash"}


def glyph_name(ch):
    return NAMES.get(ch, f"Digit{ch}")


def render(face, ch, pixel_size):
    """Render one character to a (width, height, rows-of-bools) bitmap, ink=True."""
    face.set_pixel_sizes(0, pixel_size)
    face.load_char(ch, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO)
    bitmap = face.glyph.bitmap
    width, rows, pitch = bitmap.width, bitmap.rows, bitmap.pitch

    # FT_RENDER_MODE_MONO packs 8 pixels per byte, MSB first, with a set bit
    # meaning ink -- the opposite polarity to the output, inverted below.
    pixels = [[False] * width for _ in range(rows)]
    for y in range(rows):
        for x in range(width):
            byte = bitmap.buffer[y * pitch + (x >> 3)]
            pixels[y][x] = bool(byte & (0x80 >> (x & 7)))
    return width, rows, pixels


def pad_to_common_box(glyphs, pixel_size):
    """Put every glyph on one baseline in one box, so the caller can lay them out
    by advancing a fixed pitch instead of tracking per-glyph metrics on device."""
    max_ascent = max(g["bearing_y"] for g in glyphs)
    max_descent = max(g["rows"] - g["bearing_y"] for g in glyphs)
    box_h = max_ascent + max_descent
    box_w = max(g["width"] for g in glyphs)

    for g in glyphs:
        padded = [[False] * box_w for _ in range(box_h)]
        top = max_ascent - g["bearing_y"]
        left = (box_w - g["width"]) // 2  # centred: digits read as a column of equal cells
        for y in range(g["rows"]):
            for x in range(g["width"]):
                if g["pixels"][y][x]:
                    padded[top + y][left + x] = True
        g["pixels"] = padded
        g["width"] = box_w
        g["rows"] = box_h
    return box_w, box_h


def rotate(pixels, width, height, quarter_turns):
    """Rotate clockwise by `quarter_turns` 90-degree steps."""
    for _ in range(quarter_turns % 4):
        rotated = [[False] * height for _ in range(width)]
        for y in range(height):
            for x in range(width):
                rotated[x][height - 1 - y] = pixels[y][x]
        pixels = rotated
        width, height = height, width
    return pixels, width, height


def pack(pixels, width, height):
    """1bpp, MSB first, rows padded to whole bytes, 0 = black."""
    row_bytes = (width + 7) // 8
    out = bytearray()
    for y in range(height):
        row = bytearray(b"\xff" * row_bytes)  # start white
        for x in range(width):
            if pixels[y][x]:
                row[x >> 3] &= ~(0x80 >> (x & 7)) & 0xFF
        out += row
    return bytes(out)


def emit_array(name, data, width, height):
    lines = [f"// {name}: {width}x{height}, 1bpp MSB-first, 0 = black",
             f"static const uint8_t {name}[] = {{"]
    row_bytes = (width + 7) // 8
    for i in range(0, len(data), row_bytes):
        chunk = data[i:i + row_bytes]
        lines.append("    " + " ".join(f"0x{b:02x}," for b in chunk))
    lines.append("};")
    return "\n".join(lines)


def build_set(face, pixel_size, prefix, rotate_turns):
    glyphs = []
    for ch in GLYPHS:
        width, rows, pixels = render(face, ch, pixel_size)
        glyphs.append({
            "ch": ch,
            "width": width,
            "rows": rows,
            "pixels": pixels,
            "bearing_y": face.glyph.bitmap_top,
        })
    box_w, box_h = pad_to_common_box(glyphs, pixel_size)

    blocks = []
    for g in glyphs:
        pixels, w, h = rotate(g["pixels"], g["width"], g["rows"], rotate_turns)
        name = f"{prefix}{glyph_name(g['ch'])}"
        blocks.append(emit_array(name, pack(pixels, w, h), w, h))
        g["out_w"], g["out_h"] = w, h

    out_w, out_h = glyphs[0]["out_w"], glyphs[0]["out_h"]
    table = [f"// Indexed by ClockGlyph. Every cell is {out_w}x{out_h}, so the face advances a fixed pitch.",
             f"static const uint8_t* const {prefix}Glyphs[] = {{"]
    table += [f"    {prefix}{glyph_name(ch)}," for ch in GLYPHS]
    table.append("};")

    header = [
        f"#define {prefix.upper()}_WIDTH {out_w}",
        f"#define {prefix.upper()}_HEIGHT {out_h}",
        f"// Unrotated cell, for laying the face out in logical coordinates.",
        f"#define {prefix.upper()}_CELL_W {box_w}",
        f"#define {prefix.upper()}_CELL_H {box_h}",
    ]
    return "\n".join(header) + "\n\n" + "\n\n".join(blocks) + "\n\n" + "\n".join(table)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", default="/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf")
    ap.add_argument("--large", type=int, default=150, help="pixel size for the time")
    ap.add_argument("--small", type=int, default=56, help="pixel size for the date")
    ap.add_argument("--rotate", type=int, default=0,
                    help="quarter turns clockwise baked into the bitmaps (drawImage does not rotate bits)")
    ap.add_argument("--out", default="src/images/ClockDigits.h")
    args = ap.parse_args()

    if not os.path.exists(args.font):
        sys.exit(f"font not found: {args.font}")
    face = freetype.Face(args.font)

    parts = [
        "#pragma once",
        "#include <cstdint>",
        "",
        "// Generated by scripts/build_clock_digits.py -- do not edit.",
        f"// font={os.path.basename(args.font)} large={args.large} small={args.small} rotate={args.rotate}",
        "//",
        "// Clock mode's glyphs, drawn with GfxRenderer::drawImage rather than a font: the",
        "// timer-wake path never mounts the SD card, and the built-in fonts stop at 18 pt.",
        "// The rotation is baked in because drawImage rotates the origin but not the bits.",
        "",
        "// Order matches ClockGlyph in ClockFace.h.",
        "",
        build_set(face, args.large, "ClockLarge", args.rotate),
        "",
        build_set(face, args.small, "ClockSmall", args.rotate),
        "",
    ]
    text = "\n".join(parts)

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(text)

    size = len(text.encode())
    print(f"{args.out}: {size} bytes of source")


if __name__ == "__main__":
    main()
