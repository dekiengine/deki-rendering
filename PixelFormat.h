/**
 * @file PixelFormat.h
 * @brief The one place for pixel-format arithmetic shared by the rasterizer
 *        and the packages that prepare data for it: RGB565 pack/unpack and
 *        quantisation, exact /255, the ordered-dither threshold matrix.
 *
 * RGB565 -> 8-bit expansion is by shift (no bit replication): 0x1F -> 0xF8.
 * That is the engine's convention everywhere a 565 value meets 8-bit maths
 * (blending, tinting, chroma keys), so a key quantised with QuantizeRGB565
 * compares equal to a pixel unpacked with UnpackRGB565.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <deki/Engine.h>  // Deki::ColorFormat

namespace DekiPixel
{

/// x / 255 for x in [0, 65535], exact. (x + 128) >> 8 lost the top value:
/// 255 * 255 came out as 254, so repeated tints and blends drifted darker.
inline constexpr uint8_t Div255(uint32_t x)
{
    return static_cast<uint8_t>((x + 1 + (x >> 8)) >> 8);
}

inline constexpr uint16_t PackRGB565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

inline void UnpackRGB565(uint16_t v, uint8_t& r, uint8_t& g, uint8_t& b)
{
    r = static_cast<uint8_t>((v >> 11) << 3);
    g = static_cast<uint8_t>(((v >> 5) & 0x3F) << 2);
    b = static_cast<uint8_t>((v & 0x1F) << 3);
}

/// Drop the bits RGB565 cannot hold, so an 8-bit colour compares equal to the
/// same colour after a round trip through PackRGB565/UnpackRGB565.
inline void QuantizeRGB565(uint8_t& r, uint8_t& g, uint8_t& b)
{
    r = static_cast<uint8_t>((r >> 3) << 3);
    g = static_cast<uint8_t>((g >> 2) << 2);
    b = static_cast<uint8_t>((b >> 3) << 3);
}

/// Standard src-over alpha union for coverage targets (RGB565A8):
/// out.a = src.a + dst.a * (255 - src.a) / 255. On a freshly cleared target
/// (dst.a == 0) this is src.a, which is what "is covered" consumers expect.
inline uint8_t AlphaUnion(uint8_t srcA, uint8_t dstA)
{
    if (srcA == 255) return 255;
    if (dstA == 0) return srcA;
    return static_cast<uint8_t>(srcA + Div255(static_cast<uint32_t>(dstA) * (255u - srcA)));
}

/// 8x8 Bayer threshold matrix scaled to 0..255 (the recurrent definition,
/// re-mapped to (m + 1) * 256 / 64 - 1). Ordered dithering writes a pixel
/// opaquely when its alpha exceeds the threshold at (x & 7, y & 7).
/// https://en.wikipedia.org/wiki/Ordered_dithering
inline constexpr uint8_t kBayer8x8[64] = {
      0, 128,  32, 160,   8, 136,  40, 168,
    192,  64, 224,  96, 200,  72, 232, 104,
     48, 176,  16, 144,  56, 184,  24, 152,
    240, 112, 208,  80, 248, 120, 216,  88,
     12, 140,  44, 172,   4, 132,  36, 164,
    204,  76, 236, 108, 196,  68, 228, 100,
     60, 188,  28, 156,  52, 180,  20, 148,
    252, 124, 220,  92, 244, 116, 212,  84,
};

inline uint8_t BayerThreshold(int32_t px, int32_t py)
{
    return kBayer8x8[((py & 7) << 3) | (px & 7)];
}

// ---------------------------------------------------------------------------
// Per-format pixel access, shared by the 2D blitter (QuadBlit) and deki-3d's
// rasteriser so both read and write every format the same way. Templates, so
// each format compiles to its own tight loop.
// ---------------------------------------------------------------------------

// Source layouts. RGB565A8 is any isRGB565 source with 3+ bytes per pixel,
// whether or not it declares alpha (hasAlpha decides whether byte 2 is read);
// RGBA8888 is the 4-byte non-565 layout, RGB888 3 bytes, ALPHA8 a
// coverage-only byte (a font/icon atlas drawn as a sprite: its colour is the
// tint, white when untinted).
enum class SrcKind { RGB565, RGB565A8, RGBA8888, RGB888, ALPHA8 };

template <SrcKind SK>
inline void ReadSrcPixel(const uint8_t* p, bool hasAlpha, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a)
{
    if constexpr (SK == SrcKind::RGB565 || SK == SrcKind::RGB565A8)
    {
        uint16_t v;
        memcpy(&v, p, 2);
        UnpackRGB565(v, r, g, b);
        if constexpr (SK == SrcKind::RGB565A8)
            a = hasAlpha ? p[2] : 255;
        else
            a = 255;
    }
    else if constexpr (SK == SrcKind::RGBA8888)
    {
        r = p[0]; g = p[1]; b = p[2]; a = p[3];
        (void)hasAlpha;
    }
    else if constexpr (SK == SrcKind::RGB888)
    {
        r = p[0]; g = p[1]; b = p[2]; a = 255;
        (void)hasAlpha;
    }
    else
    {
        r = g = b = 255;
        a = p[0];
        (void)hasAlpha;
    }
}

// Destination read: colour plus coverage alpha (255 for formats without one).
template <Deki::ColorFormat F>
inline void ReadDstPixel(const uint8_t* target, size_t idx, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a)
{
    if constexpr (F == Deki::ColorFormat::RGB565)
    {
        UnpackRGB565(((const uint16_t*)target)[idx], r, g, b);
        a = 255;
    }
    else if constexpr (F == Deki::ColorFormat::RGB888)
    {
        r = target[idx * 3]; g = target[idx * 3 + 1]; b = target[idx * 3 + 2];
        a = 255;
    }
    else if constexpr (F == Deki::ColorFormat::ARGB8888)
    {
        const uint32_t v = ((const uint32_t*)target)[idx];
        r = (v >> 16) & 0xFF; g = (v >> 8) & 0xFF; b = v & 0xFF;
        a = 255;
    }
    else  // RGB565A8: [lo, hi, alpha]
    {
        const uint16_t v = (uint16_t)target[idx * 3] | ((uint16_t)target[idx * 3 + 1] << 8);
        UnpackRGB565(v, r, g, b);
        a = target[idx * 3 + 2];
    }
}

// Destination write with the coverage alpha the format keeps (ignored by the
// formats without one).
template <Deki::ColorFormat F>
inline void WriteDstPixel(uint8_t* target, size_t idx, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if constexpr (F == Deki::ColorFormat::RGB565)
    {
        ((uint16_t*)target)[idx] = PackRGB565(r, g, b);
        (void)a;
    }
    else if constexpr (F == Deki::ColorFormat::RGB888)
    {
        target[idx * 3] = r; target[idx * 3 + 1] = g; target[idx * 3 + 2] = b;
        (void)a;
    }
    else if constexpr (F == Deki::ColorFormat::ARGB8888)
    {
        ((uint32_t*)target)[idx] = (0xFFu << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        (void)a;
    }
    else
    {
        const uint16_t v = PackRGB565(r, g, b);
        target[idx * 3] = (uint8_t)(v & 0xFF);
        target[idx * 3 + 1] = (uint8_t)(v >> 8);
        target[idx * 3 + 2] = a;
    }
}


}  // namespace DekiPixel
