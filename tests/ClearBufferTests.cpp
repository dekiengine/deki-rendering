/**
 * @file ClearBufferTests.cpp
 * @brief DekiRenderSystem::ClearBuffer / ClearRect write exactly the expected
 *        bytes in every framebuffer format, and ClearRect touches only its
 *        (clipped) rectangle.
 */

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <vector>

#include "DekiEngine.h"
#include "DekiRenderSystem.h"

namespace
{

std::vector<uint8_t> ExpectedPixel(DekiColorFormat f, uint8_t r, uint8_t g, uint8_t b)
{
    const uint16_t v = static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    switch (f)
    {
    case DekiColorFormat::RGB565: return { static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>(v >> 8) };
    case DekiColorFormat::RGB888: return { r, g, b };
    case DekiColorFormat::ARGB8888: return { b, g, r, 0xFF };  // little-endian 0xFFRRGGBB
    case DekiColorFormat::RGB565A8: return { static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>(v >> 8), 0xFF };
    }
    return {};
}

const DekiColorFormat kFormats[] = { DekiColorFormat::RGB565, DekiColorFormat::RGB888,
                                     DekiColorFormat::ARGB8888, DekiColorFormat::RGB565A8 };

// Every pixel of the framebuffer equals `px` inside the rect and `outside`
// elsewhere.
void ExpectFill(const DekiRenderSystem& rs, int w, int h, int l, int t, int r, int b,
                const std::vector<uint8_t>& inside, const std::vector<uint8_t>& outside)
{
    const uint8_t* fb = rs.GetFrameBuffer();
    const size_t bpp = inside.size();
    int bad = 0;
    for (int y = 0; y < h && bad < 5; ++y)
        for (int x = 0; x < w && bad < 5; ++x)
        {
            const bool in = x >= l && x < r && y >= t && y < b;
            const std::vector<uint8_t>& want = in ? inside : outside;
            if (std::memcmp(fb + (static_cast<size_t>(y) * w + x) * bpp, want.data(), bpp) != 0)
            {
                ADD_FAILURE() << "pixel (" << x << ", " << y << ") wrong";
                ++bad;
            }
        }
}

}  // namespace

TEST(ClearBuffer, EveryFormatFillsExactly)
{
    for (DekiColorFormat f : kFormats)
    {
        DekiRenderSystem rs;
        ASSERT_TRUE(rs.Setup(37, 11, f));  // odd width: the row doubling must stop exactly
        rs.ClearBuffer(200, 100, 50);
        const std::vector<uint8_t> px = ExpectedPixel(f, 200, 100, 50);
        ExpectFill(rs, 37, 11, 0, 0, 37, 11, px, px);
    }
}

TEST(ClearBuffer, ClearRectTouchesOnlyItsRectangle)
{
    for (DekiColorFormat f : kFormats)
    {
        DekiRenderSystem rs;
        ASSERT_TRUE(rs.Setup(40, 30, f));
        rs.ClearBuffer(10, 20, 30);
        rs.ClearRect(5, 7, 13, 9, 250, 120, 60);
        ExpectFill(rs, 40, 30, 5, 7, 18, 16, ExpectedPixel(f, 250, 120, 60), ExpectedPixel(f, 10, 20, 30));
    }
}

TEST(ClearBuffer, ClearRectClipsToTheFramebuffer)
{
    DekiRenderSystem rs;
    ASSERT_TRUE(rs.Setup(16, 12, DekiColorFormat::RGB565));
    rs.ClearBuffer(0, 0, 0);
    rs.ClearRect(-4, -4, 8, 8, 255, 255, 255);      // top-left corner, partly outside
    rs.ClearRect(12, 8, 100, 100, 255, 255, 255);    // bottom-right corner, partly outside
    rs.ClearRect(20, 20, 4, 4, 255, 255, 255);       // fully outside: no-op
    rs.ClearRect(3, 3, 0, 5, 255, 255, 255);         // empty: no-op
    const uint16_t* fb = reinterpret_cast<const uint16_t*>(rs.GetFrameBuffer());
    int white = 0;
    for (int i = 0; i < 16 * 12; ++i) white += (fb[i] == 0xFFFF);
    EXPECT_EQ(white, 4 * 4 + 4 * 4);
    EXPECT_EQ(fb[0], 0xFFFF);
    EXPECT_EQ(fb[11 * 16 + 15], 0xFFFF);
    EXPECT_EQ(fb[5 * 16 + 5], 0x0000);
}
