/**
 * @file QuadBlitTests.cpp
 * @brief Unit tests for QuadBlit namespace (clip stack, MakeSource, blitting)
 */

#include <gtest/gtest.h>
#include <cstring>
#include <cstdint>
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
    }

    void TearDown() override
    {
        QuadBlit::ClearClipStack();
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
// Kernel dispatch tests
// ============================================================================
// Verify that when a row kernel is registered AND alignment preconditions
// hold, the dispatcher invokes it; otherwise the scalar inner loop runs.

namespace {

inline uint16_t MakeRGB565Free(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// Counts kernel invocations and writes a deterministic marker into dst so
// tests can distinguish kernel output from the scalar path's output.
struct KernelProbe
{
    static int s_callCount;
    static uint16_t s_marker;
    static int32_t s_lastPixelCount;

    static void Reset() { s_callCount = 0; s_lastPixelCount = 0; }

    static void CopyRowMarker(const uint8_t* /*src*/, uint8_t* dst, int32_t pixelCount,
                              uint8_t, uint8_t, uint8_t, uint8_t)
    {
        ++s_callCount;
        s_lastPixelCount = pixelCount;
        uint16_t* d = reinterpret_cast<uint16_t*>(dst);
        for (int32_t i = 0; i < pixelCount; ++i) d[i] = s_marker;
    }

    static void BlendRowMarker(const uint8_t* /*src*/, uint8_t* dst, int32_t pixelCount,
                               uint8_t, uint8_t, uint8_t, uint8_t)
    {
        ++s_callCount;
        s_lastPixelCount = pixelCount;
        uint16_t* d = reinterpret_cast<uint16_t*>(dst);
        for (int32_t i = 0; i < pixelCount; ++i) d[i] = s_marker;
    }
};
int KernelProbe::s_callCount = 0;
uint16_t KernelProbe::s_marker = 0;
int32_t KernelProbe::s_lastPixelCount = 0;

// Allocates a 16-byte-aligned buffer (heap, freed via aligned-free helper).
struct AlignedBuf
{
    uint8_t* base;
    uint8_t* aligned;

    explicit AlignedBuf(size_t bytes)
    {
        base = new uint8_t[bytes + 16];
        std::memset(base, 0, bytes + 16);
        uintptr_t a = (reinterpret_cast<uintptr_t>(base) + 15) & ~uintptr_t(15);
        aligned = reinterpret_cast<uint8_t*>(a);
    }
    ~AlignedBuf() { delete[] base; }
};

} // namespace

class QuadBlitKernelDispatchTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        QuadBlit::ClearClipStack();
        // Clear any previously registered kernels.
        QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565_Copy_Row, nullptr);
        QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565A8_Blend_Row, nullptr);
        KernelProbe::Reset();
    }

    void TearDown() override
    {
        QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565_Copy_Row, nullptr);
        QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565A8_Blend_Row, nullptr);
        QuadBlit::ClearClipStack();
    }
};

TEST_F(QuadBlitKernelDispatchTest, RGB565CopyRow_UsesKernel_WhenAligned)
{
    // 16-pixel-wide source/dest at 16-byte alignment.
    const int W = 16, H = 1;
    AlignedBuf srcBuf(W * H * 2);
    AlignedBuf dstBuf(W * H * 2);
    // Fill source with a recognisable scalar value so we can prove the kernel
    // (which writes a different marker) actually ran.
    uint16_t scalarValue = MakeRGB565Free(64, 128, 192);
    auto* srcPx = reinterpret_cast<uint16_t*>(srcBuf.aligned);
    for (int i = 0; i < W * H; ++i) srcPx[i] = scalarValue;

    KernelProbe::s_marker = MakeRGB565Free(255, 0, 255);  // distinct
    QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565_Copy_Row, &KernelProbe::CopyRowMarker);

    QuadBlit::Source src = QuadBlit::MakeSource(srcBuf.aligned, W, H, 2, false, true, false);
    QuadBlit::BlitScaled(src, dstBuf.aligned, W, H, DekiColorFormat::RGB565,
                         0, 0, W, H);

    EXPECT_EQ(KernelProbe::s_callCount, H);
    EXPECT_EQ(KernelProbe::s_lastPixelCount, W);
    auto* dstPx = reinterpret_cast<uint16_t*>(dstBuf.aligned);
    for (int i = 0; i < W; ++i)
        EXPECT_EQ(dstPx[i], KernelProbe::s_marker) << "pixel " << i;
}

