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
    QuadBlit::BlitScaled(src, target, W, H, Deki::ColorFormat::RGB565,
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

    QuadBlit::BlitScaled(src, target, W, H, Deki::ColorFormat::RGB565,
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
    QuadBlit::BlitScaled(src, target, W, H, Deki::ColorFormat::RGB565,
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
    QuadBlit::Blit(src, target, W, H, Deki::ColorFormat::RGB565,
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

    QuadBlit::BlitScaled(src, target, W, H, Deki::ColorFormat::RGB565,
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
    QuadBlit::BlitScaled(src, target, W, H, Deki::ColorFormat::RGB565,
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
    QuadBlit::BlitScaled(src, dstBuf.aligned, W, H, Deki::ColorFormat::RGB565,
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
    QuadBlit::BlitScaled(src, dstBuf.aligned, W, H, Deki::ColorFormat::RGB565,
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
    QuadBlit::BlitScaled(src, dstBuf.aligned, W, H, Deki::ColorFormat::RGB565,
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
    QuadBlit::BlitScaled(src, dstBuf.aligned, W, H, Deki::ColorFormat::RGB565,
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
    QuadBlit::BlitScaled(src, dstBuf.aligned, W, H, Deki::ColorFormat::RGB565,
                         0, 0, W, H, 128, 128, 128, 255);

    EXPECT_EQ(KernelProbe::s_callCount, 0);
}

// ============================================================================
// RGB565A8 target tests
// ============================================================================
// Cover the new BlitScaled_*_to_RGB565A8 paths and their kernel-dispatch slots.

class QuadBlitRGB565A8TargetTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        QuadBlit::ClearClipStack();
        QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565_to_RGB565A8_Row, nullptr);
        QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565A8_Copy_Row, nullptr);
        QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565A8_Blend_Row_Dest_RGB565A8, nullptr);
    }
    void TearDown() override
    {
        QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565_to_RGB565A8_Row, nullptr);
        QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565A8_Copy_Row, nullptr);
        QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565A8_Blend_Row_Dest_RGB565A8, nullptr);
        QuadBlit::ClearClipStack();
    }
};

TEST_F(QuadBlitRGB565A8TargetTest, RGB565A8_to_RGB565A8_Opaque_Copies_RGB_AndSetsAlpha)
{
    // 2x1 source, RGB565A8 with alpha = 255 per pixel.
    const int W = 2, H = 1;
    uint8_t src[W * 3];
    uint16_t pix0 = MakeRGB565Free(255, 0, 0);   // red
    uint16_t pix1 = MakeRGB565Free(0, 255, 0);   // green
    src[0] = (uint8_t)(pix0 & 0xFF); src[1] = (uint8_t)((pix0 >> 8) & 0xFF); src[2] = 255;
    src[3] = (uint8_t)(pix1 & 0xFF); src[4] = (uint8_t)((pix1 >> 8) & 0xFF); src[5] = 255;

    uint8_t target[W * H * 3] = {0};

    QuadBlit::Source s = QuadBlit::MakeSource(src, W, H, 3, /*hasAlpha=*/true, /*isRGB565=*/true, false);
    QuadBlit::BlitScaled(s, target, W, H, Deki::ColorFormat::RGB565A8, 0, 0, W, H);

    EXPECT_EQ(target[0], src[0]);
    EXPECT_EQ(target[1], src[1]);
    EXPECT_EQ(target[2], 255);
    EXPECT_EQ(target[3], src[3]);
    EXPECT_EQ(target[4], src[4]);
    EXPECT_EQ(target[5], 255);
}

TEST_F(QuadBlitRGB565A8TargetTest, RGB565A8_to_RGB565A8_AlphaZero_LeavesTargetUnchanged)
{
    // Source has alpha=0 — should be a no-op for that pixel.
    const int W = 1, H = 1;
    uint16_t pix = MakeRGB565Free(255, 0, 0);
    uint8_t src[3] = { (uint8_t)(pix & 0xFF), (uint8_t)((pix >> 8) & 0xFF), 0 };

    // Pre-fill target with a recognisable pattern.
    uint8_t target[W * H * 3] = { 0xAB, 0xCD, 0xEF };

    QuadBlit::Source s = QuadBlit::MakeSource(src, W, H, 3, true, true, false);
    QuadBlit::BlitScaled(s, target, W, H, Deki::ColorFormat::RGB565A8, 0, 0, W, H);

    EXPECT_EQ(target[0], 0xAB);
    EXPECT_EQ(target[1], 0xCD);
    EXPECT_EQ(target[2], 0xEF);
}

TEST_F(QuadBlitRGB565A8TargetTest, RGB565A8_to_RGB565A8_PartialAlpha_OntoCleared_PreservesSrcAlpha)
{
    // src.a = 128, dst initially zeroed (alpha=0). After src-over alpha union,
    // out.a = src.a + dst.a*(255-src.a)/255 = 128 + 0 = 128.
    const int W = 1, H = 1;
    uint16_t pix = MakeRGB565Free(255, 255, 255);  // white
    uint8_t src[3] = { (uint8_t)(pix & 0xFF), (uint8_t)((pix >> 8) & 0xFF), 128 };
    uint8_t target[W * H * 3] = {0};

    QuadBlit::Source s = QuadBlit::MakeSource(src, W, H, 3, true, true, false);
    QuadBlit::BlitScaled(s, target, W, H, Deki::ColorFormat::RGB565A8, 0, 0, W, H);

    EXPECT_EQ(target[2], 128) << "out.a should equal src.a when dst.a was 0";
}

TEST_F(QuadBlitRGB565A8TargetTest, RGB565_to_RGB565A8_SetsAlphaTo255)
{
    // Pure RGB565 source has no alpha; target alpha byte should be 0xFF.
    const int W = 2, H = 1;
    uint16_t srcPx[W];
    srcPx[0] = MakeRGB565Free(255, 0, 0);
    srcPx[1] = MakeRGB565Free(0, 0, 255);

    uint8_t target[W * H * 3] = {0};

    QuadBlit::Source s = QuadBlit::MakeSource(
        reinterpret_cast<const uint8_t*>(srcPx), W, H, 2, /*hasAlpha=*/false, /*isRGB565=*/true, false);
    QuadBlit::BlitScaled(s, target, W, H, Deki::ColorFormat::RGB565A8, 0, 0, W, H);

    EXPECT_EQ(target[0], (uint8_t)(srcPx[0] & 0xFF));
    EXPECT_EQ(target[1], (uint8_t)((srcPx[0] >> 8) & 0xFF));
    EXPECT_EQ(target[2], 255);
    EXPECT_EQ(target[3], (uint8_t)(srcPx[1] & 0xFF));
    EXPECT_EQ(target[4], (uint8_t)((srcPx[1] >> 8) & 0xFF));
    EXPECT_EQ(target[5], 255);
}

// Kernel dispatch coverage for the new RGB565A8-target slots.

namespace {
struct RGB565A8KernelProbe
{
    static int s_callCount;
    static int32_t s_lastPixelCount;
    static uint8_t s_marker;
    static void Reset() { s_callCount = 0; s_lastPixelCount = 0; }
    static void Run(const uint8_t* /*src*/, uint8_t* dst, int32_t pixelCount,
                    uint8_t, uint8_t, uint8_t, uint8_t)
    {
        ++s_callCount;
        s_lastPixelCount = pixelCount;
        for (int32_t i = 0; i < pixelCount; ++i) {
            dst[i * 3]     = s_marker;
            dst[i * 3 + 1] = s_marker;
            dst[i * 3 + 2] = s_marker;
        }
    }
};
int RGB565A8KernelProbe::s_callCount = 0;
int32_t RGB565A8KernelProbe::s_lastPixelCount = 0;
uint8_t RGB565A8KernelProbe::s_marker = 0;
} // namespace

TEST_F(QuadBlitRGB565A8TargetTest, RGB565A8CopyRow_KernelInvoked_WhenAlignedAndOpaqueSource)
{
    // Source: hasAlpha=false → opaque-copy fast path.
    const int W = 16, H = 1;
    AlignedBuf srcBuf(W * H * 3 + 16);
    AlignedBuf dstBuf(W * H * 3);
    for (int i = 0; i < W * H; ++i)
    {
        srcBuf.aligned[i * 3 + 0] = 0x11;
        srcBuf.aligned[i * 3 + 1] = 0x22;
        srcBuf.aligned[i * 3 + 2] = 0xFF;
    }

    RGB565A8KernelProbe::Reset();
    RGB565A8KernelProbe::s_marker = 0x77;
    QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565A8_Copy_Row, &RGB565A8KernelProbe::Run);

    QuadBlit::Source s = QuadBlit::MakeSource(srcBuf.aligned, W, H, 3,
                                              /*hasAlpha=*/false, /*isRGB565=*/true, false);
    QuadBlit::BlitScaled(s, dstBuf.aligned, W, H, Deki::ColorFormat::RGB565A8, 0, 0, W, H);

    EXPECT_EQ(RGB565A8KernelProbe::s_callCount, H);
    EXPECT_EQ(RGB565A8KernelProbe::s_lastPixelCount, W);
    EXPECT_EQ(dstBuf.aligned[0], 0x77);
}

