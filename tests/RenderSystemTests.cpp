/**
 * @file RenderSystemTests.cpp
 * @brief Unit tests for DekiRenderSystem (buffer management, clear, pixel ops)
 */

#include <gtest/gtest.h>
#include "DekiRenderSystem.h"
#include "DekiEngine.h"  // For DekiColorFormat
#include "QuadBlit.h"

// ============================================================================
// GetBytesPerPixel Tests
// ============================================================================

class RenderSystemBPPTest : public ::testing::Test {};

TEST_F(RenderSystemBPPTest, RGB565_Returns2)
{
    DekiRenderSystem rs;
    EXPECT_EQ(rs.GetBytesPerPixel(DekiColorFormat::RGB565), 2);
}

TEST_F(RenderSystemBPPTest, RGB888_Returns3)
{
    DekiRenderSystem rs;
    EXPECT_EQ(rs.GetBytesPerPixel(DekiColorFormat::RGB888), 3);
}

TEST_F(RenderSystemBPPTest, ARGB8888_Returns4)
{
    DekiRenderSystem rs;
    EXPECT_EQ(rs.GetBytesPerPixel(DekiColorFormat::ARGB8888), 4);
}

// ============================================================================
// Setup Tests
// ============================================================================

class RenderSystemSetupTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        QuadBlit::SetByteSwap(false);
    }

    void TearDown() override
    {
        QuadBlit::SetByteSwap(false);
    }
};

TEST_F(RenderSystemSetupTest, SetupAllocatesBuffer)
{
    DekiRenderSystem rs;
    EXPECT_TRUE(rs.Setup(32, 24, DekiColorFormat::RGB565));

    EXPECT_EQ(rs.GetScreenWidth(), 32);
    EXPECT_EQ(rs.GetScreenHeight(), 24);
    EXPECT_EQ(rs.GetColorFormat(), DekiColorFormat::RGB565);
    EXPECT_NE(rs.GetFrameBuffer(), nullptr);
}

TEST_F(RenderSystemSetupTest, SetupRGB888)
{
    DekiRenderSystem rs;
    EXPECT_TRUE(rs.Setup(16, 16, DekiColorFormat::RGB888));

    EXPECT_EQ(rs.GetScreenWidth(), 16);
    EXPECT_EQ(rs.GetScreenHeight(), 16);
    EXPECT_EQ(rs.GetColorFormat(), DekiColorFormat::RGB888);
    EXPECT_NE(rs.GetFrameBuffer(), nullptr);
}

TEST_F(RenderSystemSetupTest, SetupARGB8888)
{
    DekiRenderSystem rs;
    EXPECT_TRUE(rs.Setup(8, 8, DekiColorFormat::ARGB8888));

    EXPECT_EQ(rs.GetScreenWidth(), 8);
    EXPECT_EQ(rs.GetScreenHeight(), 8);
    EXPECT_EQ(rs.GetColorFormat(), DekiColorFormat::ARGB8888);
    EXPECT_NE(rs.GetFrameBuffer(), nullptr);
}

TEST_F(RenderSystemSetupTest, SetupCanBeCalledTwice)
{
    DekiRenderSystem rs;
    EXPECT_TRUE(rs.Setup(32, 24, DekiColorFormat::RGB565));
    EXPECT_TRUE(rs.Setup(64, 48, DekiColorFormat::RGB888));

    EXPECT_EQ(rs.GetScreenWidth(), 64);
    EXPECT_EQ(rs.GetScreenHeight(), 48);
    EXPECT_EQ(rs.GetColorFormat(), DekiColorFormat::RGB888);
}

// ============================================================================
// ClearBuffer Tests
// ============================================================================

TEST_F(RenderSystemSetupTest, ClearBuffer_RGB565)
{
    DekiRenderSystem rs;
    rs.Setup(4, 4, DekiColorFormat::RGB565);

    rs.ClearBuffer(255, 0, 0);  // Red

    // Read back with GetPixel
    uint8_t r, g, b;
    rs.GetPixel(0, 0, &r, &g, &b);
    // RGB565 loses precision: 255 → (31<<3) = 248
    EXPECT_GE(r, 240);
    EXPECT_LE(g, 8);
    EXPECT_LE(b, 8);

    // Check another pixel
    rs.GetPixel(3, 3, &r, &g, &b);
    EXPECT_GE(r, 240);
}

TEST_F(RenderSystemSetupTest, ClearBuffer_RGB888)
{
    DekiRenderSystem rs;
    rs.Setup(4, 4, DekiColorFormat::RGB888);

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
    rs.Setup(4, 4, DekiColorFormat::ARGB8888);

    rs.ClearBuffer(64, 128, 192);

    uint8_t r, g, b;
    rs.GetPixel(1, 1, &r, &g, &b);
    EXPECT_EQ(r, 64);
    EXPECT_EQ(g, 128);
    EXPECT_EQ(b, 192);
}

