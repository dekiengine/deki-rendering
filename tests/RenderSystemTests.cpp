// DekiRenderSystem: buffer management, clearing, pixel access.

#include <gtest/gtest.h>
#include "DekiRenderSystem.h"
#include <deki/Engine.h>  // Deki::ColorFormat

// Tests name the package's types unqualified.
using namespace DekiRendering;

// ============================================================================
// GetBytesPerPixel
// ============================================================================

class RenderSystemBPPTest : public ::testing::Test
{
};

TEST_F(RenderSystemBPPTest, RGB565_Returns2)
{
    DekiRenderSystem rs;
    EXPECT_EQ(rs.GetBytesPerPixel(Deki::ColorFormat::RGB565), 2);
}

TEST_F(RenderSystemBPPTest, RGB888_Returns3)
{
    DekiRenderSystem rs;
    EXPECT_EQ(rs.GetBytesPerPixel(Deki::ColorFormat::RGB888), 3);
}

TEST_F(RenderSystemBPPTest, ARGB8888_Returns4)
{
    DekiRenderSystem rs;
    EXPECT_EQ(rs.GetBytesPerPixel(Deki::ColorFormat::ARGB8888), 4);
}

TEST_F(RenderSystemBPPTest, RGB565A8_Returns3)
{
    DekiRenderSystem rs;
    EXPECT_EQ(rs.GetBytesPerPixel(Deki::ColorFormat::RGB565A8), 3);
}

// ============================================================================
// Setup
// ============================================================================

class RenderSystemSetupTest : public ::testing::Test
{
protected:
    void SetUp() override {}

    void TearDown() override {}
};

TEST_F(RenderSystemSetupTest, SetupAllocatesBuffer)
{
    DekiRenderSystem rs;
    EXPECT_TRUE(rs.Setup(32, 24, Deki::ColorFormat::RGB565));

    EXPECT_EQ(rs.GetScreenWidth(), 32);
    EXPECT_EQ(rs.GetScreenHeight(), 24);
    EXPECT_EQ(rs.GetColorFormat(), Deki::ColorFormat::RGB565);
    EXPECT_NE(rs.GetFrameBuffer(), nullptr);
}

TEST_F(RenderSystemSetupTest, SetupRGB888)
{
    DekiRenderSystem rs;
    EXPECT_TRUE(rs.Setup(16, 16, Deki::ColorFormat::RGB888));

    EXPECT_EQ(rs.GetScreenWidth(), 16);
    EXPECT_EQ(rs.GetScreenHeight(), 16);
    EXPECT_EQ(rs.GetColorFormat(), Deki::ColorFormat::RGB888);
    EXPECT_NE(rs.GetFrameBuffer(), nullptr);
}

TEST_F(RenderSystemSetupTest, SetupARGB8888)
{
    DekiRenderSystem rs;
    EXPECT_TRUE(rs.Setup(8, 8, Deki::ColorFormat::ARGB8888));

    EXPECT_EQ(rs.GetScreenWidth(), 8);
    EXPECT_EQ(rs.GetScreenHeight(), 8);
    EXPECT_EQ(rs.GetColorFormat(), Deki::ColorFormat::ARGB8888);
    EXPECT_NE(rs.GetFrameBuffer(), nullptr);
}

TEST_F(RenderSystemSetupTest, SetupCanBeCalledTwice)
{
    DekiRenderSystem rs;
    EXPECT_TRUE(rs.Setup(32, 24, Deki::ColorFormat::RGB565));
    EXPECT_TRUE(rs.Setup(64, 48, Deki::ColorFormat::RGB888));

    EXPECT_EQ(rs.GetScreenWidth(), 64);
    EXPECT_EQ(rs.GetScreenHeight(), 48);
    EXPECT_EQ(rs.GetColorFormat(), Deki::ColorFormat::RGB888);
}

// ============================================================================
// ClearBuffer
// ============================================================================

TEST_F(RenderSystemSetupTest, ClearBuffer_RGB565)
{
    DekiRenderSystem rs;
    rs.Setup(4, 4, Deki::ColorFormat::RGB565);

    rs.ClearBuffer(255, 0, 0);  // red

    uint8_t r, g, b;
    rs.GetPixel(0, 0, &r, &g, &b);
    // RGB565 loses precision: 255 -> (31 << 3) = 248
    EXPECT_GE(r, 240);
    EXPECT_LE(g, 8);
    EXPECT_LE(b, 8);

    rs.GetPixel(3, 3, &r, &g, &b);
    EXPECT_GE(r, 240);
}

TEST_F(RenderSystemSetupTest, ClearBuffer_RGB888)
{
    DekiRenderSystem rs;
    rs.Setup(4, 4, Deki::ColorFormat::RGB888);

    rs.ClearBuffer(0, 128, 255);

    uint8_t r, g, b;
    rs.GetPixel(2, 2, &r, &g, &b);
    EXPECT_EQ(r, 0);
    EXPECT_EQ(g, 128);
    EXPECT_EQ(b, 255);
}

