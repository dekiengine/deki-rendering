// A source that promises uniform rows (each row one colour) or uniform
// columns (every row the first) is copied 1:1 without reading all of it. The
// pixels must be those of the plain copy: inside the target, clipped on any
// side, at odd and even positions and widths.

#include <gtest/gtest.h>
#include <cstdint>
#include <vector>

#include <deki/Engine.h>
#include "PixelFormat.h"
#include "QuadBlit.h"

namespace
{

constexpr int kDstW = 37, kDstH = 23;

enum class Kind
{
    Rows,
    Columns
};

std::vector<uint8_t> MakeSource(Kind kind, int w, int h)
{
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 2);
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const int i = kind == Kind::Rows ? y : x;
            DekiPixel::Store565(&pixels[(static_cast<size_t>(y) * w + x) * 2],
                                static_cast<uint16_t>(0x0841u * (i + 1) + 0x1000u * (i & 3)));
        }
    }
    return pixels;
}

std::vector<uint8_t> Copy(const std::vector<uint8_t>& pixels, int w, int h, Kind kind, bool promise, int destX,
                          int destY)
{
    std::vector<uint8_t> target(static_cast<size_t>(kDstW) * kDstH * 2);
    for (size_t i = 0; i < target.size(); ++i)
    {
        target[i] = static_cast<uint8_t>(i * 13 + 5);
    }
    QuadBlit::Source s = QuadBlit::MakeSource(pixels.data(), w, h, QuadBlit::PixelLayout::RGB565());
    s.uniformRows = promise && kind == Kind::Rows;
    s.uniformColumns = promise && kind == Kind::Columns;
    QuadBlit::ClearClipStack();
    QuadBlit::BlitScaled(s, target.data(), kDstW, kDstH, Deki::ColorFormat::RGB565, destX, destY, w, h);
    return target;
}

}  // namespace

TEST(UniformSource, PromisedCopiesMatchThePlainCopy)
{
    const int sizes[][2] = { { 20, 10 }, { 21, 9 }, { 1, 12 }, { 50, 30 } };
    const int positions[][2] = { { 3, 2 }, { 4, 5 }, { -6, 1 }, { 30, -4 }, { -10, -10 } };
    for (Kind kind : { Kind::Rows, Kind::Columns })
    {
        for (const auto& size : sizes)
        {
            const std::vector<uint8_t> src = MakeSource(kind, size[0], size[1]);
            for (const auto& pos : positions)
            {
                EXPECT_EQ(Copy(src, size[0], size[1], kind, true, pos[0], pos[1]),
                          Copy(src, size[0], size[1], kind, false, pos[0], pos[1]))
                    << (kind == Kind::Rows ? "rows" : "columns") << ", " << size[0] << "x" << size[1] << " at "
                    << pos[0] << "," << pos[1];
            }
        }
    }
}

TEST(UniformSource, ClipRectIsHonoured)
{
    const std::vector<uint8_t> src = MakeSource(Kind::Rows, 30, 20);
    auto draw = [&](bool promise)
    {
        std::vector<uint8_t> target(static_cast<size_t>(kDstW) * kDstH * 2, 0);
        QuadBlit::Source s = QuadBlit::MakeSource(src.data(), 30, 20, QuadBlit::PixelLayout::RGB565());
        s.uniformRows = promise;
        QuadBlit::ClearClipStack();
        QuadBlit::PushClipRect(5, 4, 17, 13);
        QuadBlit::BlitScaled(s, target.data(), kDstW, kDstH, Deki::ColorFormat::RGB565, 0, 0, 30, 20);
        QuadBlit::ClearClipStack();
        return target;
    };
    EXPECT_EQ(draw(true), draw(false));
}
