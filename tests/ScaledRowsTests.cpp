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

// A source wider than WholeScaleRows_RGB565 decodes at once (256 columns),
// at 2x into a wide target, clipped on both sides: the runs join seamlessly.
TEST(ScaledRows, AWideSourceAtTwiceItsSizeMatchesThePipeline)
{
    constexpr int w = 300, h = 3, tw = 640, th = 8;
    std::vector<uint8_t> src(static_cast<size_t>(w) * h * 3);
    for (int i = 0; i < w * h; ++i)
    {
        DekiPixel::Store565(&src[static_cast<size_t>(i) * 3], static_cast<uint16_t>(0x1F3Bu * (i + 3)));
        src[static_cast<size_t>(i) * 3 + 2] = static_cast<uint8_t>((i * 37) % 256);
    }
    auto draw = [&](bool viaPipeline)
    {
        std::vector<uint8_t> target(static_cast<size_t>(tw) * th * 2);
        for (size_t i = 0; i < target.size(); ++i)
        {
            target[i] = static_cast<uint8_t>(i * 5 + 3);
        }
        QuadBlit::Source s = QuadBlit::MakeSource(src.data(), w, h, QuadBlit::PixelLayout::RGB565A8());
        if (viaPipeline)
        {
            s.hasChromaKey = true;
            s.keyR = 1;
            s.keyG = 2;
            s.keyB = 3;
        }
        QuadBlit::ClearClipStack();
        QuadBlit::BlitScaled(s, target.data(), tw, th, Deki::ColorFormat::RGB565, -7, 1, w * 2, h * 2, 255, 255, 255,
                             255);
        return target;
    };
    EXPECT_EQ(draw(false), draw(true));
}

// Flipped across, down or both, at whole-number scales: the pipeline's
// pixels (ApplyFlips), clipped on a side too.
TEST(ScaledRows, AFlippedSpriteAtAWholeScaleMatchesThePipeline)
{
    const std::vector<uint8_t> src = MakeSource(true);
    const int rects[][4] = { { 3, 2, 18, 14 }, { 5, 1, 27, 21 }, { -5, -3, 18, 14 } };
    for (int flips = 1; flips <= 3; ++flips)
    {
        for (const auto& r : rects)
        {
            auto draw = [&](bool viaPipeline)
            {
                std::vector<uint8_t> target(static_cast<size_t>(kDstW) * kDstH * 2);
                for (size_t i = 0; i < target.size(); ++i)
                {
                    target[i] = static_cast<uint8_t>(i * 9 + 1);
                }
                QuadBlit::Source s =
                    QuadBlit::MakeSource(src.data(), kSrcW, kSrcH, QuadBlit::PixelLayout::RGB565A8());
                s.flipH = (flips & 1) != 0;
                s.flipV = (flips & 2) != 0;
                if (viaPipeline)
                {
                    s.hasChromaKey = true;
                    s.keyR = 1;
                    s.keyG = 2;
                    s.keyB = 3;
                }
                QuadBlit::ClearClipStack();
                QuadBlit::BlitScaled(s, target.data(), kDstW, kDstH, Deki::ColorFormat::RGB565, r[0], r[1], r[2], r[3],
                                     255, 255, 255, 255);
                return target;
            };
            EXPECT_EQ(draw(false), draw(true)) << "flips " << flips << " rect " << r[0] << "," << r[1] << " " << r[2]
                                               << "x" << r[3];
        }
    }
}

// Opaque and plain at whole-number scales (RepeatRows_RGB565), clipped by
// the target's edges and by a clip that starts inside a pixel's block.
TEST(ScaledRows, AnOpaqueSourceRepeatedAtAWholeScaleMatchesThePipeline)
{
    const std::vector<uint8_t> src = MakeSource(false);
    const int rects[][4] = { { 3, 2, 18, 14 }, { 1, 0, 27, 21 }, { -5, -3, 36, 28 }, { 30, 25, 36, 28 } };
    for (const auto& r : rects)
    {
        for (int clipped = 0; clipped < 2; ++clipped)
        {
            auto draw = [&](bool viaPipeline)
            {
                std::vector<uint8_t> target(static_cast<size_t>(kDstW) * kDstH * 2);
                for (size_t i = 0; i < target.size(); ++i)
                {
                    target[i] = static_cast<uint8_t>(i * 9 + 1);
                }
                QuadBlit::Source s = QuadBlit::MakeSource(src.data(), kSrcW, kSrcH, QuadBlit::PixelLayout::RGB565());
                if (viaPipeline)
                {
                    s.hasChromaKey = true;
                    s.keyR = 1;
                    s.keyG = 2;
                    s.keyB = 3;
                }
                QuadBlit::ClearClipStack();
                if (clipped)
                {
                    QuadBlit::PushClipRect(r[0] + 3, r[1] + 1, r[0] + r[2] - 2, r[1] + r[3] - 3);
                }
                QuadBlit::BlitScaled(s, target.data(), kDstW, kDstH, Deki::ColorFormat::RGB565, r[0], r[1], r[2], r[3],
                                     255, 255, 255, 255);
                QuadBlit::ClearClipStack();
                return target;
            };
            EXPECT_EQ(draw(false), draw(true)) << "rect " << r[0] << "," << r[1] << " " << r[2] << "x" << r[3]
                                               << (clipped ? " clipped" : "");
        }
    }
}

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
        // Whole-number scales take WholeScaleRows_RGB565:
        { 3, 2, 18, 14 },   // 2x
        { 5, 1, 27, 21 },   // 3x
        { 2, 4, 18, 21 },   // 2x across, 3x down
        { -5, -3, 18, 14 }, // 2x, clipped mid-block top left
        { 39, 29, 18, 14 }, // 2x, clipped mid-block bottom right
        { 1, 1, 9, 7 },     // 1x is 1:1, the row paths
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