TEST_F(QuadBlitRGB565A8TargetTest, RGB565A8BlendRow_KernelInvoked_WhenAlignedAndAlphaSource)
{
    // Source: hasAlpha=true → alpha blend path; kernel slot consulted with no tint/key.
    const int W = 16, H = 1;
    AlignedBuf srcBuf(W * H * 3 + 16);
    AlignedBuf dstBuf(W * H * 3);
    for (int i = 0; i < W * H; ++i)
    {
        srcBuf.aligned[i * 3 + 0] = 0x33;
        srcBuf.aligned[i * 3 + 1] = 0x44;
        srcBuf.aligned[i * 3 + 2] = 200;  // partial alpha keeps us off the a==0/255 short-circuits
    }

    RGB565A8KernelProbe::Reset();
    RGB565A8KernelProbe::s_marker = 0x99;
    QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565A8_Blend_Row_Dest_RGB565A8, &RGB565A8KernelProbe::Run);

    QuadBlit::Source s = QuadBlit::MakeSource(srcBuf.aligned, W, H, 3, true, true, false);
    QuadBlit::BlitScaled(s, dstBuf.aligned, W, H, Deki::ColorFormat::RGB565A8, 0, 0, W, H);

    EXPECT_EQ(RGB565A8KernelProbe::s_callCount, H);
    EXPECT_EQ(RGB565A8KernelProbe::s_lastPixelCount, W);
}

