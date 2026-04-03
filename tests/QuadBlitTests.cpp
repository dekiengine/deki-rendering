/**
 * @file QuadBlitTests.cpp
 * @brief Unit tests for QuadBlit namespace (clip stack, MakeSource, blitting)
 */

#include <gtest/gtest.h>
#include "QuadBlit.h"
#include "DekiEngine.h"  // For DekiColorFormat

// ============================================================================
// Clip Rect Stack Tests
// ============================================================================

class QuadBlitClipTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        QuadBlit::ClearClipStack();
    }

    void TearDown() override
    {
        QuadBlit::ClearClipStack();
    }
};

TEST_F(QuadBlitClipTest, DefaultClipRectIsUnset)
{
    QuadBlit::ClipRect rect = QuadBlit::GetCurrentClipRect();
    EXPECT_FALSE(rect.IsSet());
    EXPECT_EQ(rect.left, 0);
    EXPECT_EQ(rect.top, 0);
    EXPECT_EQ(rect.right, INT32_MAX);
    EXPECT_EQ(rect.bottom, INT32_MAX);
}

TEST_F(QuadBlitClipTest, PushClipRectSetsActiveRect)
{
    QuadBlit::PushClipRect(10, 20, 100, 200);
    QuadBlit::ClipRect rect = QuadBlit::GetCurrentClipRect();
    EXPECT_TRUE(rect.IsSet());
    EXPECT_EQ(rect.left, 10);
    EXPECT_EQ(rect.top, 20);
    EXPECT_EQ(rect.right, 100);
    EXPECT_EQ(rect.bottom, 200);
}

TEST_F(QuadBlitClipTest, PopClipRectRestoresDefault)
{
    QuadBlit::PushClipRect(10, 20, 100, 200);
    QuadBlit::PopClipRect();
    QuadBlit::ClipRect rect = QuadBlit::GetCurrentClipRect();
    EXPECT_FALSE(rect.IsSet());
}

TEST_F(QuadBlitClipTest, NestedClipsIntersect)
{
    // Parent: 10,10 → 100,100
    QuadBlit::PushClipRect(10, 10, 100, 100);
    // Child: 50,50 → 200,200 → should be clamped to 50,50 → 100,100
    QuadBlit::PushClipRect(50, 50, 200, 200);

    QuadBlit::ClipRect rect = QuadBlit::GetCurrentClipRect();
    EXPECT_EQ(rect.left, 50);
    EXPECT_EQ(rect.top, 50);
    EXPECT_EQ(rect.right, 100);
    EXPECT_EQ(rect.bottom, 100);
}

TEST_F(QuadBlitClipTest, PopNestedClipRestoresParent)
{
    QuadBlit::PushClipRect(10, 10, 100, 100);
    QuadBlit::PushClipRect(50, 50, 200, 200);
    QuadBlit::PopClipRect();

    QuadBlit::ClipRect rect = QuadBlit::GetCurrentClipRect();
    EXPECT_EQ(rect.left, 10);
    EXPECT_EQ(rect.top, 10);
    EXPECT_EQ(rect.right, 100);
    EXPECT_EQ(rect.bottom, 100);
}

TEST_F(QuadBlitClipTest, ClearClipStackResetsEverything)
{
    QuadBlit::PushClipRect(10, 10, 100, 100);
    QuadBlit::PushClipRect(20, 20, 80, 80);
    QuadBlit::ClearClipStack();

    QuadBlit::ClipRect rect = QuadBlit::GetCurrentClipRect();
    EXPECT_FALSE(rect.IsSet());
}

TEST_F(QuadBlitClipTest, PopOnEmptyStackIsNoOp)
{
    // Should not crash
    QuadBlit::PopClipRect();
    QuadBlit::PopClipRect();
    QuadBlit::ClipRect rect = QuadBlit::GetCurrentClipRect();
    EXPECT_FALSE(rect.IsSet());
}

TEST_F(QuadBlitClipTest, SetClipEnabledToggles)
{
    EXPECT_TRUE(QuadBlit::IsClipEnabled());

    QuadBlit::SetClipEnabled(false);
    EXPECT_FALSE(QuadBlit::IsClipEnabled());

    QuadBlit::SetClipEnabled(true);
    EXPECT_TRUE(QuadBlit::IsClipEnabled());
}

TEST_F(QuadBlitClipTest, DisabledClipReturnsDefaultRect)
{
    QuadBlit::PushClipRect(10, 10, 100, 100);
    QuadBlit::SetClipEnabled(false);

    // Should return default (unset) rect when disabled
    QuadBlit::ClipRect rect = QuadBlit::GetCurrentClipRect();
    EXPECT_FALSE(rect.IsSet());
}

TEST_F(QuadBlitClipTest, ClearClipStackReEnablesClipping)
{
    QuadBlit::SetClipEnabled(false);
    QuadBlit::ClearClipStack();
    EXPECT_TRUE(QuadBlit::IsClipEnabled());
}

