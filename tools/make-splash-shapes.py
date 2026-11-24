"""Writes SplashShapes.h, the DEKI letters the splash screen draws (Splash.cpp),
from deki-brand's device/deki-splash-320x240.png.

Each letter is a mask of its white face, two bits a pixel (how much of the
pixel it covers, 0 to 3), so the splash can move the letters one by one. Their
blue side is not stored: Splash.cpp draws it from the mask. The icon above
them is not here either: Splash.cpp draws it from the brand's shapes.

    python tools/make-splash-shapes.py <deki-brand>/device/deki-splash-320x240.png

Needs Pillow.
"""

import math
import pathlib
import sys

from PIL import Image

image = Image.open(sys.argv[1]).convert("RGB")
left, top, right, bottom = image.point(lambda v: 255 if v > 8 else 0).getbbox()
logo = image.crop((left, top, right, bottom))
width, height = logo.size
pixels = logo.load()


def face(x, y):
    """How much of the pixel is the letters' white face, 0 to 3."""
    r, g, b = pixels[x, y]
    white = max(0, min(255, (min(r, g) - 40) * 255 // 190))
    return round(white / 85)


cover = [[face(x, y) for x in range(width)] for y in range(height)]
rows = [y for y in range(height) if any(cover[y])]
first, last = rows[0], rows[-1]

# The letters lean 10 degrees: undo that, then the three widest gaps between
# lit columns are where one letter ends and the next begins.
lean = math.tan(math.radians(10))


def upright(x, y):
    return x - (last - y) * lean


columns = {}
for y in range(first, last + 1):
    for x in range(width):
        if cover[y][x] >= 2:
            columns[round(upright(x, y))] = True
lit = sorted(columns)
gaps = sorted(((lit[i + 1] - lit[i], (lit[i] + lit[i + 1]) / 2) for i in range(len(lit) - 1)), reverse=True)
cuts = sorted(middle for _, middle in gaps[:3])


def letter_of(x, y):
    return sum(1 for c in cuts if upright(x, y) > c)


letters = []
bits = []
for index in range(4):
    mine = [(x, y) for y in range(first, last + 1) for x in range(width) if cover[y][x] and letter_of(x, y) == index]
    x0 = min(x for x, _ in mine)
    y0 = min(y for _, y in mine)
    x1 = max(x for x, _ in mine) + 1
    y1 = max(y for _, y in mine) + 1
    owned = set(mine)
    offset = len(bits)
    for y in range(y0, y1):
        for x in range(x0, x1):
            bits.append(cover[y][x] if (x, y) in owned else 0)
    letters.append((x0, y0, x1 - x0, y1 - y0, offset))

packed = []
for i in range(0, len(bits), 4):
    group = bits[i:i + 4] + [0] * (4 - len(bits[i:i + 4]))
    packed.append(group[0] | group[1] << 2 | group[2] << 4 | group[3] << 6)

# 19 values a line: what clang-format makes of it, so formatting leaves it alone.
lines = []
for i in range(0, len(packed), 19):
    lines.append("    " + ", ".join(f"0x{b:02X}" for b in packed[i:i + 19]) + ",")

entries = "\n".join(f"    {{ {x}, {y}, {w}, {h}, {o} }}," for x, y, w, h, o in letters)
out = pathlib.Path(__file__).resolve().parent.parent / "SplashShapes.h"
out.write_text(f"""#pragma once

// The DEKI letters for the splash screen (Splash.cpp), from deki-brand's
// device/deki-splash-320x240.png: each letter's white face, two bits a pixel
// (0 to 3, how much of the pixel it covers), so they can move one by one.
// Made by tools/make-splash-shapes.py; edit that and run it again.

#include <cstdint>

namespace DekiRendering::SplashShapes
{{

/// The logo's box, the icon at the top and the letters at the bottom, in the
/// picture's pixels: what the splash draws at 1x.
constexpr int kWidth = {width};
constexpr int kHeight = {height};

/// The picture the logo was cut from: drawn at 1x up to twice this size.
constexpr int kDesignWidth = {image.width};
constexpr int kDesignHeight = {image.height};

/// A letter: where its mask sits in the logo's box, its size, and where its
/// pixels start in kLetterBits (counted in pixels, four to a byte, low bits
/// first).
struct Letter
{{
    int x;
    int y;
    int width;
    int height;
    int first;
}};

inline constexpr Letter kLetters[4] = {{
{entries}
}};

inline constexpr uint8_t kLetterBits[] = {{
{chr(10).join(lines)}
}};

/// How much of the letter's pixel (x, y) its face covers, 0 to 3.
constexpr int Cover(const Letter& letter, int x, int y)
{{
    const int at = letter.first + y * letter.width + x;
    return (kLetterBits[at / 4] >> (at % 4 * 2)) & 3;
}}

}}  // namespace DekiRendering::SplashShapes
""", encoding="utf-8", newline="\n")
print(out, width, height, len(packed), "bytes", letters, "cuts", cuts)
