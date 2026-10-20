// QuadBlit keeps the tint tables of the last tint between blits. A blit must
// still get its own tint's pixels whatever was drawn before it: the same tint
// again, another tint, or the same colour with another alpha.

#include <gtest/gtest.h>
#include <cstdint>
#include <vector>

#include <deki/Engine.h>
#include "PixelFormat.h"
#include "QuadBlit.h"

namespace
{

constexpr int kSrcW = 24, kSrcH = 12;
constexpr int kDstW = 40, kDstH = 20;

// An RGB565A8 source with every alpha from transparent to opaque.
std::vector<uint8_t> MakeSource()
{
    std::vector<uint8_t> pixels(kSrcW * kSrcH * 3);
    for (int y = 0; y < kSrcH; ++y)
    {
        for (int x = 0; x < kSrcW; ++x)
        {
            uint8_t* p = &pixels[(y * kSrcW + x) * 3];
            DekiPixel::Store565(p, static_cast<uint16_t>(0x1234u * (x + 1) + 0x0F0Fu * (y + 1)));
            p[2] = static_cast<uint8_t>((x * 37 + y * 11) % 256);
        }
    }
    return pixels;
}

std::vector<uint8_t> Draw(const std::vector<uint8_t>& pixels, uint8_t tr, uint8_t tg, uint8_t tb, uint8_t ta)
{
    std::vector<uint8_t> target(kDstW * kDstH * 2);
    for (size_t i = 0; i < target.size(); ++i)
    {
        target[i] = static_cast<uint8_t>(i * 7 + 3);  // a busy background, so blends show
    }
    const QuadBlit::Source s =
        QuadBlit::MakeSource(pixels.data(), kSrcW, kSrcH, QuadBlit::PixelLayout::RGB565A8(), false, nullptr);
    QuadBlit::ClearClipStack();
    QuadBlit::BlitScaled(s, target.data(), kDstW, kDstH, Deki::ColorFormat::RGB565, 8, 4, kSrcW, kSrcH, tr, tg, tb, ta);
    return target;
}

}  // namespace

TEST(TintTables, EachBlitGetsItsOwnTint)
{
    const std::vector<uint8_t> src = MakeSource();
    const std::vector<uint8_t> red = Draw(src, 255, 0, 0, 255);
    const std::vector<uint8_t> blue = Draw(src, 0, 0, 255, 255);
    EXPECT_NE(blue, red);
    EXPECT_EQ(Draw(src, 255, 0, 0, 255), red) << "red after blue";
    EXPECT_EQ(Draw(src, 255, 0, 0, 255), red) << "red after red";
    EXPECT_EQ(Draw(src, 0, 0, 255, 255), blue) << "blue after red";
}

TEST(TintTables, SameColourWithAnotherAlpha)
{
    // A fading particle keeps its colour and changes its alpha: the tables
    // are reused, the alpha must not be.
    const std::vector<uint8_t> src = MakeSource();
    const std::vector<uint8_t> faint = Draw(src, 120, 200, 255, 40);
    const std::vector<uint8_t> strong = Draw(src, 120, 200, 255, 200);
    EXPECT_NE(faint, strong);
    EXPECT_EQ(Draw(src, 120, 200, 255, 40), faint);
}