TEST_F(QuadBlitKernelDispatchTest, RGB565CopyRow_SkipsKernel_WhenSourceMisaligned)
{
    const int W = 16, H = 1;
    AlignedBuf srcBuf(W * H * 2 + 16);
    AlignedBuf dstBuf(W * H * 2);
    // Misalign the source by 2 bytes (still 2-byte aligned for uint16_t reads,
    // but no longer 16-byte aligned).
    uint8_t* srcMisaligned = srcBuf.aligned + 2;
    uint16_t scalarValue = MakeRGB565Free(64, 128, 192);
    auto* srcPx = reinterpret_cast<uint16_t*>(srcMisaligned);
    for (int i = 0; i < W * H; ++i) srcPx[i] = scalarValue;

    KernelProbe::s_marker = MakeRGB565Free(255, 0, 255);
    QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565_Copy_Row, &KernelProbe::CopyRowMarker);

    QuadBlit::Source src = QuadBlit::MakeSource(srcMisaligned, W, H, 2, false, true, false);
    QuadBlit::BlitScaled(src, dstBuf.aligned, W, H, DekiColorFormat::RGB565,
                         0, 0, W, H);

    // Kernel must NOT have been called — alignment precondition failed.
    EXPECT_EQ(KernelProbe::s_callCount, 0);
    // Output must match the scalar copy (same as source).
    auto* dstPx = reinterpret_cast<uint16_t*>(dstBuf.aligned);
    for (int i = 0; i < W; ++i)
        EXPECT_EQ(dstPx[i], scalarValue) << "pixel " << i;
}

TEST_F(QuadBlitKernelDispatchTest, RGB565CopyRow_NoKernel_RunsScalar)
{
    // No kernel registered — bytes must be copied verbatim by the scalar path.
    const int W = 8, H = 1;
    AlignedBuf srcBuf(W * H * 2);
    AlignedBuf dstBuf(W * H * 2);
    auto* srcPx = reinterpret_cast<uint16_t*>(srcBuf.aligned);
    for (int i = 0; i < W * H; ++i)
        srcPx[i] = MakeRGB565Free(static_cast<uint8_t>(i * 16), 0, 0);

    QuadBlit::Source src = QuadBlit::MakeSource(srcBuf.aligned, W, H, 2, false, true, false);
    QuadBlit::BlitScaled(src, dstBuf.aligned, W, H, DekiColorFormat::RGB565,
                         0, 0, W, H);

    EXPECT_EQ(KernelProbe::s_callCount, 0);
    auto* dstPx = reinterpret_cast<uint16_t*>(dstBuf.aligned);
    for (int i = 0; i < W; ++i)
        EXPECT_EQ(dstPx[i], srcPx[i]);
}

TEST_F(QuadBlitKernelDispatchTest, RGB565A8BlendRow_UsesKernel_WhenAligned_AndUntinted)
{
    // Source is RGB565A8 (3 bytes/pixel). At 1:1 with hasAlpha=true and no
    // tint, no chroma key, no row-spans, the blend kernel slot should be
    // consulted.
    const int W = 16, H = 1;
    AlignedBuf srcBuf(W * H * 3 + 16);
    AlignedBuf dstBuf(W * H * 2);
    // Fill source with non-zero alpha so the per-pixel else branch is reached.
    for (int i = 0; i < W * H; ++i)
    {
        srcBuf.aligned[i * 3 + 0] = 0x12;  // RGB565 lo
        srcBuf.aligned[i * 3 + 1] = 0x34;  // RGB565 hi
        srcBuf.aligned[i * 3 + 2] = 200;   // alpha (any value > 0 keeps us off the early-skip)
    }

    KernelProbe::s_marker = MakeRGB565Free(0, 255, 255);
    QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565A8_Blend_Row, &KernelProbe::BlendRowMarker);

    QuadBlit::Source src = QuadBlit::MakeSource(srcBuf.aligned, W, H, 3, /*hasAlpha=*/true, /*isRGB565=*/true, false);
    QuadBlit::BlitScaled(src, dstBuf.aligned, W, H, DekiColorFormat::RGB565,
                         0, 0, W, H);

    EXPECT_EQ(KernelProbe::s_callCount, H);
    EXPECT_EQ(KernelProbe::s_lastPixelCount, W);
    auto* dstPx = reinterpret_cast<uint16_t*>(dstBuf.aligned);
    for (int i = 0; i < W; ++i)
        EXPECT_EQ(dstPx[i], KernelProbe::s_marker) << "pixel " << i;
}

TEST_F(QuadBlitKernelDispatchTest, RGB565A8BlendRow_SkipsKernel_WhenTinted)
{
    // Tinted blend must take the scalar path even with alignment satisfied.
    const int W = 16, H = 1;
    AlignedBuf srcBuf(W * H * 3 + 16);
    AlignedBuf dstBuf(W * H * 2);
    for (int i = 0; i < W * H; ++i)
    {
        srcBuf.aligned[i * 3 + 0] = 0x00;
        srcBuf.aligned[i * 3 + 1] = 0x00;
        srcBuf.aligned[i * 3 + 2] = 255;
    }

    KernelProbe::s_marker = MakeRGB565Free(0, 255, 255);
    QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565A8_Blend_Row, &KernelProbe::BlendRowMarker);

    QuadBlit::Source src = QuadBlit::MakeSource(srcBuf.aligned, W, H, 3, true, true, false);
    // Apply a non-identity tint -> precondition fails -> kernel must not run.
    QuadBlit::BlitScaled(src, dstBuf.aligned, W, H, DekiColorFormat::RGB565,
                         0, 0, W, H, 128, 128, 128, 255);

    EXPECT_EQ(KernelProbe::s_callCount, 0);
}