TEST_F(QuadBlitRGB565A8TargetTest, RGB565ToRGB565A8_KernelInvoked_WhenAligned)
{
    const int W = 16, H = 1;
    AlignedBuf srcBuf(W * H * 2);
    AlignedBuf dstBuf(W * H * 3);
    auto* srcPx = reinterpret_cast<uint16_t*>(srcBuf.aligned);
    for (int i = 0; i < W * H; ++i) srcPx[i] = MakeRGB565Free((uint8_t)(i * 16), 0, 0);

    RGB565A8KernelProbe::Reset();
    RGB565A8KernelProbe::s_marker = 0x55;
    QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565_to_RGB565A8_Row, &RGB565A8KernelProbe::Run);

    QuadBlit::Source s = QuadBlit::MakeSource(srcBuf.aligned, W, H, 2,
                                              /*hasAlpha=*/false, /*isRGB565=*/true, false);
    QuadBlit::BlitScaled(s, dstBuf.aligned, W, H, Deki::ColorFormat::RGB565A8, 0, 0, W, H);

    EXPECT_EQ(RGB565A8KernelProbe::s_callCount, H);
    EXPECT_EQ(RGB565A8KernelProbe::s_lastPixelCount, W);
}

// ============================================================================
// Ordered-dither alpha tests
// ============================================================================
// Cover the new useOrderedDither path. Validates: opaque pixels still draw,
// fully-transparent pixels still skip, partial-alpha pixels follow the Bayer
// threshold pattern (no destination read, no blend math).

