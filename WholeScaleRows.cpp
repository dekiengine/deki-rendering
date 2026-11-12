#include "WholeScaleRows.h"

#include <algorithm>

#include "PixelFormat.h"

namespace QuadBlit
{

namespace
{

using DekiPixel::Div255;
using DekiPixel::SrcKind;

// One target row of a source row drawn at scaleX: columns [startX, endX).
template <SrcKind SK>
void WholeScaleRow(const uint8_t* srcRow, int32_t bpp, bool hasAlpha, int32_t scaleX, uint32_t* row, int32_t destX,
                   int32_t startX, int32_t endX)
{
    int32_t sx = (startX - destX) / scaleX;
    int32_t x = startX;
    while (x < endX)
    {
        // The target columns source column sx covers, inside the clip.
        const int32_t blockEnd = std::min(endX, destX + (sx + 1) * scaleX);
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
        DekiPixel::ReadSrcPixel<SK>(srcRow + static_cast<size_t>(sx) * bpp, hasAlpha, r, g, b, a);
        if (a == 255)
        {
            const uint32_t opaque =
                (0xFFu << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
            for (; x < blockEnd; ++x)
            {
                row[x] = opaque;
            }
        }
        else if (a != 0)
        {
            // As the pipeline's plain composite: the target keeps no alpha.
            const uint32_t invA = 255u - a;
            for (; x < blockEnd; ++x)
            {
                const uint32_t v = row[x];
                const uint32_t bgR = (v >> 16) & 0xFF;
                const uint32_t bgG = (v >> 8) & 0xFF;
                const uint32_t bgB = v & 0xFF;
                row[x] = (0xFFu << 24) | (static_cast<uint32_t>(Div255(r * a + bgR * invA)) << 16) |
                         (static_cast<uint32_t>(Div255(g * a + bgG * invA)) << 8) | Div255(b * a + bgB * invA);
            }
        }
        x = blockEnd;
        ++sx;
    }
}

template <SrcKind SK>
void WholeScaleRows(const Source& source, uint32_t* target, int32_t targetWidth, int32_t destX, int32_t destY,
                    int32_t scaleX, int32_t scaleY, int32_t startX, int32_t startY, int32_t endX, int32_t endY)
{
    const int32_t bpp = source.bytesPerPixel;
    const int32_t stride = source.stride > 0 ? source.stride : source.width * bpp;
    for (int32_t y = startY; y < endY; ++y)
    {
        const uint8_t* srcRow = source.pixels + static_cast<size_t>((y - destY) / scaleY) * stride;
        WholeScaleRow<SK>(srcRow, bpp, source.hasAlpha, scaleX, target + static_cast<size_t>(y) * targetWidth, destX,
                          startX, endX);
    }
}

SrcKind KindOfSource(const Source& s)
{
    if (s.isRGB565)
    {
        return s.bytesPerPixel >= 3 ? SrcKind::RGB565A8 : SrcKind::RGB565;
    }
    if (s.bytesPerPixel == 4)
    {
        return SrcKind::RGBA8888;
    }
    if (s.bytesPerPixel == 3)
    {
        return SrcKind::RGB888;
    }
    return SrcKind::ALPHA8;
}

}  // namespace

bool WholeScaleRows_ARGB8888(const Source& source, uint32_t* target, int32_t targetWidth, int32_t destX, int32_t destY,
                             int32_t destWidth, int32_t destHeight, int32_t startX, int32_t startY, int32_t endX,
                             int32_t endY)
{
    const int32_t scaleX = WholeScale(source.width, destWidth);
    const int32_t scaleY = WholeScale(source.height, destHeight);
    if (scaleX == 0 || scaleY == 0)
    {
        return false;
    }
    switch (KindOfSource(source))
    {
        case SrcKind::RGBA8888:
            WholeScaleRows<SrcKind::RGBA8888>(source, target, targetWidth, destX, destY, scaleX, scaleY, startX, startY,
                                              endX, endY);
            break;
        case SrcKind::RGB888:
            WholeScaleRows<SrcKind::RGB888>(source, target, targetWidth, destX, destY, scaleX, scaleY, startX, startY,
                                            endX, endY);
            break;
        case SrcKind::RGB565:
            WholeScaleRows<SrcKind::RGB565>(source, target, targetWidth, destX, destY, scaleX, scaleY, startX, startY,
                                            endX, endY);
            break;
        case SrcKind::RGB565A8:
            WholeScaleRows<SrcKind::RGB565A8>(source, target, targetWidth, destX, destY, scaleX, scaleY, startX, startY,
                                              endX, endY);
            break;
        case SrcKind::ALPHA8:
            WholeScaleRows<SrcKind::ALPHA8>(source, target, targetWidth, destX, destY, scaleX, scaleY, startX, startY,
                                            endX, endY);
            break;
    }
    return true;
}

}  // namespace QuadBlit
