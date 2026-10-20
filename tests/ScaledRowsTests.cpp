// Scaled blits onto RGB565 from an RGB565 or RGB565A8 source take a tight
// loop (TintScaledRows_RGB565) instead of the per-pixel pipeline. They must
// give the pipeline's pixels. The pipeline is reached for comparison with a
// chroma key no source pixel has: it changes the path, not a single pixel.

#include <gtest/gtest.h>
#include <cstdint>
#include <vector>

#include <deki/Engine.h>
#include "PixelFormat.h"
#include "QuadBlit.h"

namespace
{

constexpr int kSrcW = 9, kSrcH = 7;
constexpr int kDstW = 48, kDstH = 36;

std::vector<uint8_t> MakeSource(bool withAlpha)
{
    const int bpp = withAlpha ? 3 : 2;
    std::vector<uint8_t> pixels(static_cast<size_t>(kSrcW) * kSrcH * bpp);
    for (int y = 0; y < kSrcH; ++y)
    {
        for (int x = 0; x < kSrcW; ++x)
        {
            uint8_t* p = &pixels[(static_cast<size_t>(y) * kSrcW + x) * bpp];
            DekiPixel::Store565(p, static_cast<uint16_t>(0x2C51u * (x + 1) + 0x1717u * (y + 2)));
            if (withAlpha)
            {
                p[2] = static_cast<uint8_t>((x * 53 + y * 29) % 256);
            }
        }
    }
    return pixels;
}

std::vector<uint8_t> Draw(const std::vector<uint8_t>& pixels, bool withAlpha, bool viaPipeline, int destX, int destY,
                          int destW, int destH, uint8_t tr, uint8_t tg, uint8_t tb, uint8_t ta)
{
    std::vector<uint8_t> target(static_cast<size_t>(kDstW) * kDstH * 2);
    for (size_t i = 0; i < target.size(); ++i)
    {
        target[i] = static_cast<uint8_t>(i * 9 + 1);
    }
    QuadBlit::Source s = QuadBlit::MakeSource(
        pixels.data(), kSrcW, kSrcH, withAlpha ? QuadBlit::PixelLayout::RGB565A8() : QuadBlit::PixelLayout::RGB565());
    if (viaPipeline)
    {
        // Sources hold RGB565, whose red is a multiple of 8 once expanded:
        // a red of 1 never matches.
        s.hasChromaKey = true;
        s.keyR = 1;
        s.keyG = 2;
        s.keyB = 3;
    }
    QuadBlit::ClearClipStack();
    QuadBlit::BlitScaled(s, target.data(), kDstW, kDstH, Deki::ColorFormat::RGB565, destX, destY, destW, destH, tr, tg,
                         tb, ta);
    return target;
}

}  // namespace

TEST(ScaledRows, MatchThePipelineOntoRGB565)
{
    struct Tint
    {
        uint8_t r, g, b, a;
    };
    const Tint tints[] = { { 255, 255, 255, 255 }, { 255, 255, 255, 90 }, { 240, 120, 30, 255 }, { 60, 200, 255, 150 } };
    const int rects[][4] = {
        { 4, 3, 18, 14 },   // enlarged
        { 10, 8, 5, 4 },    // shrunk
        { 7, 5, 27, 9 },    // stretched one way
        { -6, -4, 20, 21 }, // clipped top left
        { 38, 30, 16, 12 }, // clipped bottom right
    };
    for (bool withAlpha : { false, true })
    {
        const std::vector<uint8_t> src = MakeSource(withAlpha);
        for (const Tint& t : tints)
        {
            for (const auto& r : rects)
            {
                EXPECT_EQ(Draw(src, withAlpha, false, r[0], r[1], r[2], r[3], t.r, t.g, t.b, t.a),
                          Draw(src, withAlpha, true, r[0], r[1], r[2], r[3], t.r, t.g, t.b, t.a))
                    << (withAlpha ? "RGB565A8" : "RGB565") << " tint " << int(t.r) << "," << int(t.g) << ","
                    << int(t.b) << "," << int(t.a) << " rect " << r[0] << "," << r[1] << " " << r[2] << "x" << r[3];
            }
        }
    }
}