class QuadBlitDitherTest : public ::testing::Test
{
protected:
    void SetUp() override    { QuadBlit::ClearClipStack(); }
    void TearDown() override { QuadBlit::ClearClipStack(); }
};

TEST_F(QuadBlitDitherTest, OpaqueSrc_WritesAllPixels_RGB565A8_to_RGB565)
{
    // 4x1 RGB565A8 source with a==255 — every pixel must draw regardless of
    // Bayer threshold. Hits the specialized RGB565A8→RGB565 dither path.
    const int W = 4, H = 1;
    uint8_t src[W * H * 3];
    uint16_t color = MakeRGB565Free(255, 128, 0);
    for (int i = 0; i < W * H; ++i) {
        src[i * 3]     = (uint8_t)(color & 0xFF);
        src[i * 3 + 1] = (uint8_t)((color >> 8) & 0xFF);
        src[i * 3 + 2] = 255;  // fully opaque
    }
    uint8_t target[W * H * 2] = {0};

    QuadBlit::Source s = QuadBlit::MakeSource(src, W, H, 3, /*hasAlpha=*/true, /*isRGB565=*/true, false);
    QuadBlit::BlitScaled(s, target, W, H, Deki::ColorFormat::RGB565,
                         0, 0, W, H, 255, 255, 255, 255, /*useOrderedDither=*/true);

    auto* dst = reinterpret_cast<uint16_t*>(target);
    for (int i = 0; i < W * H; ++i)
        EXPECT_EQ(dst[i], color) << "pixel " << i << " should be opaque-written";
}

TEST_F(QuadBlitDitherTest, ZeroAlphaSrc_LeavesTargetUnchanged)
{
    // src.a = 0 → must skip every pixel even with dither active.
    const int W = 2, H = 1;
    uint8_t src[W * H * 3];
    uint16_t color = MakeRGB565Free(255, 0, 0);
    for (int i = 0; i < W * H; ++i) {
        src[i * 3]     = (uint8_t)(color & 0xFF);
        src[i * 3 + 1] = (uint8_t)((color >> 8) & 0xFF);
        src[i * 3 + 2] = 0;  // fully transparent
    }
    uint16_t pre = MakeRGB565Free(0, 0, 0);
    uint16_t target[W * H] = { pre, pre };

    QuadBlit::Source s = QuadBlit::MakeSource(src, W, H, 3, true, true, false);
    QuadBlit::BlitScaled(s, reinterpret_cast<uint8_t*>(target), W, H, Deki::ColorFormat::RGB565,
                         0, 0, W, H, 255, 255, 255, 255, /*useOrderedDither=*/true);

    EXPECT_EQ(target[0], pre);
    EXPECT_EQ(target[1], pre);
}