// ============================================================================
// MakeSource Tests
// ============================================================================

class QuadBlitSourceTest : public ::testing::Test {};

TEST_F(QuadBlitSourceTest, RGB565OpaqueSource)
{
    uint8_t pixels[4] = {0};
    QuadBlit::Source src = QuadBlit::MakeSource(pixels, 1, 1, 2, false, true);

    EXPECT_EQ(src.pixels, pixels);
    EXPECT_EQ(src.width, 1);
    EXPECT_EQ(src.height, 1);
    EXPECT_EQ(src.bytesPerPixel, 2);
    EXPECT_FALSE(src.hasAlpha);
    EXPECT_TRUE(src.isRGB565);
    EXPECT_TRUE(src.ownsPixels);  // default
    EXPECT_EQ(src.alphaOffset, 0);
}

TEST_F(QuadBlitSourceTest, RGBA8888Source)
{
    uint8_t pixels[4] = {0};
    QuadBlit::Source src = QuadBlit::MakeSource(pixels, 2, 3, 4, true, false);

    EXPECT_EQ(src.width, 2);
    EXPECT_EQ(src.height, 3);
    EXPECT_EQ(src.bytesPerPixel, 4);
    EXPECT_TRUE(src.hasAlpha);
    EXPECT_FALSE(src.isRGB565);
    EXPECT_EQ(src.alphaOffset, 3);  // RGBA: alpha at offset 3
}

TEST_F(QuadBlitSourceTest, RGB565A8Source)
{
    uint8_t pixels[3] = {0};
    QuadBlit::Source src = QuadBlit::MakeSource(pixels, 1, 1, 3, true, true);

    EXPECT_TRUE(src.hasAlpha);
    EXPECT_TRUE(src.isRGB565);
    EXPECT_EQ(src.alphaOffset, 2);  // RGB565A8: alpha at offset 2
}

TEST_F(QuadBlitSourceTest, OwnsPixelsFlagPassedThrough)
{
    uint8_t pixels[4] = {0};

    QuadBlit::Source owned = QuadBlit::MakeSource(pixels, 1, 1, 2, false, true, true);
    EXPECT_TRUE(owned.ownsPixels);

    QuadBlit::Source borrowed = QuadBlit::MakeSource(pixels, 1, 1, 2, false, true, false);
    EXPECT_FALSE(borrowed.ownsPixels);
}

// ============================================================================
// Blit / BlitScaled Tests
// ============================================================================

class QuadBlitPixelTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        QuadBlit::ClearClipStack();
        QuadBlit::SetByteSwap(false);
    }

    void TearDown() override
    {
        QuadBlit::ClearClipStack();
        QuadBlit::SetByteSwap(false);
    }

    // Helper: encode RGB565 pixel
    static uint16_t MakeRGB565(uint8_t r, uint8_t g, uint8_t b)
    {
        return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    }

    // Helper: read RGB565 pixel from buffer at (x, y)
    static uint16_t ReadRGB565(const uint8_t* buf, int32_t w, int32_t x, int32_t y)
    {
        return *reinterpret_cast<const uint16_t*>(buf + (y * w + x) * 2);
    }
};

TEST_F(QuadBlitPixelTest, BlitScaled_RGB565_1x1_Opaque)
{
    // Source: 1x1 red pixel
    uint16_t srcPixel = MakeRGB565(255, 0, 0);
    QuadBlit::Source src = QuadBlit::MakeSource(
        reinterpret_cast<const uint8_t*>(&srcPixel), 1, 1, 2, false, true, false);

    // Target: 4x4 buffer, all black
    const int W = 4, H = 4;
    uint8_t target[W * H * 2] = {0};

    // Blit at (1, 1), no scaling
    QuadBlit::BlitScaled(src, target, W, H, DekiColorFormat::RGB565,
                         1, 1, 1, 1);

    // Pixel at (1,1) should be red
    EXPECT_EQ(ReadRGB565(target, W, 1, 1), srcPixel);

    // Pixel at (0,0) should remain black
    EXPECT_EQ(ReadRGB565(target, W, 0, 0), 0);

    // Pixel at (2,2) should remain black
    EXPECT_EQ(ReadRGB565(target, W, 2, 2), 0);
}

TEST_F(QuadBlitPixelTest, BlitScaled_RGB565_2x2_AtOrigin)
{
    // Source: 2x2 with distinct pixels
    uint16_t srcPixels[4] = {
        MakeRGB565(255, 0, 0), MakeRGB565(0, 255, 0),
        MakeRGB565(0, 0, 255), MakeRGB565(255, 255, 0),
    };
    QuadBlit::Source src = QuadBlit::MakeSource(
        reinterpret_cast<const uint8_t*>(srcPixels), 2, 2, 2, false, true, false);

    const int W = 4, H = 4;
    uint8_t target[W * H * 2] = {0};

    QuadBlit::BlitScaled(src, target, W, H, DekiColorFormat::RGB565,
                         0, 0, 2, 2);

    EXPECT_EQ(ReadRGB565(target, W, 0, 0), srcPixels[0]);
    EXPECT_EQ(ReadRGB565(target, W, 1, 0), srcPixels[1]);
    EXPECT_EQ(ReadRGB565(target, W, 0, 1), srcPixels[2]);
    EXPECT_EQ(ReadRGB565(target, W, 1, 1), srcPixels[3]);

    // (2,0) should be black
    EXPECT_EQ(ReadRGB565(target, W, 2, 0), 0);
}