// ============================================================================
// GetPixel Edge Cases
// ============================================================================

TEST_F(RenderSystemSetupTest, GetPixel_OutOfBounds_ReturnsBlack)
{
    DekiRenderSystem rs;
    rs.Setup(4, 4, DekiColorFormat::RGB565);
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
    // No Setup() called — buffer is null

    uint8_t r = 99, g = 99, b = 99;
    rs.GetPixel(0, 0, &r, &g, &b);
    EXPECT_EQ(r, 0);
    EXPECT_EQ(g, 0);
    EXPECT_EQ(b, 0);
}

TEST_F(RenderSystemSetupTest, GetPixel_ColorOverload)
{
    DekiRenderSystem rs;
    rs.Setup(4, 4, DekiColorFormat::RGB888);
    rs.ClearBuffer(100, 150, 200);

    deki::Color c = rs.GetPixel(0, 0);
    EXPECT_EQ(c.r, 100);
    EXPECT_EQ(c.g, 150);
    EXPECT_EQ(c.b, 200);
}

// ============================================================================
// ByteSwapFrameBuffer Tests
// ============================================================================

TEST_F(RenderSystemSetupTest, ByteSwapFrameBuffer_SwapsRGB565Bytes)
{
    DekiRenderSystem rs;
    rs.Setup(2, 1, DekiColorFormat::RGB565);

    // Write a known pixel directly
    uint16_t* buf = const_cast<uint16_t*>(
        reinterpret_cast<const uint16_t*>(rs.GetFrameBuffer()));
    buf[0] = 0x1234;
    buf[1] = 0xABCD;

    rs.ByteSwapFrameBuffer();

    EXPECT_EQ(buf[0], 0x3412);
    EXPECT_EQ(buf[1], 0xCDAB);
}

TEST_F(RenderSystemSetupTest, ByteSwapFrameBuffer_DoubleSwapRestoresOriginal)
{
    DekiRenderSystem rs;
    rs.Setup(2, 1, DekiColorFormat::RGB565);

    uint16_t* buf = const_cast<uint16_t*>(
        reinterpret_cast<const uint16_t*>(rs.GetFrameBuffer()));
    buf[0] = 0x5678;
    buf[1] = 0x9ABC;

    rs.ByteSwapFrameBuffer();
    rs.ByteSwapFrameBuffer();

    EXPECT_EQ(buf[0], 0x5678);
    EXPECT_EQ(buf[1], 0x9ABC);
}

TEST_F(RenderSystemSetupTest, ByteSwapFrameBuffer_NonRGB565_NoOp)
{
    DekiRenderSystem rs;
    rs.Setup(2, 1, DekiColorFormat::RGB888);

    const uint8_t* buf = rs.GetFrameBuffer();
    // Write known values
    uint8_t* mbuf = const_cast<uint8_t*>(buf);
    mbuf[0] = 0x12; mbuf[1] = 0x34; mbuf[2] = 0x56;
    mbuf[3] = 0x78; mbuf[4] = 0x9A; mbuf[5] = 0xBC;

    rs.ByteSwapFrameBuffer();

    // Should be unchanged (byte swap only applies to RGB565)
    EXPECT_EQ(mbuf[0], 0x12);
    EXPECT_EQ(mbuf[1], 0x34);
    EXPECT_EQ(mbuf[2], 0x56);
}

// ============================================================================
// Renderer Management Tests
// ============================================================================

TEST_F(RenderSystemSetupTest, SetRenderer_GetRenderer)
{
    DekiRenderSystem rs;
    EXPECT_EQ(rs.GetRenderer(), nullptr);

    // We can't easily create a real DekiRenderer without more infrastructure,
    // but we can verify null handling
    rs.SetRenderer(nullptr);
    EXPECT_EQ(rs.GetRenderer(), nullptr);
}

TEST_F(RenderSystemSetupTest, RenderWithNullPrefab_NoOp)
{
    DekiRenderSystem rs;
    rs.Setup(4, 4, DekiColorFormat::RGB565);

    // Should not crash
    rs.Render(nullptr);
}

TEST_F(RenderSystemSetupTest, RenderWithNoRenderer_NoOp)
{
    DekiRenderSystem rs;
    rs.Setup(4, 4, DekiColorFormat::RGB565);
    rs.SetRenderer(nullptr);

    // Should not crash (no renderer set)
    // Note: We can't pass a real Prefab easily, but nullptr covers the null check
    rs.Render(nullptr);
}