TEST_F(QuadBlitDitherTest, PartialAlpha_FollowsBayerThreshold)
{
    // 8x8 source filled with src.a = 128. The 8x8 Bayer matrix has values
    // 0..255 spread evenly; threshold-comparing 128 against the matrix should
    // pass for exactly half of the pixels. Each output pixel is either
    // src-RGB or untouched (no blend).
    const int W = 8, H = 8;
    uint8_t src[W * H * 3];
    uint16_t color = MakeRGB565Free(0, 255, 0);  // green
    for (int i = 0; i < W * H; ++i) {
        src[i * 3]     = (uint8_t)(color & 0xFF);
        src[i * 3 + 1] = (uint8_t)((color >> 8) & 0xFF);
        src[i * 3 + 2] = 128;
    }
    uint16_t bg = MakeRGB565Free(0, 0, 255);  // blue background — must NOT be blended
    uint16_t target[W * H];
    for (int i = 0; i < W * H; ++i) target[i] = bg;

    QuadBlit::Source s = QuadBlit::MakeSource(src, W, H, 3, true, true, false);
    QuadBlit::BlitScaled(s, reinterpret_cast<uint8_t*>(target), W, H, Deki::ColorFormat::RGB565,
                         0, 0, W, H, 255, 255, 255, 255, /*useOrderedDither=*/true);

    int wrote = 0, kept = 0;
    for (int i = 0; i < W * H; ++i) {
        if (target[i] == color) ++wrote;
        else if (target[i] == bg) ++kept;
        else FAIL() << "pixel " << i << " is " << target[i] << " — neither pure src nor pure bg, blend leaked";
    }
    // Bayer 8x8 has 64 unique values [0..255]. Pixels with threshold < 128 pass;
    // exactly half (32) by construction.
    EXPECT_EQ(wrote, 32);
    EXPECT_EQ(kept, 32);
}

TEST_F(QuadBlitDitherTest, GenericPath_RGB565A8_to_RGB565A8)
{
    // Hits the generic dither path (target != RGB565). Same threshold
    // semantics; just verify alpha=0 skip and alpha=255 write.
    const int W = 2, H = 1;
    uint8_t src[W * H * 3] = {
        0x00, 0xF8, 0xFF,   // red, opaque
        0x00, 0xF8, 0x00,   // red, transparent
    };
    uint8_t target[W * H * 3] = { 0x11, 0x22, 0x33,  0x44, 0x55, 0x66 };

    QuadBlit::Source s = QuadBlit::MakeSource(src, W, H, 3, true, true, false);
    QuadBlit::BlitScaled(s, target, W, H, Deki::ColorFormat::RGB565A8,
                         0, 0, W, H, 255, 255, 255, 255, /*useOrderedDither=*/true);

    EXPECT_EQ(target[0], 0x00);  // opaque pixel: src bytes written
    EXPECT_EQ(target[1], 0xF8);
    EXPECT_EQ(target[2], 0xFF);  // alpha = 0xFF on opaque write
    EXPECT_EQ(target[3], 0x44);  // transparent pixel: untouched
    EXPECT_EQ(target[4], 0x55);
    EXPECT_EQ(target[5], 0x66);
}


// ============================================================================
// Regressions: row-span edge cases, clip-stack overflow, rotation tint order
// ============================================================================

namespace
{
uint16_t Pack565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
uint16_t Read565(const uint8_t* buf, int w, int x, int y)
{
    return reinterpret_cast<const uint16_t*>(buf)[y * w + x];
}
// RGB565A8: 2 bytes of colour then 1 byte of alpha, per pixel.
void Put565A8(uint8_t* dst, int i, uint16_t rgb, uint8_t a)
{
    dst[i * 3 + 0] = static_cast<uint8_t>(rgb & 0xFF);
    dst[i * 3 + 1] = static_cast<uint8_t>(rgb >> 8);
    dst[i * 3 + 2] = a;
}
}  // namespace

class QuadBlitSpanRegressionTest : public ::testing::Test
{
protected:
    void SetUp() override { QuadBlit::ClearClipStack(); }
    void TearDown() override { QuadBlit::ClearClipStack(); }
};

