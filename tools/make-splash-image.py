"""Writes SplashImage.h, the Deki logo the splash screen shows (Splash.cpp),
from deki-brand's device/deki-splash-320x240.png: the part that is not black,
as RGB565 for drawing on the black screen.

    python tools/make-splash-image.py <deki-brand>/device/deki-splash-320x240.png

Needs Pillow.
"""

import pathlib
import sys

from PIL import Image

image = Image.open(sys.argv[1]).convert("RGB")
left, top, right, bottom = image.point(lambda v: 255 if v > 8 else 0).getbbox()
logo = image.crop((left, top, right, bottom))
width, height = logo.size

data = logo.tobytes()
words = []
for i in range(0, len(data), 3):
    r, g, b = data[i], data[i + 1], data[i + 2]
    words.append(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))

rows = []
for i in range(0, len(words), 14):
    rows.append("    " + ", ".join(f"0x{w:04X}" for w in words[i:i + 14]) + ",")

out = pathlib.Path(__file__).resolve().parent.parent / "SplashImage.h"
out.write_text(f"""#pragma once

// The Deki logo for the splash screen (Splash.cpp): the icon and DEKI from
// deki-brand's device/deki-splash-320x240.png, cut to what is not black, in
// RGB565. Made by tools/make-splash-image.py; edit that and run it again.

#include <cstdint>

namespace DekiRendering::SplashImage
{{

constexpr int kWidth = {width};
constexpr int kHeight = {height};

// The picture it was cut from: the logo is drawn at 1x up to twice this size.
constexpr int kDesignWidth = {image.width};
constexpr int kDesignHeight = {image.height};

alignas(4) inline constexpr uint16_t kPixels[kWidth * kHeight] = {{
{chr(10).join(rows)}
}};

}}  // namespace DekiRendering::SplashImage
""", encoding="utf-8", newline="\n")
print(out, width, height, len(words) * 2, "bytes")