TEST_F(QuadBlitPixelTest, BlitScaled_FullyOutOfBounds_NoWrite)
{
    uint16_t srcPixel = MakeRGB565(255, 0, 0);
    QuadBlit::Source src = QuadBlit::MakeSource(
        reinterpret_cast<const uint8_t*>(&srcPixel), 1, 1, 2, false, true, false);

    const int W = 4, H = 4;
    uint8_t target[W * H * 2] = {0};

    // Blit at (10, 10) — completely outside 4x4 buffer
    QuadBlit::BlitScaled(src, target, W, H, DekiColorFormat::RGB565,
                         10, 10, 1, 1);

    // All pixels should remain black
    for (int i = 0; i < W * H; i++)
    {
        EXPECT_EQ(reinterpret_cast<uint16_t*>(target)[i], 0)
            << "pixel " << i << " should be 0";
    }
}

TEST_F(QuadBlitPixelTest, Blit_WithTintAlphaZero_NoWrite)
{
    uint16_t srcPixel = MakeRGB565(255, 0, 0);
    QuadBlit::Source src = QuadBlit::MakeSource(
        reinterpret_cast<const uint8_t*>(&srcPixel), 1, 1, 2, false, true, false);

    const int W = 4, H = 4;
    uint8_t target[W * H * 2] = {0};

    // Blit with tintA=0 (invisible)
    QuadBlit::Blit(src, target, W, H, DekiColorFormat::RGB565,
                   1, 1, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
                   255, 255, 255, 0);

    // All pixels should remain black
    for (int i = 0; i < W * H; i++)
    {
        EXPECT_EQ(reinterpret_cast<uint16_t*>(target)[i], 0);
    }
}

TEST_F(QuadBlitPixelTest, BlitScaled_ClipRectRestrictsOutput)
{
    // Source: 2x2 all red
    uint16_t red = MakeRGB565(255, 0, 0);
    uint16_t srcPixels[4] = {red, red, red, red};
    QuadBlit::Source src = QuadBlit::MakeSource(
        reinterpret_cast<const uint8_t*>(srcPixels), 2, 2, 2, false, true, false);

    const int W = 4, H = 4;
    uint8_t target[W * H * 2] = {0};

    // Clip to only (0,0)→(1,1) — only top-left pixel should be written
    QuadBlit::PushClipRect(0, 0, 1, 1);

    QuadBlit::BlitScaled(src, target, W, H, DekiColorFormat::RGB565,
                         0, 0, 2, 2);

    // (0,0) should be red (inside clip)
    EXPECT_EQ(ReadRGB565(target, W, 0, 0), red);

    // (1,0), (0,1), (1,1) should be black (outside clip)
    EXPECT_EQ(ReadRGB565(target, W, 1, 0), 0);
    EXPECT_EQ(ReadRGB565(target, W, 0, 1), 0);
    EXPECT_EQ(ReadRGB565(target, W, 1, 1), 0);
}

TEST_F(QuadBlitPixelTest, BlitScaled_1x1_To_2x2_Upscale)
{
    // Source: 1x1 green pixel
    uint16_t green = MakeRGB565(0, 255, 0);
    QuadBlit::Source src = QuadBlit::MakeSource(
        reinterpret_cast<const uint8_t*>(&green), 1, 1, 2, false, true, false);

    const int W = 4, H = 4;
    uint8_t target[W * H * 2] = {0};

    // Scale 1x1 to 2x2 at (0,0)
    QuadBlit::BlitScaled(src, target, W, H, DekiColorFormat::RGB565,
                         0, 0, 2, 2);

    // All 4 pixels in the 2x2 dest should be green
    EXPECT_EQ(ReadRGB565(target, W, 0, 0), green);
    EXPECT_EQ(ReadRGB565(target, W, 1, 0), green);
    EXPECT_EQ(ReadRGB565(target, W, 0, 1), green);
    EXPECT_EQ(ReadRGB565(target, W, 1, 1), green);

    // (2,0) should be black
    EXPECT_EQ(ReadRGB565(target, W, 2, 0), 0);
}

// ============================================================================
// ByteSwap Tests
// ============================================================================

TEST_F(QuadBlitPixelTest, ByteSwapToggle)
{
    EXPECT_FALSE(QuadBlit::GetByteSwap());
    QuadBlit::SetByteSwap(true);
    EXPECT_TRUE(QuadBlit::GetByteSwap());
    QuadBlit::SetByteSwap(false);
    EXPECT_FALSE(QuadBlit::GetByteSwap());
}