// A row with no fully-opaque pixel carries an EMPTY span (start == end == w).
// With the old sentinel (start=w, end=0) the left region covered the whole row
// and the right region covered it again from x=0: every soft pixel was blended
// twice. The spanned blit must equal the unspanned one.
TEST_F(QuadBlitSpanRegressionTest, EmptyOpaqueSpan_BlendsEachPixelOnce)
{
    const int W = 4;
    uint8_t src[W * 3];
    for (int i = 0; i < W; i++) Put565A8(src, i, Pack565(255, 255, 255), 128);
    const int16_t emptySpan[2] = { W, W };

    uint8_t withSpans[W * 2] = {0}, withoutSpans[W * 2] = {0};
    QuadBlit::Source spanned = QuadBlit::MakeSource(src, W, 1, 3, true, true, false, emptySpan);
    QuadBlit::Source plain = QuadBlit::MakeSource(src, W, 1, 3, true, true, false, nullptr);
    QuadBlit::BlitScaled(spanned, withSpans, W, 1, Deki::ColorFormat::RGB565, 0, 0, W, 1);
    QuadBlit::BlitScaled(plain, withoutSpans, W, 1, Deki::ColorFormat::RGB565, 0, 0, W, 1);

    for (int x = 0; x < W; x++)
        EXPECT_EQ(Read565(withSpans, W, x, 0), Read565(withoutSpans, W, x, 0)) << "x=" << x;
    // Half-alpha white over black is mid grey, not white (which double blending gives).
    EXPECT_LT(Read565(withSpans, W, 0, 0), Pack565(200, 200, 200));
}

// The right-alpha region used to start at opaqueEnd even when the clip rect
// started further right, writing pixels the clip had excluded.
TEST_F(QuadBlitSpanRegressionTest, RightAlphaRegion_RespectsClipLeftEdge)
{
    const int W = 4;
    uint8_t src[W * 3];
    for (int i = 0; i < W; i++) Put565A8(src, i, Pack565(255, 255, 255), 128);
    const int16_t emptySpan[2] = { W, W };
    QuadBlit::Source spanned = QuadBlit::MakeSource(src, W, 1, 3, true, true, false, emptySpan);

    uint8_t target[W * 2] = {0};
    QuadBlit::PushClipRect(2, 0, W, 1);
    QuadBlit::BlitScaled(spanned, target, W, 1, Deki::ColorFormat::RGB565, 0, 0, W, 1);
    QuadBlit::PopClipRect();

    EXPECT_EQ(Read565(target, W, 0, 0), 0u) << "left of the clip rect must stay untouched";
    EXPECT_EQ(Read565(target, W, 1, 0), 0u);
    EXPECT_NE(Read565(target, W, 2, 0), 0u);
    EXPECT_NE(Read565(target, W, 3, 0), 0u);
}

// The clip stack has no fixed capacity: 40 nested levels (deeper than the old
// 16-slot array) all intersect and pop back out in order.
TEST_F(QuadBlitSpanRegressionTest, ClipStack_DeepNestingHasNoCap)
{
    ASSERT_EQ(QuadBlit::GetClipStackDepth(), 0);
    constexpr int kDepth = 40;
    for (int i = 0; i < kDepth; i++)
    {
        QuadBlit::PushClipRect(i, 0, 100, 100);  // shrinks by one column per level
        EXPECT_EQ(QuadBlit::GetClipStackDepth(), i + 1);
        EXPECT_EQ(QuadBlit::GetCurrentClipRect().left, i);  // intersected with the parent
    }
    for (int i = kDepth - 1; i >= 0; i--)
    {
        EXPECT_EQ(QuadBlit::GetCurrentClipRect().left, i);
        QuadBlit::PopClipRect();
    }
    EXPECT_EQ(QuadBlit::GetClipStackDepth(), 0);
    QuadBlit::PopClipRect();  // extra pop on an empty stack is harmless
    EXPECT_EQ(QuadBlit::GetClipStackDepth(), 0);
}