TEST_F(RenderSystemSetupTest, ClearBuffer_ARGB8888)
{
    DekiRenderSystem rs;
    rs.Setup(4, 4, Deki::ColorFormat::ARGB8888);

    rs.ClearBuffer(64, 128, 192);

    uint8_t r, g, b;
    rs.GetPixel(1, 1, &r, &g, &b);
    EXPECT_EQ(r, 64);
    EXPECT_EQ(g, 128);
    EXPECT_EQ(b, 192);
}

TEST_F(RenderSystemSetupTest, Setup_RGB565A8)
{
    DekiRenderSystem rs;
    EXPECT_TRUE(rs.Setup(8, 8, Deki::ColorFormat::RGB565A8));

    EXPECT_EQ(rs.GetScreenWidth(), 8);
    EXPECT_EQ(rs.GetScreenHeight(), 8);
    EXPECT_EQ(rs.GetColorFormat(), Deki::ColorFormat::RGB565A8);
    EXPECT_NE(rs.GetFrameBuffer(), nullptr);
}

TEST_F(RenderSystemSetupTest, ClearBuffer_RGB565A8_RoundTripsRGB)
{
    DekiRenderSystem rs;
    rs.Setup(4, 4, Deki::ColorFormat::RGB565A8);

    // ClearBuffer writes RGB565 plus alpha 0xFF per pixel.
    rs.ClearBuffer(0, 128, 255);

    uint8_t r, g, b;
    rs.GetPixel(2, 2, &r, &g, &b);
    // RGB565 quantisation: G has 6 bits, B has 5.
    EXPECT_LE(r, 8);
    EXPECT_GE(g, 124);  // 128 = 0x80 keeps its top 6 bits, so reads back as ~128
    EXPECT_LE(g, 132);
    EXPECT_GE(b, 247);  // 255 -> 31 << 3 = 248
}

// ============================================================================
// GetPixel edge cases
// ============================================================================

TEST_F(RenderSystemSetupTest, GetPixel_OutOfBounds_ReturnsBlack)
{
    DekiRenderSystem rs;
    rs.Setup(4, 4, Deki::ColorFormat::RGB565);
    rs.ClearBuffer(255, 255, 255);

    uint8_t r, g, b;
    rs.GetPixel(-1, 0, &r, &g, &b);
    EXPECT_EQ(r, 0);
    EXPECT_EQ(g, 0);
    EXPECT_EQ(b, 0);

    rs.GetPixel(0, -1, &r, &g, &b);
    EXPECT_EQ(r, 0);

    rs.GetPixel(4, 0, &r, &g, &b);
    EXPECT_EQ(r, 0);

    rs.GetPixel(0, 4, &r, &g, &b);
    EXPECT_EQ(r, 0);
}

TEST_F(RenderSystemSetupTest, GetPixel_NullBuffer_ReturnsBlack)
{
    DekiRenderSystem rs;
    // No Setup(), so the buffer is null.

    uint8_t r = 99, g = 99, b = 99;
    rs.GetPixel(0, 0, &r, &g, &b);
    EXPECT_EQ(r, 0);
    EXPECT_EQ(g, 0);
    EXPECT_EQ(b, 0);
}

TEST_F(RenderSystemSetupTest, GetPixel_ColorOverload)
{
    DekiRenderSystem rs;
    rs.Setup(4, 4, Deki::ColorFormat::RGB888);
    rs.ClearBuffer(100, 150, 200);

    Deki::Color c = rs.GetPixel(0, 0);
    EXPECT_EQ(c.r, 100);
    EXPECT_EQ(c.g, 150);
    EXPECT_EQ(c.b, 200);
}

// ============================================================================
// Renderer management
// ============================================================================

TEST_F(RenderSystemSetupTest, SetRenderer_GetRenderer)
{
    DekiRenderSystem rs;
    EXPECT_EQ(rs.GetRenderer(), nullptr);

    // A real DekiRenderer needs more setup than this test has; it checks null
    // handling only.
    rs.SetRenderer(nullptr);
    EXPECT_EQ(rs.GetRenderer(), nullptr);
}

TEST_F(RenderSystemSetupTest, RenderWithNullScene_NoOp)
{
    DekiRenderSystem rs;
    rs.Setup(4, 4, Deki::ColorFormat::RGB565);

    // Must not crash.
    rs.Render(nullptr);
}

TEST_F(RenderSystemSetupTest, RenderWithNoRenderer_NoOp)
{
    DekiRenderSystem rs;
    rs.Setup(4, 4, Deki::ColorFormat::RGB565);
    rs.SetRenderer(nullptr);

    // Must not crash with no renderer set. A null scene stands in for a real
    // one, which is hard to build here.
    rs.Render(nullptr);
}
