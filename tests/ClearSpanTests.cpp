// A source can list each row's run of fully transparent pixels
// (clearRowSpans), and a 1:1 tinted blit then skips it without reading. An
// outline-shaped source must blend to the same pixels with and without the
// runs: tinted or not, faded or not, clipped on every side, with rows that
// have no run.

#include <gtest/gtest.h>
#include <cstdint>
#include <vector>

#include <deki/Engine.h>
#include "PixelFormat.h"
#include "QuadBlit.h"

namespace
{

constexpr int kSrcW = 30, kSrcH = 20;
constexpr int kDstW = 44, kDstH = 30;

// A frame 3 px thick with soft alpha, empty inside. Rows 0-2 and the last
// three are all frame (no transparent run); the rest have one inside.
struct Outline
{
    std::vector<uint8_t> pixels = std::vector<uint8_t>(kSrcW * kSrcH * 3, 0);
    std::vector<int16_t> clear = std::vector<int16_t>(kSrcH * 2, 0);

    Outline()
    {
        for (int y = 0; y < kSrcH; ++y)
        {
            for (int x = 0; x < kSrcW; ++x)
            {
                const bool edge = x < 3 || x >= kSrcW - 3 || y < 3 || y >= kSrcH - 3;
                if (!edge)
                {
                    continue;
                }
                uint8_t* p = &pixels[(y * kSrcW + x) * 3];
                DekiPixel::Store565(p, static_cast<uint16_t>(0x4A69u * (x + 1) + 0x0821u * y));
                p[2] = static_cast<uint8_t>(60 + (x * 41 + y * 17) % 196);
            }
            if (y >= 3 && y < kSrcH - 3)
            {
                clear[y * 2] = 3;
                clear[y * 2 + 1] = static_cast<int16_t>(kSrcW - 3);
            }
        }
    }
};

std::vector<uint8_t> Draw(const Outline& o, bool withRuns, int destX, int destY, uint8_t tr, uint8_t tg, uint8_t tb,
                          uint8_t ta, bool clip)
{
    std::vector<uint8_t> target(static_cast<size_t>(kDstW) * kDstH * 2);
    for (size_t i = 0; i < target.size(); ++i)
    {
        target[i] = static_cast<uint8_t>(i * 5 + 11);
    }
    QuadBlit::Source s = QuadBlit::MakeSource(o.pixels.data(), kSrcW, kSrcH, QuadBlit::PixelLayout::RGB565A8());
    s.clearRowSpans = withRuns ? o.clear.data() : nullptr;
    QuadBlit::ClearClipStack();
    if (clip)
    {
        QuadBlit::PushClipRect(destX + 2, destY + 4, destX + 22, destY + 15);
    }
    QuadBlit::BlitScaled(s, target.data(), kDstW, kDstH, Deki::ColorFormat::RGB565, destX, destY, kSrcW, kSrcH, tr, tg,
                         tb, ta);
    QuadBlit::ClearClipStack();
    return target;
}

}  // namespace

TEST(ClearSpans, SkippingTransparentRunsChangesNoPixel)
{
    const Outline o;
    const uint8_t tints[][4] = { { 255, 255, 255, 20 }, { 255, 255, 255, 255 }, { 180, 60, 220, 255 },
                                 { 30, 200, 90, 128 } };
    const int positions[][2] = { { 6, 4 }, { -5, 3 }, { 20, -4 }, { -12, -8 }, { 30, 20 } };
    for (const auto& t : tints)
    {
        for (const auto& pos : positions)
        {
            for (bool clip : { false, true })
            {
                EXPECT_EQ(Draw(o, true, pos[0], pos[1], t[0], t[1], t[2], t[3], clip),
                          Draw(o, false, pos[0], pos[1], t[0], t[1], t[2], t[3], clip))
                    << "tint " << int(t[0]) << "," << int(t[1]) << "," << int(t[2]) << "," << int(t[3]) << " at "
                    << pos[0] << "," << pos[1] << " clip " << clip;
            }
        }
    }
}