// The rotation path must tint the source BEFORE compositing, like every
// BlitScaled kernel. A full turn takes the rotated code path with the same
// geometry as no rotation, so the two must agree on the centre pixel.
TEST_F(QuadBlitSpanRegressionTest, RotationPath_TintsSourceBeforeBlend)
{
    const int S = 3;
    uint8_t src[S * S * 3];
    for (int i = 0; i < S * S; i++) Put565A8(src, i, Pack565(255, 255, 255), 128);
    QuadBlit::Source source = QuadBlit::MakeSource(src, S, S, 3, true, true, false);

    const int W = 9, H = 9;
    uint8_t straight[W * H * 2], turned[W * H * 2];
    // A bright, distinct background so a wrong blend order shows.
    for (int i = 0; i < W * H; i++)
    {
        reinterpret_cast<uint16_t*>(straight)[i] = Pack565(0, 0, 255);
        reinterpret_cast<uint16_t*>(turned)[i] = Pack565(0, 0, 255);
    }
    const float fullTurn = 6.28318530718f;
    QuadBlit::Blit(source, straight, W, H, Deki::ColorFormat::RGB565, 4, 4, 1.0f, 1.0f, 0.0f, 0.5f, 0.5f,
                   255, 0, 0, 255);
    QuadBlit::Blit(source, turned, W, H, Deki::ColorFormat::RGB565, 4, 4, 1.0f, 1.0f, fullTurn, 0.5f, 0.5f,
                   255, 0, 0, 255);

    EXPECT_EQ(Read565(turned, W, 4, 4), Read565(straight, W, 4, 4));
}

// ---------------------------------------------------------------------------
// Flips. Tiled-authored tiles carry H/V/D flags; they used to be expressed as
// negative destination sizes, which BlitScaled rejects outright.
// ---------------------------------------------------------------------------
TEST_F(QuadBlitSpanRegressionTest, FlipH_MirrorsColumns)
{
    uint16_t src[2] = { Pack565(255, 0, 0), Pack565(0, 255, 0) };
    QuadBlit::Source s = QuadBlit::MakeSource(reinterpret_cast<const uint8_t*>(src), 2, 1, 2, false, true, false);
    s.flipH = true;
    uint8_t target[2 * 2] = {0};
    QuadBlit::BlitScaled(s, target, 2, 1, Deki::ColorFormat::RGB565, 0, 0, 2, 1);
    EXPECT_EQ(Read565(target, 2, 0, 0), src[1]);
    EXPECT_EQ(Read565(target, 2, 1, 0), src[0]);
}

TEST_F(QuadBlitSpanRegressionTest, FlipV_MirrorsRows)
{
    uint16_t src[2] = { Pack565(255, 0, 0), Pack565(0, 255, 0) };
    QuadBlit::Source s = QuadBlit::MakeSource(reinterpret_cast<const uint8_t*>(src), 1, 2, 2, false, true, false);
    s.flipV = true;
    uint8_t target[1 * 2 * 2] = {0};
    QuadBlit::BlitScaled(s, target, 1, 2, Deki::ColorFormat::RGB565, 0, 0, 1, 2);
    EXPECT_EQ(Read565(target, 1, 0, 0), src[1]);
    EXPECT_EQ(Read565(target, 1, 0, 1), src[0]);
}

TEST_F(QuadBlitSpanRegressionTest, FlipD_Transposes)
{
    uint16_t src[4] = { Pack565(255, 0, 0), Pack565(0, 255, 0),
                        Pack565(0, 0, 255), Pack565(255, 255, 0) };
    QuadBlit::Source s = QuadBlit::MakeSource(reinterpret_cast<const uint8_t*>(src), 2, 2, 2, false, true, false);
    s.flipD = true;
    uint8_t target[2 * 2 * 2] = {0};
    QuadBlit::BlitScaled(s, target, 2, 2, Deki::ColorFormat::RGB565, 0, 0, 2, 2);
    EXPECT_EQ(Read565(target, 2, 0, 0), src[0]);
    EXPECT_EQ(Read565(target, 2, 1, 0), src[2]);  // (1,0) samples source (0,1)
    EXPECT_EQ(Read565(target, 2, 0, 1), src[1]);
    EXPECT_EQ(Read565(target, 2, 1, 1), src[3]);
}

