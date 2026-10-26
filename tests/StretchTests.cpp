// Stretched strips (Source::stretchWidth / stretchHeight).
//
// One column drawn as a picture whose rows are each one colour, or one row
// drawn as a picture whose rows are all the same, must put down exactly the
// pixels the whole picture would: placed the same, scaled the same, clipped
// the same and rotated the same.

#include <gtest/gtest.h>

#include "QuadBlit.h"
#include "PixelFormat.h"
#include <deki/Engine.h>  // Deki::ColorFormat

#include <cstdint>
#include <vector>

using namespace DekiRendering;

namespace
{

constexpr int32_t kTargetW = 120;
constexpr int32_t kTargetH = 90;

// A picture `w` x `h` and the strip that stands in for it: rows each one
// colour (`down`), or every row the same (`!down`).
struct Pair
{
    std::vector<uint16_t> whole;
    std::vector<uint16_t> strip;
    int32_t w, h;
    bool down;
};

uint16_t ColourAt(int32_t i)
{
    return static_cast<uint16_t>(0x1234 + i * 2731);
}

Pair MakePair(int32_t w, int32_t h, bool down)
{
    Pair p{ std::vector<uint16_t>(static_cast<size_t>(w) * h), {}, w, h, down };
    p.strip.resize(down ? h : w);
    for (int32_t y = 0; y < h; y++)
    {
        for (int32_t x = 0; x < w; x++)
        {
            p.whole[static_cast<size_t>(y) * w + x] = ColourAt(down ? y : x);
        }
    }
    for (size_t i = 0; i < p.strip.size(); i++)
    {
        p.strip[i] = ColourAt(static_cast<int32_t>(i));
    }
    return p;
}

std::vector<uint16_t> Draw(const QuadBlit::Source& source, int32_t x, int32_t y, float scaleX, float scaleY,
                           float rotation, float pivotX, float pivotY)
{
    std::vector<uint16_t> target(static_cast<size_t>(kTargetW) * kTargetH, 0xBEEF);
    QuadBlit::Blit(source, reinterpret_cast<uint8_t*>(target.data()), kTargetW, kTargetH, Deki::ColorFormat::RGB565, x,
                   y, scaleX, scaleY, rotation, pivotX, pivotY, 255, 255, 255, 255, false);
    return target;
}

void ExpectSameFrames(const Pair& p)
{
    QuadBlit::Source whole = QuadBlit::MakeSource(reinterpret_cast<const uint8_t*>(p.whole.data()), p.w, p.h,
                                                  QuadBlit::PixelLayout::RGB565());
    QuadBlit::Source strip = QuadBlit::MakeSource(reinterpret_cast<const uint8_t*>(p.strip.data()), p.down ? 1 : p.w,
                                                  p.down ? p.h : 1, QuadBlit::PixelLayout::RGB565());
    (p.down ? strip.stretchWidth : strip.stretchHeight) = p.down ? p.w : p.h;

    const float scales[] = { 1.0f, 2.0f, 0.5f, 1.37f, 3.0f, 0.71f };
    const float rotations[] = { 0.0f, 0.3f, -1.1f };
    const int32_t places[][2] = { { 60, 45 }, { 5, 80 }, { 118, 2 }, { -10, 40 } };
    for (float sx : scales)
    {
        for (float sy : { 1.0f, 1.6f })
        {
            for (float r : rotations)
            {
                for (const auto& at : places)
                {
                    for (float pivot : { 0.5f, 0.0f })
                    {
                        ASSERT_EQ(Draw(strip, at[0], at[1], sx, sy, r, pivot, pivot),
                                  Draw(whole, at[0], at[1], sx, sy, r, pivot, pivot))
                            << (p.down ? "column" : "row") << " scale " << sx << "x" << sy << " rotation " << r
                            << " at " << at[0] << "," << at[1] << " pivot " << pivot;
                    }
                }
            }
        }
    }
}

}  // namespace

class StretchTest : public ::testing::Test
{
protected:
    void SetUp() override { QuadBlit::ClearClipStack(); }
    void TearDown() override { QuadBlit::ClearClipStack(); }
};

TEST_F(StretchTest, OneColumnDrawsTheWholePicture)
{
    ExpectSameFrames(MakePair(48, 30, true));
}

TEST_F(StretchTest, OneRowDrawsTheWholePicture)
{
    ExpectSameFrames(MakePair(48, 30, false));
}

TEST_F(StretchTest, ClippedTheSame)
{
    QuadBlit::PushClipRect(20, 15, 90, 70);
    ExpectSameFrames(MakePair(64, 40, true));
    ExpectSameFrames(MakePair(64, 40, false));
}

TEST_F(StretchTest, PlacedAndCoveredAsTheWholePicture)
{
    const Pair p = MakePair(48, 30, true);
    QuadBlit::Source whole = QuadBlit::MakeSource(reinterpret_cast<const uint8_t*>(p.whole.data()), p.w, p.h,
                                                  QuadBlit::PixelLayout::RGB565());
    QuadBlit::Source strip =
        QuadBlit::MakeSource(reinterpret_cast<const uint8_t*>(p.strip.data()), 1, p.h, QuadBlit::PixelLayout::RGB565());
    strip.stretchWidth = p.w;
    for (float r : { 0.0f, 0.4f })
    {
        int32_t l1, t1, r1, b1, l2, t2, r2, b2;
        ASSERT_TRUE(QuadBlit::BlitArea(whole, 50, 40, 2.5f, 3.0f, r, 0.5f, 0.5f, l1, t1, r1, b1));
        ASSERT_TRUE(QuadBlit::BlitArea(strip, 50, 40, 2.5f, 3.0f, r, 0.5f, 0.5f, l2, t2, r2, b2));
        EXPECT_EQ(l1, l2);
        EXPECT_EQ(t1, t2);
        EXPECT_EQ(r1, r2);
        EXPECT_EQ(b1, b2);
    }
    EXPECT_EQ(QuadBlit::CoversRect(whole, 50, 40, 2.5f, 3.0f, 0.0f, 0.5f, 0.5f, 255, 0, 0, kTargetW, kTargetH),
              QuadBlit::CoversRect(strip, 50, 40, 2.5f, 3.0f, 0.0f, 0.5f, 0.5f, 255, 0, 0, kTargetW, kTargetH));
    EXPECT_TRUE(QuadBlit::CoversRect(strip, 60, 45, 3.0f, 4.0f, 0.0f, 0.5f, 0.5f, 255, 0, 0, kTargetW, kTargetH));
}
