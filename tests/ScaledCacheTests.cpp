// The scaled-blit cache (Source::cacheScaled): a source drawn at the same
// size in two frames is drawn from a copy scaled once, 1:1. The copy must give
// the pixels the scaled blit gives, and must only be made, kept and dropped as
// QuadBlit.h says.

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

std::vector<uint8_t> MakePixels(bool withAlpha)
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
                // Opaque, clear and in between, with opaque and clear runs.
                const int a = (x * 53 + y * 29) % 300;
                p[2] = static_cast<uint8_t>(a > 255 ? 255 : (x < 2 ? 0 : a));
            }
        }
    }
    return pixels;
}

QuadBlit::Source SourceOf(const std::vector<uint8_t>& pixels, bool withAlpha, bool cacheable)
{
    QuadBlit::Source s = QuadBlit::MakeSource(
        pixels.data(), kSrcW, kSrcH, withAlpha ? QuadBlit::PixelLayout::RGB565A8() : QuadBlit::PixelLayout::RGB565());
    s.cacheScaled = cacheable;
    return s;
}

std::vector<uint8_t> Background()
{
    std::vector<uint8_t> target(static_cast<size_t>(kDstW) * kDstH * 2);
    for (size_t i = 0; i < target.size(); ++i)
    {
        target[i] = static_cast<uint8_t>(i * 9 + 1);
    }
    return target;
}

std::vector<uint8_t> Draw(const QuadBlit::Source& s, int destX, int destY, int destW, int destH, uint8_t tr,
                          uint8_t tg, uint8_t tb, uint8_t ta)
{
    std::vector<uint8_t> target = Background();
    QuadBlit::ClearClipStack();
    QuadBlit::BlitScaled(s, target.data(), kDstW, kDstH, Deki::ColorFormat::RGB565, destX, destY, destW, destH, tr, tg,
                         tb, ta);
    return target;
}

class ScaledCache : public ::testing::Test
{
protected:
    void SetUp() override { QuadBlit::ClearScaledCache(); }
    void TearDown() override { QuadBlit::ClearScaledCache(); }
};

}  // namespace

// Drawn from the copy, the pixels are the scaled blit's, for every tint, size
// and clip the scaled path takes.
TEST_F(ScaledCache, CopyGivesTheScaledBlitsPixels)
{
    struct Tint
    {
        uint8_t r, g, b, a;
    };
    const Tint tints[] = { { 255, 255, 255, 255 }, { 255, 255, 255, 90 }, { 240, 120, 30, 255 }, { 60, 200, 255, 150 } };
    const int rects[][4] = {
        { 4, 3, 18, 14 },    // twice the size
        { 10, 8, 5, 4 },     // shrunk
        { 7, 5, 27, 9 },     // stretched one way
        { -6, -4, 20, 21 },  // clipped top left
        { 38, 30, 16, 12 },  // clipped bottom right
    };
    for (bool withAlpha : { false, true })
    {
        const std::vector<uint8_t> pixels = MakePixels(withAlpha);
        for (const auto& r : rects)
        {
            for (const Tint& t : tints)
            {
                QuadBlit::ClearScaledCache();
                const std::vector<uint8_t> want =
                    Draw(SourceOf(pixels, withAlpha, false), r[0], r[1], r[2], r[3], t.r, t.g, t.b, t.a);
                const QuadBlit::Source cached = SourceOf(pixels, withAlpha, true);
                Draw(cached, r[0], r[1], r[2], r[3], t.r, t.g, t.b, t.a);
                QuadBlit::NextFrame();
                const std::vector<uint8_t> got = Draw(cached, r[0], r[1], r[2], r[3], t.r, t.g, t.b, t.a);
                ASSERT_GT(QuadBlit::ScaledCacheBytes(), 0u) << "no copy made";
                EXPECT_EQ(got, want) << "alpha " << withAlpha << " rect " << r[0] << "," << r[1] << " " << r[2] << "x"
                                     << r[3] << " tint " << int(t.r) << "," << int(t.g) << "," << int(t.b) << ","
                                     << int(t.a);
            }
        }
    }
}