TEST_F(QuadBlitSpanRegressionTest, NegativeSize_IsStillRejected)
{
    uint16_t src[1] = { Pack565(255, 0, 0) };
    QuadBlit::Source s = QuadBlit::MakeSource(reinterpret_cast<const uint8_t*>(src), 1, 1, 2, false, true, false);
    uint8_t target[2] = {0};
    QuadBlit::BlitScaled(s, target, 1, 1, Deki::ColorFormat::RGB565, 0, 0, -1, 1);
    EXPECT_EQ(Read565(target, 1, 0, 0), 0u);
}

TEST_F(QuadBlitSpanRegressionTest, RotationPath_HalfTurnRotatesThePattern)
{
    uint16_t src[4] = { Pack565(255, 0, 0), Pack565(0, 255, 0),
                        Pack565(0, 0, 255), Pack565(255, 255, 0) };
    QuadBlit::Source s = QuadBlit::MakeSource(reinterpret_cast<const uint8_t*>(src), 2, 2, 2, false, true, false);
    const int W = 5, H = 5;
    uint8_t target[W * H * 2] = {0};
    QuadBlit::Blit(s, target, W, H, Deki::ColorFormat::RGB565, 2, 2, 1.0f, 1.0f, 3.14159265f, 0.5f, 0.5f);
    // Same corner-sampled geometry as the float version: the rotated quad
    // lands on columns/rows 2..3 with the pattern turned by 180 degrees.
    EXPECT_EQ(Read565(target, W, 2, 2), src[3]);
    EXPECT_EQ(Read565(target, W, 3, 2), src[2]);
    EXPECT_EQ(Read565(target, W, 2, 3), src[1]);
    EXPECT_EQ(Read565(target, W, 3, 3), src[0]);
    EXPECT_EQ(Read565(target, W, 1, 2), 0u);
    EXPECT_EQ(Read565(target, W, 0, 0), 0u);
}

TEST_F(QuadBlitSpanRegressionTest, RotationPath_ARGBTargetMatchesRGB565)
{
    const int S = 3;
    uint8_t src[S * S * 3];
    for (int i = 0; i < S * S; i++) Put565A8(src, i, Pack565(255, 255, 255), 128);
    QuadBlit::Source source = QuadBlit::MakeSource(src, S, S, 3, true, true, false);
    const int W = 9, H = 9;
    uint8_t t565[W * H * 2];
    uint8_t t888[W * H * 4];
    for (int i = 0; i < W * H; i++)
    {
        reinterpret_cast<uint16_t*>(t565)[i] = Pack565(0, 0, 255);
        reinterpret_cast<uint32_t*>(t888)[i] = 0xFF0000FFu;
    }
    QuadBlit::Blit(source, t565, W, H, Deki::ColorFormat::RGB565, 4, 4, 1.0f, 1.0f, 0.7f, 0.5f, 0.5f, 255, 0, 0, 255);
    QuadBlit::Blit(source, t888, W, H, Deki::ColorFormat::ARGB8888, 4, 4, 1.0f, 1.0f, 0.7f, 0.5f, 0.5f, 255, 0, 0, 255);
    const uint16_t c565 = Read565(t565, W, 4, 4);
    const uint32_t c888 = reinterpret_cast<const uint32_t*>(t888)[4 * W + 4];
    // Both blends of half-alpha red over blue: red ~128, blue ~127 (5/6/5 quantised).
    EXPECT_NEAR((c565 >> 11) << 3, (c888 >> 16) & 0xFF, 8);
    EXPECT_NEAR((c565 & 0x1F) << 3, c888 & 0xFF, 8);
}