// A size is only copied once it has been drawn in an earlier frame too: the
// strips of one frame draw it several times, and that is not twice.
TEST_F(ScaledCache, CopiesASizeOnlyInItsSecondFrame)
{
    const std::vector<uint8_t> pixels = MakePixels(true);
    const QuadBlit::Source s = SourceOf(pixels, true, true);
    Draw(s, 4, 3, 18, 14, 255, 255, 255, 255);
    Draw(s, 4, 3, 18, 14, 255, 255, 255, 255);
    EXPECT_EQ(QuadBlit::ScaledCacheBytes(), 0u);
    EXPECT_EQ(QuadBlit::ScaledCacheEntries(), 1u);
    QuadBlit::NextFrame();
    Draw(s, 4, 3, 18, 14, 255, 255, 255, 255);
    EXPECT_GT(QuadBlit::ScaledCacheBytes(), 0u);
}

// A size that changes every frame (an animated scale) is never copied.
TEST_F(ScaledCache, AnimatedSizeIsNotCopied)
{
    const std::vector<uint8_t> pixels = MakePixels(true);
    const QuadBlit::Source s = SourceOf(pixels, true, true);
    for (int f = 0; f < 10; ++f)
    {
        Draw(s, 2, 2, 12 + f, 10 + f, 255, 255, 255, 255);
        QuadBlit::NextFrame();
    }
    EXPECT_EQ(QuadBlit::ScaledCacheBytes(), 0u);
}

// Only sources that ask, at a size other than their own, are copied.
TEST_F(ScaledCache, OnlyCacheableScaledSourcesAreCopied)
{
    const std::vector<uint8_t> pixels = MakePixels(true);
    for (int f = 0; f < 3; ++f)
    {
        Draw(SourceOf(pixels, true, false), 4, 3, 18, 14, 255, 255, 255, 255);
        Draw(SourceOf(pixels, true, true), 4, 3, kSrcW, kSrcH, 255, 255, 255, 255);
        QuadBlit::NextFrame();
    }
    EXPECT_EQ(QuadBlit::ScaledCacheEntries(), 0u);
}

// ForgetScaled drops the copies of the pixels it is given, and only those.
TEST_F(ScaledCache, ForgetDropsThatSourcesCopies)
{
    const std::vector<uint8_t> a = MakePixels(true);
    const std::vector<uint8_t> b = MakePixels(true);
    for (int f = 0; f < 2; ++f)
    {
        Draw(SourceOf(a, true, true), 4, 3, 18, 14, 255, 255, 255, 255);
        Draw(SourceOf(b, true, true), 4, 3, 18, 14, 255, 255, 255, 255);
        QuadBlit::NextFrame();
    }
    ASSERT_EQ(QuadBlit::ScaledCacheEntries(), 2u);
    QuadBlit::ForgetScaled(a.data(), a.size());
    EXPECT_EQ(QuadBlit::ScaledCacheEntries(), 1u);
    QuadBlit::ForgetScaled(b.data(), b.size());
    EXPECT_EQ(QuadBlit::ScaledCacheEntries(), 0u);
    EXPECT_EQ(QuadBlit::ScaledCacheBytes(), 0u);
}

// A copy not drawn for a while is dropped; one still drawn is kept.
TEST_F(ScaledCache, IdleCopiesAreDropped)
{
    const std::vector<uint8_t> kept = MakePixels(true);
    const std::vector<uint8_t> idle = MakePixels(false);
    for (int f = 0; f < 2; ++f)
    {
        Draw(SourceOf(kept, true, true), 4, 3, 18, 14, 255, 255, 255, 255);
        Draw(SourceOf(idle, false, true), 4, 3, 18, 14, 255, 255, 255, 255);
        QuadBlit::NextFrame();
    }
    ASSERT_EQ(QuadBlit::ScaledCacheEntries(), 2u);
    for (int f = 0; f < 200; ++f)
    {
        Draw(SourceOf(kept, true, true), 4, 3, 18, 14, 255, 255, 255, 255);
        QuadBlit::NextFrame();
    }
    EXPECT_EQ(QuadBlit::ScaledCacheEntries(), 1u);
}
