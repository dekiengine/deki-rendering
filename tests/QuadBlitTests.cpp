// QuadBlit: the clip stack, MakeSource, blitting.

#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include "QuadBlit.h"
#include "PixelFormat.h"
#include <deki/Engine.h>  // Deki::ColorFormat

// Tests name the package's types unqualified.
using namespace DekiRendering;

// ============================================================================
// Clip rect stack
// ============================================================================

class QuadBlitClipTest : public ::testing::Test
{
protected:
    void SetUp() override { QuadBlit::ClearClipStack(); }

    void TearDown() override { QuadBlit::ClearClipStack(); }
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
    // Child: 50,50 → 200,200, clamped to 50,50 → 100,100
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
    // Must not crash.
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

    // Disabled: the default (unset) rect.
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
// MakeSource
// ============================================================================

class QuadBlitSourceTest : public ::testing::Test
{
};

TEST_F(QuadBlitSourceTest, RGB565OpaqueSource)
{
    uint8_t pixels[4] = { 0 };
    QuadBlit::Source src = QuadBlit::MakeSource(pixels, 1, 1, QuadBlit::PixelLayout::RGB565());

    EXPECT_EQ(src.pixels, pixels);
    EXPECT_EQ(src.width, 1);
    EXPECT_EQ(src.height, 1);
    EXPECT_EQ(src.bytesPerPixel, 2);
    EXPECT_FALSE(src.hasAlpha);
    EXPECT_TRUE(src.isRGB565);
    // Borrowed by default: freeing a buffer its component still uses next
    // frame is the worse mistake, and every component in the tree keeps its
    // own.
    EXPECT_FALSE(src.ownsPixels);
    EXPECT_EQ(src.alphaOffset, 0);
}

TEST_F(QuadBlitSourceTest, RGBA8888Source)
{
    uint8_t pixels[4] = { 0 };
    QuadBlit::Source src = QuadBlit::MakeSource(pixels, 2, 3, QuadBlit::PixelLayout::RGBA8888());

    EXPECT_EQ(src.width, 2);
    EXPECT_EQ(src.height, 3);
    EXPECT_EQ(src.bytesPerPixel, 4);
    EXPECT_TRUE(src.hasAlpha);
    EXPECT_FALSE(src.isRGB565);
    EXPECT_EQ(src.alphaOffset, 3);  // RGBA: alpha at offset 3
}

TEST_F(QuadBlitSourceTest, RGB565A8Source)
{
    uint8_t pixels[3] = { 0 };
    QuadBlit::Source src = QuadBlit::MakeSource(pixels, 1, 1, QuadBlit::PixelLayout::RGB565A8());

    EXPECT_TRUE(src.hasAlpha);
    EXPECT_TRUE(src.isRGB565);
    EXPECT_EQ(src.alphaOffset, 2);  // RGB565A8: alpha at offset 2
}

TEST_F(QuadBlitSourceTest, OwnsPixelsFlagPassedThrough)
{
    uint8_t pixels[4] = { 0 };

    QuadBlit::Source owned = QuadBlit::MakeSource(pixels, 1, 1, QuadBlit::PixelLayout::RGB565(), true);
    EXPECT_TRUE(owned.ownsPixels);

    QuadBlit::Source borrowed = QuadBlit::MakeSource(pixels, 1, 1, QuadBlit::PixelLayout::RGB565(), false);
    EXPECT_FALSE(borrowed.ownsPixels);
}

// ============================================================================
// Blit and BlitScaled
// ============================================================================

class QuadBlitPixelTest : public ::testing::Test
{
protected:
    void SetUp() override { QuadBlit::ClearClipStack(); }

    void TearDown() override { QuadBlit::ClearClipStack(); }

    // Encodes an RGB565 pixel.
    static uint16_t MakeRGB565(uint8_t r, uint8_t g, uint8_t b)
    {
        return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    }

    // Reads the RGB565 pixel at (x, y).
    static uint16_t ReadRGB565(const uint8_t* buf, int32_t w, int32_t x, int32_t y)
    {
        return *reinterpret_cast<const uint16_t*>(buf + (y * w + x) * 2);
    }
};

TEST_F(QuadBlitPixelTest, BlitScaled_RGB565_1x1_Opaque)
{
    // Source: 1x1 red pixel
    uint16_t srcPixel = MakeRGB565(255, 0, 0);
    QuadBlit::Source src =
        QuadBlit::MakeSource(reinterpret_cast<const uint8_t*>(&srcPixel), 1, 1, QuadBlit::PixelLayout::RGB565(), false);

    // Target: 4x4 buffer, all black
    const int w = 4, h = 4;
    uint8_t target[w * h * 2] = { 0 };

    // Blit at (1, 1), no scaling
    QuadBlit::BlitScaled(src, target, w, h, Deki::ColorFormat::RGB565, 1, 1, 1, 1);

    // (1,1) is red.
    EXPECT_EQ(ReadRGB565(target, w, 1, 1), srcPixel);

    // (0,0) stays black.
    EXPECT_EQ(ReadRGB565(target, w, 0, 0), 0);

    // (2,2) stays black.
    EXPECT_EQ(ReadRGB565(target, w, 2, 2), 0);
}

TEST_F(QuadBlitPixelTest, BlitScaled_RGB565_2x2_AtOrigin)
{
    // Source: 2x2 with distinct pixels
    uint16_t srcPixels[4] = {
        MakeRGB565(255, 0, 0),
        MakeRGB565(0, 255, 0),
        MakeRGB565(0, 0, 255),
        MakeRGB565(255, 255, 0),
    };
    QuadBlit::Source src =
        QuadBlit::MakeSource(reinterpret_cast<const uint8_t*>(srcPixels), 2, 2, QuadBlit::PixelLayout::RGB565(), false);

    const int w = 4, h = 4;
    uint8_t target[w * h * 2] = { 0 };

    QuadBlit::BlitScaled(src, target, w, h, Deki::ColorFormat::RGB565, 0, 0, 2, 2);

    EXPECT_EQ(ReadRGB565(target, w, 0, 0), srcPixels[0]);
    EXPECT_EQ(ReadRGB565(target, w, 1, 0), srcPixels[1]);
    EXPECT_EQ(ReadRGB565(target, w, 0, 1), srcPixels[2]);
    EXPECT_EQ(ReadRGB565(target, w, 1, 1), srcPixels[3]);

    // (2,0) stays black.
    EXPECT_EQ(ReadRGB565(target, w, 2, 0), 0);
}

TEST_F(QuadBlitPixelTest, BlitScaled_FullyOutOfBounds_NoWrite)
{
    uint16_t srcPixel = MakeRGB565(255, 0, 0);
    QuadBlit::Source src =
        QuadBlit::MakeSource(reinterpret_cast<const uint8_t*>(&srcPixel), 1, 1, QuadBlit::PixelLayout::RGB565(), false);

    const int w = 4, h = 4;
    uint8_t target[w * h * 2] = { 0 };

    // Blit at (10, 10), entirely outside the 4x4 buffer.
    QuadBlit::BlitScaled(src, target, w, h, Deki::ColorFormat::RGB565, 10, 10, 1, 1);

    // Every pixel stays black.
    for (int i = 0; i < w * h; i++)
    {
        EXPECT_EQ(reinterpret_cast<uint16_t*>(target)[i], 0) << "pixel " << i << " should be 0";
    }
}

TEST_F(QuadBlitPixelTest, Blit_WithTintAlphaZero_NoWrite)
{
    uint16_t srcPixel = MakeRGB565(255, 0, 0);
    QuadBlit::Source src =
        QuadBlit::MakeSource(reinterpret_cast<const uint8_t*>(&srcPixel), 1, 1, QuadBlit::PixelLayout::RGB565(), false);

    const int w = 4, h = 4;
    uint8_t target[w * h * 2] = { 0 };

    // tintA 0: invisible.
    QuadBlit::Blit(src, target, w, h, Deki::ColorFormat::RGB565, 1, 1, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 255, 255, 255, 0);

    // Every pixel stays black.
    for (int i = 0; i < w * h; i++)
    {
        EXPECT_EQ(reinterpret_cast<uint16_t*>(target)[i], 0);
    }
}

TEST_F(QuadBlitPixelTest, BlitScaled_ClipRectRestrictsOutput)
{
    // Source: 2x2 all red
    uint16_t red = MakeRGB565(255, 0, 0);
    uint16_t srcPixels[4] = { red, red, red, red };
    QuadBlit::Source src =
        QuadBlit::MakeSource(reinterpret_cast<const uint8_t*>(srcPixels), 2, 2, QuadBlit::PixelLayout::RGB565(), false);

    const int w = 4, h = 4;
    uint8_t target[w * h * 2] = { 0 };

    // Clip to (0,0)→(1,1): only the top-left pixel is written.
    QuadBlit::PushClipRect(0, 0, 1, 1);

    QuadBlit::BlitScaled(src, target, w, h, Deki::ColorFormat::RGB565, 0, 0, 2, 2);

    // (0,0), inside the clip, is red.
    EXPECT_EQ(ReadRGB565(target, w, 0, 0), red);

    // (1,0), (0,1) and (1,1), outside the clip, stay black.
    EXPECT_EQ(ReadRGB565(target, w, 1, 0), 0);
    EXPECT_EQ(ReadRGB565(target, w, 0, 1), 0);
    EXPECT_EQ(ReadRGB565(target, w, 1, 1), 0);
}

TEST_F(QuadBlitPixelTest, BlitScaled_1x1_To_2x2_Upscale)
{
    // Source: 1x1 green pixel
    uint16_t green = MakeRGB565(0, 255, 0);
    QuadBlit::Source src =
        QuadBlit::MakeSource(reinterpret_cast<const uint8_t*>(&green), 1, 1, QuadBlit::PixelLayout::RGB565(), false);

    const int w = 4, h = 4;
    uint8_t target[w * h * 2] = { 0 };

    // Scale 1x1 to 2x2 at (0,0)
    QuadBlit::BlitScaled(src, target, w, h, Deki::ColorFormat::RGB565, 0, 0, 2, 2);

    // All 4 pixels of the 2x2 destination are green.
    EXPECT_EQ(ReadRGB565(target, w, 0, 0), green);
    EXPECT_EQ(ReadRGB565(target, w, 1, 0), green);
    EXPECT_EQ(ReadRGB565(target, w, 0, 1), green);
    EXPECT_EQ(ReadRGB565(target, w, 1, 1), green);

    // (2,0) stays black.
    EXPECT_EQ(ReadRGB565(target, w, 2, 0), 0);
}

// ============================================================================
// Kernel dispatch
// ============================================================================
// When a row kernel is registered and the alignment preconditions hold, the
// dispatcher calls it; otherwise the scalar loop runs.

namespace
{

inline uint16_t MakeRGB565Free(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// Counts kernel invocations and writes a deterministic marker into dst so
// tests can distinguish kernel output from the scalar path's output.
struct KernelProbe
{
    static int s_CallCount;
    static uint16_t s_Marker;
    static int32_t s_LastPixelCount;

    static void Reset()
    {
        s_CallCount = 0;
        s_LastPixelCount = 0;
    }

    static void CopyRowMarker(const uint8_t* /*src*/, uint8_t* dst, int32_t pixelCount, uint8_t, uint8_t, uint8_t,
                              uint8_t)
    {
        ++s_CallCount;
        s_LastPixelCount = pixelCount;
        uint16_t* d = reinterpret_cast<uint16_t*>(dst);
        for (int32_t i = 0; i < pixelCount; ++i)
        {
            d[i] = s_Marker;
        }
    }

    static void BlendRowMarker(const uint8_t* /*src*/, uint8_t* dst, int32_t pixelCount, uint8_t, uint8_t, uint8_t,
                               uint8_t)
    {
        ++s_CallCount;
        s_LastPixelCount = pixelCount;
        uint16_t* d = reinterpret_cast<uint16_t*>(dst);
        for (int32_t i = 0; i < pixelCount; ++i)
        {
            d[i] = s_Marker;
        }
    }
};
int KernelProbe::s_CallCount = 0;
uint16_t KernelProbe::s_Marker = 0;
int32_t KernelProbe::s_LastPixelCount = 0;

// A 16-byte-aligned heap buffer, freed by its aligned-free helper.
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

}  // namespace

class QuadBlitKernelDispatchTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        QuadBlit::ClearClipStack();
        // Clear any kernels registered earlier.
        QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565CopyRow, nullptr);
        QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565A8BlendRow, nullptr);
        KernelProbe::Reset();
    }

    void TearDown() override
    {
        QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565CopyRow, nullptr);
        QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565A8BlendRow, nullptr);
        QuadBlit::ClearClipStack();
    }
};

TEST_F(QuadBlitKernelDispatchTest, RGB565CopyRow_UsesKernel_WhenAligned)
{
    // 16-pixel-wide source and destination, 16-byte aligned.
    const int w = 16, h = 1;
    AlignedBuf srcBuf(w * h * 2);
    AlignedBuf dstBuf(w * h * 2);
    // Fill the source with a recognisable value, so the kernel's different
    // marker proves the kernel ran.
    uint16_t scalarValue = MakeRGB565Free(64, 128, 192);
    auto* srcPx = reinterpret_cast<uint16_t*>(srcBuf.aligned);
    for (int i = 0; i < w * h; ++i)
    {
        srcPx[i] = scalarValue;
    }

    KernelProbe::s_Marker = MakeRGB565Free(255, 0, 255);  // distinct
    QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565CopyRow, &KernelProbe::CopyRowMarker);

    QuadBlit::Source src = QuadBlit::MakeSource(srcBuf.aligned, w, h, QuadBlit::PixelLayout::RGB565(), false);
    QuadBlit::BlitScaled(src, dstBuf.aligned, w, h, Deki::ColorFormat::RGB565, 0, 0, w, h);

    EXPECT_EQ(KernelProbe::s_CallCount, h);
    EXPECT_EQ(KernelProbe::s_LastPixelCount, w);
    auto* dstPx = reinterpret_cast<uint16_t*>(dstBuf.aligned);
    for (int i = 0; i < w; ++i)
    {
        EXPECT_EQ(dstPx[i], KernelProbe::s_Marker) << "pixel " << i;
    }
}

TEST_F(QuadBlitKernelDispatchTest, RGB565CopyRow_SkipsKernel_WhenSourceMisaligned)
{
    const int w = 16, h = 1;
    AlignedBuf srcBuf(w * h * 2 + 16);
    AlignedBuf dstBuf(w * h * 2);
    // Misalign the source by 2 bytes (still 2-byte aligned for uint16_t reads,
    // but no longer 16-byte aligned).
    uint8_t* srcMisaligned = srcBuf.aligned + 2;
    uint16_t scalarValue = MakeRGB565Free(64, 128, 192);
    auto* srcPx = reinterpret_cast<uint16_t*>(srcMisaligned);
    for (int i = 0; i < w * h; ++i)
    {
        srcPx[i] = scalarValue;
    }

    KernelProbe::s_Marker = MakeRGB565Free(255, 0, 255);
    QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565CopyRow, &KernelProbe::CopyRowMarker);

    QuadBlit::Source src = QuadBlit::MakeSource(srcMisaligned, w, h, QuadBlit::PixelLayout::RGB565(), false);
    QuadBlit::BlitScaled(src, dstBuf.aligned, w, h, Deki::ColorFormat::RGB565, 0, 0, w, h);

    // The kernel must not run: the alignment precondition failed.
    EXPECT_EQ(KernelProbe::s_CallCount, 0);
    // The output is the scalar copy, the same as the source.
    auto* dstPx = reinterpret_cast<uint16_t*>(dstBuf.aligned);
    for (int i = 0; i < w; ++i)
    {
        EXPECT_EQ(dstPx[i], scalarValue) << "pixel " << i;
    }
}

TEST_F(QuadBlitKernelDispatchTest, RGB565CopyRow_NoKernel_RunsScalar)
{
    // No kernel registered: the scalar path copies the bytes verbatim.
    const int w = 8, h = 1;
    AlignedBuf srcBuf(w * h * 2);
    AlignedBuf dstBuf(w * h * 2);
    auto* srcPx = reinterpret_cast<uint16_t*>(srcBuf.aligned);
    for (int i = 0; i < w * h; ++i)
    {
        srcPx[i] = MakeRGB565Free(static_cast<uint8_t>(i * 16), 0, 0);
    }

    QuadBlit::Source src = QuadBlit::MakeSource(srcBuf.aligned, w, h, QuadBlit::PixelLayout::RGB565(), false);
    QuadBlit::BlitScaled(src, dstBuf.aligned, w, h, Deki::ColorFormat::RGB565, 0, 0, w, h);

    EXPECT_EQ(KernelProbe::s_CallCount, 0);
    auto* dstPx = reinterpret_cast<uint16_t*>(dstBuf.aligned);
    for (int i = 0; i < w; ++i)
    {
        EXPECT_EQ(dstPx[i], srcPx[i]);
    }
}

TEST_F(QuadBlitKernelDispatchTest, RGB565A8BlendRow_UsesKernel_WhenAligned_AndUntinted)
{
    // Source is RGB565A8 (3 bytes/pixel). At 1:1 with hasAlpha true and no
    // tint, chroma key or row spans, the blend kernel slot is used.
    const int w = 16, h = 1;
    AlignedBuf srcBuf(w * h * 3 + 16);
    AlignedBuf dstBuf(w * h * 2);
    // Non-zero alpha, so the per-pixel else branch is reached.
    for (int i = 0; i < w * h; ++i)
    {
        srcBuf.aligned[i * 3 + 0] = 0x12;  // RGB565 lo
        srcBuf.aligned[i * 3 + 1] = 0x34;  // RGB565 hi
        srcBuf.aligned[i * 3 + 2] = 200;   // alpha (any value > 0 keeps us off the early-skip)
    }

    KernelProbe::s_Marker = MakeRGB565Free(0, 255, 255);
    QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565A8BlendRow, &KernelProbe::BlendRowMarker);

    QuadBlit::Source src = QuadBlit::MakeSource(srcBuf.aligned, w, h, QuadBlit::PixelLayout::RGB565A8(), false);
    QuadBlit::BlitScaled(src, dstBuf.aligned, w, h, Deki::ColorFormat::RGB565, 0, 0, w, h);

    EXPECT_EQ(KernelProbe::s_CallCount, h);
    EXPECT_EQ(KernelProbe::s_LastPixelCount, w);
    auto* dstPx = reinterpret_cast<uint16_t*>(dstBuf.aligned);
    for (int i = 0; i < w; ++i)
    {
        EXPECT_EQ(dstPx[i], KernelProbe::s_Marker) << "pixel " << i;
    }
}

TEST_F(QuadBlitKernelDispatchTest, RGB565A8BlendRow_UsesKernel_WhenTinted)
{
    // The kernel takes the tint (KernelOp::RGB565A8BlendRow), so a tinted
    // blend goes to it too, whole rows at a time.
    const int w = 16, h = 1;
    AlignedBuf srcBuf(w * h * 3 + 16);
    AlignedBuf dstBuf(w * h * 2);
    for (int i = 0; i < w * h; ++i)
    {
        srcBuf.aligned[i * 3 + 0] = 0x00;
        srcBuf.aligned[i * 3 + 1] = 0x00;
        srcBuf.aligned[i * 3 + 2] = 255;
    }

    KernelProbe::s_Marker = MakeRGB565Free(0, 255, 255);
    QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565A8BlendRow, &KernelProbe::BlendRowMarker);

    QuadBlit::Source src = QuadBlit::MakeSource(srcBuf.aligned, w, h, QuadBlit::PixelLayout::RGB565A8(), false);
    QuadBlit::BlitScaled(src, dstBuf.aligned, w, h, Deki::ColorFormat::RGB565, 0, 0, w, h, 128, 128, 128, 255);

    EXPECT_EQ(KernelProbe::s_CallCount, 1);
    EXPECT_EQ(KernelProbe::s_LastPixelCount, w);
}

// ============================================================================
// RGB565A8 targets
// ============================================================================
// The paths into an RGB565A8 target and their kernel slots.

class QuadBlitRGB565A8TargetTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        QuadBlit::ClearClipStack();
        QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565ToRGB565A8Row, nullptr);
        QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565A8CopyRow, nullptr);
        QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565A8BlendRowDestRGB565A8, nullptr);
    }
    void TearDown() override
    {
        QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565ToRGB565A8Row, nullptr);
        QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565A8CopyRow, nullptr);
        QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565A8BlendRowDestRGB565A8, nullptr);
        QuadBlit::ClearClipStack();
    }
};

TEST_F(QuadBlitRGB565A8TargetTest, RGB565A8_to_RGB565A8_Opaque_Copies_RGB_AndSetsAlpha)
{
    // 2x1 source, RGB565A8 with alpha = 255 per pixel.
    const int w = 2, h = 1;
    uint8_t src[w * 3];
    uint16_t pix0 = MakeRGB565Free(255, 0, 0);  // red
    uint16_t pix1 = MakeRGB565Free(0, 255, 0);  // green
    src[0] = (uint8_t)(pix0 & 0xFF);
    src[1] = (uint8_t)((pix0 >> 8) & 0xFF);
    src[2] = 255;
    src[3] = (uint8_t)(pix1 & 0xFF);
    src[4] = (uint8_t)((pix1 >> 8) & 0xFF);
    src[5] = 255;

    uint8_t target[w * h * 3] = { 0 };

    QuadBlit::Source s = QuadBlit::MakeSource(src, w, h, QuadBlit::PixelLayout::RGB565A8(), false);
    QuadBlit::BlitScaled(s, target, w, h, Deki::ColorFormat::RGB565A8, 0, 0, w, h);

    EXPECT_EQ(target[0], src[0]);
    EXPECT_EQ(target[1], src[1]);
    EXPECT_EQ(target[2], 255);
    EXPECT_EQ(target[3], src[3]);
    EXPECT_EQ(target[4], src[4]);
    EXPECT_EQ(target[5], 255);
}

TEST_F(QuadBlitRGB565A8TargetTest, RGB565A8_to_RGB565A8_AlphaZero_LeavesTargetUnchanged)
{
    // Source alpha 0: that pixel is left alone.
    const int w = 1, h = 1;
    uint16_t pix = MakeRGB565Free(255, 0, 0);
    uint8_t src[3] = { (uint8_t)(pix & 0xFF), (uint8_t)((pix >> 8) & 0xFF), 0 };

    // Fill the target with a recognisable pattern first.
    uint8_t target[w * h * 3] = { 0xAB, 0xCD, 0xEF };

    QuadBlit::Source s = QuadBlit::MakeSource(src, w, h, QuadBlit::PixelLayout::RGB565A8(), false);
    QuadBlit::BlitScaled(s, target, w, h, Deki::ColorFormat::RGB565A8, 0, 0, w, h);

    EXPECT_EQ(target[0], 0xAB);
    EXPECT_EQ(target[1], 0xCD);
    EXPECT_EQ(target[2], 0xEF);
}

TEST_F(QuadBlitRGB565A8TargetTest, RGB565A8_to_RGB565A8_PartialAlpha_OntoCleared_PreservesSrcAlpha)
{
    // src.a = 128, dst initially zeroed (alpha=0). After src-over alpha union,
    // out.a = src.a + dst.a*(255-src.a)/255 = 128 + 0 = 128.
    const int w = 1, h = 1;
    uint16_t pix = MakeRGB565Free(255, 255, 255);  // white
    uint8_t src[3] = { (uint8_t)(pix & 0xFF), (uint8_t)((pix >> 8) & 0xFF), 128 };
    uint8_t target[w * h * 3] = { 0 };

    QuadBlit::Source s = QuadBlit::MakeSource(src, w, h, QuadBlit::PixelLayout::RGB565A8(), false);
    QuadBlit::BlitScaled(s, target, w, h, Deki::ColorFormat::RGB565A8, 0, 0, w, h);

    EXPECT_EQ(target[2], 128) << "out.a should equal src.a when dst.a was 0";
}

TEST_F(QuadBlitRGB565A8TargetTest, RGB565_to_RGB565A8_SetsAlphaTo255)
{
    // A plain RGB565 source has no alpha, so the target alpha byte is 0xFF.
    const int w = 2, h = 1;
    uint16_t srcPx[w];
    srcPx[0] = MakeRGB565Free(255, 0, 0);
    srcPx[1] = MakeRGB565Free(0, 0, 255);

    uint8_t target[w * h * 3] = { 0 };

    QuadBlit::Source s =
        QuadBlit::MakeSource(reinterpret_cast<const uint8_t*>(srcPx), w, h, QuadBlit::PixelLayout::RGB565(), false);
    QuadBlit::BlitScaled(s, target, w, h, Deki::ColorFormat::RGB565A8, 0, 0, w, h);

    EXPECT_EQ(target[0], (uint8_t)(srcPx[0] & 0xFF));
    EXPECT_EQ(target[1], (uint8_t)((srcPx[0] >> 8) & 0xFF));
    EXPECT_EQ(target[2], 255);
    EXPECT_EQ(target[3], (uint8_t)(srcPx[1] & 0xFF));
    EXPECT_EQ(target[4], (uint8_t)((srcPx[1] >> 8) & 0xFF));
    EXPECT_EQ(target[5], 255);
}

// Kernel dispatch for the RGB565A8-target slots.

namespace
{
struct RGB565A8KernelProbe
{
    static int s_CallCount;
    static int32_t s_LastPixelCount;
    static uint8_t s_Marker;
    static void Reset()
    {
        s_CallCount = 0;
        s_LastPixelCount = 0;
    }
    static void Run(const uint8_t* /*src*/, uint8_t* dst, int32_t pixelCount, uint8_t, uint8_t, uint8_t, uint8_t)
    {
        ++s_CallCount;
        s_LastPixelCount = pixelCount;
        for (int32_t i = 0; i < pixelCount; ++i)
        {
            dst[i * 3] = s_Marker;
            dst[i * 3 + 1] = s_Marker;
            dst[i * 3 + 2] = s_Marker;
        }
    }
};
int RGB565A8KernelProbe::s_CallCount = 0;
int32_t RGB565A8KernelProbe::s_LastPixelCount = 0;
uint8_t RGB565A8KernelProbe::s_Marker = 0;
}  // namespace

TEST_F(QuadBlitRGB565A8TargetTest, RGB565A8CopyRow_KernelInvoked_WhenAlignedAndOpaqueSource)
{
    // Source hasAlpha false: the opaque-copy fast path.
    const int w = 16, h = 1;
    AlignedBuf srcBuf(w * h * 3 + 16);
    AlignedBuf dstBuf(w * h * 3);
    for (int i = 0; i < w * h; ++i)
    {
        srcBuf.aligned[i * 3 + 0] = 0x11;
        srcBuf.aligned[i * 3 + 1] = 0x22;
        srcBuf.aligned[i * 3 + 2] = 0xFF;
    }

    RGB565A8KernelProbe::Reset();
    RGB565A8KernelProbe::s_Marker = 0x77;
    QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565A8CopyRow, &RGB565A8KernelProbe::Run);

    QuadBlit::Source s = QuadBlit::MakeSource(srcBuf.aligned, w, h, QuadBlit::PixelLayout::RGB565A8NoAlpha(), false);
    QuadBlit::BlitScaled(s, dstBuf.aligned, w, h, Deki::ColorFormat::RGB565A8, 0, 0, w, h);

    EXPECT_EQ(RGB565A8KernelProbe::s_CallCount, h);
    EXPECT_EQ(RGB565A8KernelProbe::s_LastPixelCount, w);
    EXPECT_EQ(dstBuf.aligned[0], 0x77);
}

TEST_F(QuadBlitRGB565A8TargetTest, RGB565A8BlendRow_KernelInvoked_WhenAlignedAndAlphaSource)
{
    // Source hasAlpha true: the alpha blend path, whose kernel slot is used with no tint or key.
    const int w = 16, h = 1;
    AlignedBuf srcBuf(w * h * 3 + 16);
    AlignedBuf dstBuf(w * h * 3);
    for (int i = 0; i < w * h; ++i)
    {
        srcBuf.aligned[i * 3 + 0] = 0x33;
        srcBuf.aligned[i * 3 + 1] = 0x44;
        srcBuf.aligned[i * 3 + 2] = 200;  // partial alpha keeps us off the a==0/255 short-circuits
    }

    RGB565A8KernelProbe::Reset();
    RGB565A8KernelProbe::s_Marker = 0x99;
    QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565A8BlendRowDestRGB565A8, &RGB565A8KernelProbe::Run);

    QuadBlit::Source s = QuadBlit::MakeSource(srcBuf.aligned, w, h, QuadBlit::PixelLayout::RGB565A8(), false);
    QuadBlit::BlitScaled(s, dstBuf.aligned, w, h, Deki::ColorFormat::RGB565A8, 0, 0, w, h);

    EXPECT_EQ(RGB565A8KernelProbe::s_CallCount, h);
    EXPECT_EQ(RGB565A8KernelProbe::s_LastPixelCount, w);
}

TEST_F(QuadBlitRGB565A8TargetTest, RGB565ToRGB565A8_KernelInvoked_WhenAligned)
{
    const int w = 16, h = 1;
    AlignedBuf srcBuf(w * h * 2);
    AlignedBuf dstBuf(w * h * 3);
    auto* srcPx = reinterpret_cast<uint16_t*>(srcBuf.aligned);
    for (int i = 0; i < w * h; ++i)
    {
        srcPx[i] = MakeRGB565Free((uint8_t)(i * 16), 0, 0);
    }

    RGB565A8KernelProbe::Reset();
    RGB565A8KernelProbe::s_Marker = 0x55;
    QuadBlit::RegisterKernel(QuadBlit::KernelOp::RGB565ToRGB565A8Row, &RGB565A8KernelProbe::Run);

    QuadBlit::Source s = QuadBlit::MakeSource(srcBuf.aligned, w, h, QuadBlit::PixelLayout::RGB565(), false);
    QuadBlit::BlitScaled(s, dstBuf.aligned, w, h, Deki::ColorFormat::RGB565A8, 0, 0, w, h);

    EXPECT_EQ(RGB565A8KernelProbe::s_CallCount, h);
    EXPECT_EQ(RGB565A8KernelProbe::s_LastPixelCount, w);
}

// ============================================================================
// Ordered-dither alpha
// ============================================================================
// The useOrderedDither path: opaque pixels still draw, fully transparent ones
// are still skipped, and partial-alpha pixels follow the Bayer threshold
// pattern (no destination read, no blend maths).

class QuadBlitDitherTest : public ::testing::Test
{
protected:
    void SetUp() override { QuadBlit::ClearClipStack(); }
    void TearDown() override { QuadBlit::ClearClipStack(); }
};

TEST_F(QuadBlitDitherTest, OpaqueSrc_WritesAllPixels_RGB565A8_to_RGB565)
{
    // 4x1 RGB565A8 source with alpha 255: every pixel draws whatever the
    // Bayer threshold. Takes the RGB565A8→RGB565 dither path.
    const int w = 4, h = 1;
    uint8_t src[w * h * 3];
    uint16_t color = MakeRGB565Free(255, 128, 0);
    for (int i = 0; i < w * h; ++i)
    {
        src[i * 3] = (uint8_t)(color & 0xFF);
        src[i * 3 + 1] = (uint8_t)((color >> 8) & 0xFF);
        src[i * 3 + 2] = 255;  // fully opaque
    }
    uint8_t target[w * h * 2] = { 0 };

    QuadBlit::Source s = QuadBlit::MakeSource(src, w, h, QuadBlit::PixelLayout::RGB565A8(), false);
    QuadBlit::BlitScaled(s, target, w, h, Deki::ColorFormat::RGB565, 0, 0, w, h, 255, 255, 255, 255,
                         /*useOrderedDither=*/true);

    auto* dst = reinterpret_cast<uint16_t*>(target);
    for (int i = 0; i < w * h; ++i)
    {
        EXPECT_EQ(dst[i], color) << "pixel " << i << " should be opaque-written";
    }
}

TEST_F(QuadBlitDitherTest, ZeroAlphaSrc_LeavesTargetUnchanged)
{
    // src.a = 0: every pixel is skipped, dither or not.
    const int w = 2, h = 1;
    uint8_t src[w * h * 3];
    uint16_t color = MakeRGB565Free(255, 0, 0);
    for (int i = 0; i < w * h; ++i)
    {
        src[i * 3] = (uint8_t)(color & 0xFF);
        src[i * 3 + 1] = (uint8_t)((color >> 8) & 0xFF);
        src[i * 3 + 2] = 0;  // fully transparent
    }
    uint16_t pre = MakeRGB565Free(0, 0, 0);
    uint16_t target[w * h] = { pre, pre };

    QuadBlit::Source s = QuadBlit::MakeSource(src, w, h, QuadBlit::PixelLayout::RGB565A8(), false);
    QuadBlit::BlitScaled(s, reinterpret_cast<uint8_t*>(target), w, h, Deki::ColorFormat::RGB565, 0, 0, w, h, 255, 255,
                         255, 255, /*useOrderedDither=*/true);

    EXPECT_EQ(target[0], pre);
    EXPECT_EQ(target[1], pre);
}

TEST_F(QuadBlitDitherTest, PartialAlpha_FollowsBayerThreshold)
{
    // 8x8 source with src.a = 128. The Bayer matrix spreads its values evenly
    // over 0..255, so exactly half the pixels pass the threshold. Each output
    // pixel is either the source colour or untouched, never a blend.
    const int w = 8, h = 8;
    uint8_t src[w * h * 3];
    uint16_t color = MakeRGB565Free(0, 255, 0);  // green
    for (int i = 0; i < w * h; ++i)
    {
        src[i * 3] = (uint8_t)(color & 0xFF);
        src[i * 3 + 1] = (uint8_t)((color >> 8) & 0xFF);
        src[i * 3 + 2] = 128;
    }
    uint16_t bg = MakeRGB565Free(0, 0, 255);  // blue background, must not be blended
    uint16_t target[w * h];
    for (int i = 0; i < w * h; ++i)
    {
        target[i] = bg;
    }

    QuadBlit::Source s = QuadBlit::MakeSource(src, w, h, QuadBlit::PixelLayout::RGB565A8(), false);
    QuadBlit::BlitScaled(s, reinterpret_cast<uint8_t*>(target), w, h, Deki::ColorFormat::RGB565, 0, 0, w, h, 255, 255,
                         255, 255, /*useOrderedDither=*/true);

    int wrote = 0, kept = 0;
    for (int i = 0; i < w * h; ++i)
    {
        if (target[i] == color)
        {
            ++wrote;
        }
        else if (target[i] == bg)
        {
            ++kept;
        }
        else
        {
            FAIL() << "pixel " << i << " is " << target[i] << " — neither pure src nor pure bg, blend leaked";
        }
    }
    // The 8x8 Bayer matrix has 64 distinct values in 0..255. Pixels with a
    // threshold below 128 pass: exactly half (32) by construction.
    EXPECT_EQ(wrote, 32);
    EXPECT_EQ(kept, 32);
}

TEST_F(QuadBlitDitherTest, GenericPath_RGB565A8_to_RGB565A8)
{
    // The generic dither path (target not RGB565), with the same thresholds;
    // checks only that alpha 0 skips and alpha 255 writes.
    const int w = 2, h = 1;
    uint8_t src[w * h * 3] = {
        0x00, 0xF8, 0xFF,  // red, opaque
        0x00, 0xF8, 0x00,  // red, transparent
    };
    uint8_t target[w * h * 3] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };

    QuadBlit::Source s = QuadBlit::MakeSource(src, w, h, QuadBlit::PixelLayout::RGB565A8(), false);
    QuadBlit::BlitScaled(s, target, w, h, Deki::ColorFormat::RGB565A8, 0, 0, w, h, 255, 255, 255, 255,
                         /*useOrderedDither=*/true);

    EXPECT_EQ(target[0], 0x00);  // opaque pixel: src bytes written
    EXPECT_EQ(target[1], 0xF8);
    EXPECT_EQ(target[2], 0xFF);  // alpha = 0xFF on opaque write
    EXPECT_EQ(target[3], 0x44);  // transparent pixel: untouched
    EXPECT_EQ(target[4], 0x55);
    EXPECT_EQ(target[5], 0x66);
}

// ============================================================================
// Regressions: row-span edge cases, deep clip stacks, rotation tint order
// ============================================================================

namespace
{
uint16_t Pack565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
// The pixel as stored: compare it with stored pixels, or FromMemory565 it to
// compare it with a value.
uint16_t Read565(const uint8_t* buf, int w, int x, int y)
{
    return reinterpret_cast<const uint16_t*>(buf)[y * w + x];
}
// RGB565A8: 2 bytes of colour (the value `rgb`, as stored) then 1 byte of
// alpha, per pixel.
void Put565A8(uint8_t* dst, int i, uint16_t rgb, uint8_t a)
{
    DekiPixel::Store565(dst + i * 3, rgb);
    dst[i * 3 + 2] = a;
}
}  // namespace

class QuadBlitSpanRegressionTest : public ::testing::Test
{
protected:
    void SetUp() override { QuadBlit::ClearClipStack(); }
    void TearDown() override { QuadBlit::ClearClipStack(); }
};

// A row with no fully opaque pixel carries an empty span (start == end == w).
// The spanned blit must equal the unspanned one; a span of (w, 0) would make
// the left and right regions both cover the row and blend each soft pixel
// twice.
TEST_F(QuadBlitSpanRegressionTest, EmptyOpaqueSpan_BlendsEachPixelOnce)
{
    const int w = 4;
    uint8_t src[w * 3];
    for (int i = 0; i < w; i++)
    {
        Put565A8(src, i, Pack565(255, 255, 255), 128);
    }
    const int16_t emptySpan[2] = { w, w };

    uint8_t withSpans[w * 2] = { 0 }, withoutSpans[w * 2] = { 0 };
    QuadBlit::Source spanned = QuadBlit::MakeSource(src, w, 1, QuadBlit::PixelLayout::RGB565A8(), false, emptySpan);
    QuadBlit::Source plain = QuadBlit::MakeSource(src, w, 1, QuadBlit::PixelLayout::RGB565A8(), false, nullptr);
    QuadBlit::BlitScaled(spanned, withSpans, w, 1, Deki::ColorFormat::RGB565, 0, 0, w, 1);
    QuadBlit::BlitScaled(plain, withoutSpans, w, 1, Deki::ColorFormat::RGB565, 0, 0, w, 1);

    for (int x = 0; x < w; x++)
    {
        EXPECT_EQ(Read565(withSpans, w, x, 0), Read565(withoutSpans, w, x, 0)) << "x=" << x;
    }
    // Half-alpha white over black is mid grey; blending twice would give white.
    EXPECT_LT(DekiPixel::FromMemory565(Read565(withSpans, w, 0, 0)), Pack565(200, 200, 200));
}

// The right alpha region must start at the clip start when the clip rect
// starts after opaqueEnd, not write pixels the clip excludes.
TEST_F(QuadBlitSpanRegressionTest, RightAlphaRegion_RespectsClipLeftEdge)
{
    const int w = 4;
    uint8_t src[w * 3];
    for (int i = 0; i < w; i++)
    {
        Put565A8(src, i, Pack565(255, 255, 255), 128);
    }
    const int16_t emptySpan[2] = { w, w };
    QuadBlit::Source spanned = QuadBlit::MakeSource(src, w, 1, QuadBlit::PixelLayout::RGB565A8(), false, emptySpan);

    uint8_t target[w * 2] = { 0 };
    QuadBlit::PushClipRect(2, 0, w, 1);
    QuadBlit::BlitScaled(spanned, target, w, 1, Deki::ColorFormat::RGB565, 0, 0, w, 1);
    QuadBlit::PopClipRect();

    EXPECT_EQ(Read565(target, w, 0, 0), 0u) << "left of the clip rect must stay untouched";
    EXPECT_EQ(Read565(target, w, 1, 0), 0u);
    EXPECT_NE(Read565(target, w, 2, 0), 0u);
    EXPECT_NE(Read565(target, w, 3, 0), 0u);
}

// The clip stack has no fixed capacity: 40 nested levels all intersect and pop
// back out in order.
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
    QuadBlit::PopClipRect();  // an extra pop on an empty stack is harmless
    EXPECT_EQ(QuadBlit::GetClipStackDepth(), 0);
}

// The rotation path must tint the source before compositing, like every
// BlitScaled path. A full turn takes the rotated code path with the same
// geometry as no rotation, so the two must agree on the centre pixel.
TEST_F(QuadBlitSpanRegressionTest, RotationPath_TintsSourceBeforeBlend)
{
    const int s = 3;
    uint8_t src[s * s * 3];
    for (int i = 0; i < s * s; i++)
    {
        Put565A8(src, i, Pack565(255, 255, 255), 128);
    }
    QuadBlit::Source source = QuadBlit::MakeSource(src, s, s, QuadBlit::PixelLayout::RGB565A8(), false);

    const int w = 9, h = 9;
    uint8_t straight[w * h * 2], turned[w * h * 2];
    // A bright, distinct background so a wrong blend order shows.
    for (int i = 0; i < w * h; i++)
    {
        reinterpret_cast<uint16_t*>(straight)[i] = Pack565(0, 0, 255);
        reinterpret_cast<uint16_t*>(turned)[i] = Pack565(0, 0, 255);
    }
    const float fullTurn = 6.28318530718f;
    QuadBlit::Blit(source, straight, w, h, Deki::ColorFormat::RGB565, 4, 4, 1.0f, 1.0f, 0.0f, 0.5f, 0.5f, 255, 0, 0,
                   255);
    QuadBlit::Blit(source, turned, w, h, Deki::ColorFormat::RGB565, 4, 4, 1.0f, 1.0f, fullTurn, 0.5f, 0.5f, 255, 0, 0,
                   255);

    EXPECT_EQ(Read565(turned, w, 4, 4), Read565(straight, w, 4, 4));
}

// ---------------------------------------------------------------------------
// Flips. Tiled-authored tiles carry H/V/D flags, which map to the Source flip
// fields; negative destination sizes are not flips, and BlitScaled rejects
// them.
// ---------------------------------------------------------------------------
TEST_F(QuadBlitSpanRegressionTest, FlipH_MirrorsColumns)
{
    uint16_t src[2] = { Pack565(255, 0, 0), Pack565(0, 255, 0) };
    QuadBlit::Source s =
        QuadBlit::MakeSource(reinterpret_cast<const uint8_t*>(src), 2, 1, QuadBlit::PixelLayout::RGB565(), false);
    s.flipH = true;
    uint8_t target[2 * 2] = { 0 };
    QuadBlit::BlitScaled(s, target, 2, 1, Deki::ColorFormat::RGB565, 0, 0, 2, 1);
    EXPECT_EQ(Read565(target, 2, 0, 0), src[1]);
    EXPECT_EQ(Read565(target, 2, 1, 0), src[0]);
}

TEST_F(QuadBlitSpanRegressionTest, FlipV_MirrorsRows)
{
    uint16_t src[2] = { Pack565(255, 0, 0), Pack565(0, 255, 0) };
    QuadBlit::Source s =
        QuadBlit::MakeSource(reinterpret_cast<const uint8_t*>(src), 1, 2, QuadBlit::PixelLayout::RGB565(), false);
    s.flipV = true;
    uint8_t target[1 * 2 * 2] = { 0 };
    QuadBlit::BlitScaled(s, target, 1, 2, Deki::ColorFormat::RGB565, 0, 0, 1, 2);
    EXPECT_EQ(Read565(target, 1, 0, 0), src[1]);
    EXPECT_EQ(Read565(target, 1, 0, 1), src[0]);
}

TEST_F(QuadBlitSpanRegressionTest, FlipD_Transposes)
{
    uint16_t src[4] = { Pack565(255, 0, 0), Pack565(0, 255, 0), Pack565(0, 0, 255), Pack565(255, 255, 0) };
    QuadBlit::Source s =
        QuadBlit::MakeSource(reinterpret_cast<const uint8_t*>(src), 2, 2, QuadBlit::PixelLayout::RGB565(), false);
    s.flipD = true;
    uint8_t target[2 * 2 * 2] = { 0 };
    QuadBlit::BlitScaled(s, target, 2, 2, Deki::ColorFormat::RGB565, 0, 0, 2, 2);
    EXPECT_EQ(Read565(target, 2, 0, 0), src[0]);
    EXPECT_EQ(Read565(target, 2, 1, 0), src[2]);  // (1,0) samples source (0,1)
    EXPECT_EQ(Read565(target, 2, 0, 1), src[1]);
    EXPECT_EQ(Read565(target, 2, 1, 1), src[3]);
}

TEST_F(QuadBlitSpanRegressionTest, NegativeSize_IsStillRejected)
{
    uint16_t src[1] = { Pack565(255, 0, 0) };
    QuadBlit::Source s =
        QuadBlit::MakeSource(reinterpret_cast<const uint8_t*>(src), 1, 1, QuadBlit::PixelLayout::RGB565(), false);
    uint8_t target[2] = { 0 };
    QuadBlit::BlitScaled(s, target, 1, 1, Deki::ColorFormat::RGB565, 0, 0, -1, 1);
    EXPECT_EQ(Read565(target, 1, 0, 0), 0u);
}

TEST_F(QuadBlitSpanRegressionTest, RotationPath_HalfTurnRotatesThePattern)
{
    uint16_t src[4] = { Pack565(255, 0, 0), Pack565(0, 255, 0), Pack565(0, 0, 255), Pack565(255, 255, 0) };
    QuadBlit::Source s =
        QuadBlit::MakeSource(reinterpret_cast<const uint8_t*>(src), 2, 2, QuadBlit::PixelLayout::RGB565(), false);
    const int w = 5, h = 5;
    uint8_t target[w * h * 2] = { 0 };
    QuadBlit::Blit(s, target, w, h, Deki::ColorFormat::RGB565, 2, 2, 1.0f, 1.0f, 3.14159265f, 0.5f, 0.5f);
    // Same corner-sampled geometry as the float version: the rotated quad
    // lands on columns/rows 2..3 with the pattern turned by 180 degrees.
    EXPECT_EQ(Read565(target, w, 2, 2), src[3]);
    EXPECT_EQ(Read565(target, w, 3, 2), src[2]);
    EXPECT_EQ(Read565(target, w, 2, 3), src[1]);
    EXPECT_EQ(Read565(target, w, 3, 3), src[0]);
    EXPECT_EQ(Read565(target, w, 1, 2), 0u);
    EXPECT_EQ(Read565(target, w, 0, 0), 0u);
}

TEST_F(QuadBlitSpanRegressionTest, RotationPath_ARGBTargetMatchesRGB565)
{
    const int s = 3;
    uint8_t src[s * s * 3];
    for (int i = 0; i < s * s; i++)
    {
        Put565A8(src, i, Pack565(255, 255, 255), 128);
    }
    QuadBlit::Source source = QuadBlit::MakeSource(src, s, s, QuadBlit::PixelLayout::RGB565A8(), false);
    const int w = 9, h = 9;
    uint8_t t565[w * h * 2];
    uint8_t t888[w * h * 4];
    for (int i = 0; i < w * h; i++)
    {
        reinterpret_cast<uint16_t*>(t565)[i] = DekiPixel::ToMemory565(Pack565(0, 0, 255));
        reinterpret_cast<uint32_t*>(t888)[i] = 0xFF0000FFu;
    }
    QuadBlit::Blit(source, t565, w, h, Deki::ColorFormat::RGB565, 4, 4, 1.0f, 1.0f, 0.7f, 0.5f, 0.5f, 255, 0, 0, 255);
    QuadBlit::Blit(source, t888, w, h, Deki::ColorFormat::ARGB8888, 4, 4, 1.0f, 1.0f, 0.7f, 0.5f, 0.5f, 255, 0, 0, 255);
    const uint16_t c565 = DekiPixel::FromMemory565(Read565(t565, w, 4, 4));
    const uint32_t c888 = reinterpret_cast<const uint32_t*>(t888)[4 * w + 4];
    // Both blends of half-alpha red over blue: red ~128, blue ~127 (5/6/5 quantised).
    EXPECT_NEAR((c565 >> 11) << 3, (c888 >> 16) & 0xFF, 8);
    EXPECT_NEAR((c565 & 0x1F) << 3, c888 & 0xFF, 8);
}

// ============================================================================
// The 1:1 tinted/blended RGB565 row loop against the pipeline's arithmetic
// ============================================================================

namespace
{
uint8_t RefDiv255(uint32_t x)
{
    return static_cast<uint8_t>((x + 1 + (x >> 8)) >> 8);
}

// What CompositePixel does for an RGB565A8 source on an RGB565 target, written
// out from scratch, so the fast loop is checked against the definition.
uint16_t RefComposite(uint16_t src, uint8_t a, uint16_t bg, uint8_t tR, uint8_t tG, uint8_t tB, uint8_t tA)
{
    if (a == 0)
    {
        return bg;
    }
    uint32_t r = (src >> 11) << 3, g = ((src >> 5) & 0x3F) << 2, b = (src & 0x1F) << 3;
    r = RefDiv255(r * tR);
    g = RefDiv255(g * tG);
    b = RefDiv255(b * tB);
    const uint32_t effA = tA != 255 ? RefDiv255(a * tA) : a;
    if (effA == 0)
    {
        return bg;
    }
    if (effA != 255)
    {
        const uint32_t inv = 255 - effA;
        r = RefDiv255(r * effA + ((bg >> 11) << 3) * inv);
        g = RefDiv255(g * effA + (((bg >> 5) & 0x3F) << 2) * inv);
        b = RefDiv255(b * effA + ((bg & 0x1F) << 3) * inv);
    }
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
}  // namespace

TEST(QuadBlitTintRows, EveryPixelMatchesThePipelinesArithmetic)
{
    constexpr int kW = 37, kH = 5;  // odd width: no alignment luck
    uint32_t seed = 12345;
    auto next = [&seed]()
    {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    const uint8_t tints[][4] = { { 255, 255, 255, 255 }, { 58, 91, 160, 255 }, { 255, 255, 255, 128 },
                                 { 200, 30, 255, 77 },  { 0, 0, 0, 255 },    { 255, 128, 64, 0 } };
    for (const auto& t : tints)
    {
        uint8_t src[kW * kH * 3];
        uint16_t target[kW * kH];
        uint16_t expected[kW * kH];
        for (int i = 0; i < kW * kH; ++i)
        {
            const uint16_t v = static_cast<uint16_t>(next());
            const uint32_t pick = next() % 4;
            const uint8_t a = pick == 0 ? 0 : pick == 1 ? 255 : static_cast<uint8_t>(next());
            DekiPixel::Store565(src + i * 3, v);
            src[i * 3 + 2] = a;
            target[i] = static_cast<uint16_t>(next());  // as stored
            expected[i] = t[3] == 0 ? target[i]
                                    : DekiPixel::ToMemory565(RefComposite(v, a, DekiPixel::FromMemory565(target[i]),
                                                                          t[0], t[1], t[2], t[3]));
        }
        const QuadBlit::Source source = QuadBlit::MakeSource(src, kW, kH, QuadBlit::PixelLayout::RGB565A8());
        QuadBlit::BlitScaled(source, reinterpret_cast<uint8_t*>(target), kW, kH, Deki::ColorFormat::RGB565, 0, 0, kW,
                             kH, t[0], t[1], t[2], t[3]);
        for (int i = 0; i < kW * kH; ++i)
        {
            ASSERT_EQ(target[i], expected[i]) << "pixel " << i << " tint " << int(t[0]) << "," << int(t[1]) << ","
                                              << int(t[2]) << "," << int(t[3]);
        }
    }
}

// The scaled RGB565A8-target loop (particles into their composite) against the
// scaled pipeline: its 16.16 stepping, the composite above, and the alpha union.
TEST(QuadBlitTintScaledRows, EveryPixelMatchesThePipelinesArithmetic)
{
    constexpr int kSW = 11, kSH = 7;  // source
    constexpr int kTW = 31, kTH = 13;  // target
    uint32_t seed = 777;
    auto next = [&seed]()
    {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    const uint8_t tints[][4] = { { 255, 255, 255, 255 }, { 58, 91, 160, 255 }, { 255, 255, 255, 128 },
                                 { 200, 30, 255, 77 } };
    // Destination rectangles: shrunk, grown, 1:1, and partly off the target.
    const int rects[][4] = { { 2, 1, 23, 9 }, { 0, 0, 5, 4 }, { 3, 2, kSW, kSH }, { 25, -3, 17, 10 } };
    for (const auto& t : tints)
    {
        for (const auto& r : rects)
        {
            uint8_t src[kSW * kSH * 3];
            for (int i = 0; i < kSW * kSH; ++i)
            {
                const uint16_t v = static_cast<uint16_t>(next());
                const uint32_t pick = next() % 4;
                DekiPixel::Store565(src + i * 3, v);
                src[i * 3 + 2] = pick == 0 ? 0 : pick == 1 ? 255 : static_cast<uint8_t>(next());
            }
            uint8_t target[kTW * kTH * 3];
            uint8_t expected[kTW * kTH * 3];
            for (int i = 0; i < kTW * kTH * 3; ++i)
            {
                target[i] = expected[i] = static_cast<uint8_t>(next());
            }
            const uint32_t xStep = (static_cast<uint32_t>(kSW) << 16) / static_cast<uint32_t>(r[2]);
            const uint32_t yStep = (static_cast<uint32_t>(kSH) << 16) / static_cast<uint32_t>(r[3]);
            for (int y = std::max(0, r[1]); y < std::min(kTH, r[1] + r[3]); ++y)
            {
                for (int x = std::max(0, r[0]); x < std::min(kTW, r[0] + r[2]); ++x)
                {
                    const int sx = static_cast<int>((static_cast<uint32_t>(x - r[0]) * xStep) >> 16);
                    const int sy = static_cast<int>((static_cast<uint32_t>(y - r[1]) * yStep) >> 16);
                    const uint8_t* sp = src + (sy * kSW + sx) * 3;
                    uint8_t* dp = expected + (y * kTW + x) * 3;
                    const uint8_t a = sp[2];
                    const uint32_t effA = t[3] != 255 ? RefDiv255(a * t[3]) : a;
                    if (a == 0 || effA == 0)
                    {
                        continue;
                    }
                    const uint16_t v = DekiPixel::Load565(sp);
                    const uint16_t bg = DekiPixel::Load565(dp);
                    DekiPixel::Store565(dp, RefComposite(v, a, bg, t[0], t[1], t[2], t[3]));
                    dp[2] = effA == 255 ? 255
                            : dp[2] == 0
                                ? static_cast<uint8_t>(effA)
                                : static_cast<uint8_t>(effA + RefDiv255(static_cast<uint32_t>(dp[2]) * (255 - effA)));
                }
            }
            const QuadBlit::Source source = QuadBlit::MakeSource(src, kSW, kSH, QuadBlit::PixelLayout::RGB565A8());
            QuadBlit::BlitScaled(source, target, kTW, kTH, Deki::ColorFormat::RGB565A8, r[0], r[1], r[2], r[3], t[0],
                                 t[1], t[2], t[3]);
            for (int i = 0; i < kTW * kTH * 3; ++i)
            {
                ASSERT_EQ(target[i], expected[i]) << "byte " << i << " rect " << r[0] << "," << r[1] << "," << r[2]
                                                  << "x" << r[3] << " tint " << int(t[0]) << "," << int(t[1]) << ","
                                                  << int(t[2]) << "," << int(t[3]);
            }
        }
    }
}

// ============================================================================
// CoversRect
// ============================================================================

// Yes only when the blit writes every pixel of the rect with the source's own
// colours: placed over all of it, opaque, untinted in alpha, unturned, and not
// clipped short of it.
TEST_F(QuadBlitClipTest, CoversRect_OnlyOpaqueBlitsOverAllOfTheRect)
{
    uint8_t px[4 * 4 * 3] = {};
    const QuadBlit::Source opaque = QuadBlit::MakeSource(px, 4, 4, QuadBlit::PixelLayout::RGB565());

    // 4 px at scale 3 from (10, 20), pivot top-left: 12 x 12.
    EXPECT_TRUE(QuadBlit::CoversRect(opaque, 10, 20, 3.0f, 3.0f, 0.0f, 0.0f, 0.0f, 255, 10, 20, 22, 32));
    EXPECT_TRUE(QuadBlit::CoversRect(opaque, 16, 26, 3.0f, 3.0f, 0.0f, 0.5f, 0.5f, 255, 10, 20, 22, 32))
        << "centre pivot";
    EXPECT_FALSE(QuadBlit::CoversRect(opaque, 10, 20, 3.0f, 3.0f, 0.0f, 0.0f, 0.0f, 255, 9, 20, 22, 32));
    EXPECT_FALSE(QuadBlit::CoversRect(opaque, 10, 20, 3.0f, 3.0f, 0.0f, 0.0f, 0.0f, 255, 10, 19, 22, 32));
    EXPECT_FALSE(QuadBlit::CoversRect(opaque, 10, 20, 3.0f, 3.0f, 0.0f, 0.0f, 0.0f, 255, 10, 20, 23, 32));
    EXPECT_FALSE(QuadBlit::CoversRect(opaque, 10, 20, 3.0f, 3.0f, 0.0f, 0.0f, 0.0f, 255, 10, 20, 22, 33));

    // A colour tint still writes every pixel; an alpha tint or a turn may not.
    EXPECT_FALSE(QuadBlit::CoversRect(opaque, 10, 20, 3.0f, 3.0f, 0.0f, 0.0f, 0.0f, 254, 10, 20, 22, 32));
    EXPECT_FALSE(QuadBlit::CoversRect(opaque, 10, 20, 3.0f, 3.0f, 0.1f, 0.0f, 0.0f, 255, 10, 20, 22, 32));

    // Sources that can leave holes.
    EXPECT_FALSE(QuadBlit::CoversRect(QuadBlit::MakeSource(px, 4, 4, QuadBlit::PixelLayout::RGB565A8()), 10, 20, 3.0f,
                                      3.0f, 0.0f, 0.0f, 0.0f, 255, 10, 20, 22, 32));
    EXPECT_TRUE(QuadBlit::CoversRect(QuadBlit::MakeSource(px, 4, 4, QuadBlit::PixelLayout::RGB565A8NoAlpha()), 10, 20,
                                     3.0f, 3.0f, 0.0f, 0.0f, 0.0f, 255, 10, 20, 22, 32));
    EXPECT_FALSE(QuadBlit::CoversRect(QuadBlit::MakeSource(px, 4, 4, QuadBlit::PixelLayout::Alpha8()), 10, 20, 3.0f,
                                      3.0f, 0.0f, 0.0f, 0.0f, 255, 10, 20, 22, 32));
    QuadBlit::Source keyed = opaque;
    keyed.hasChromaKey = true;
    EXPECT_FALSE(QuadBlit::CoversRect(keyed, 10, 20, 3.0f, 3.0f, 0.0f, 0.0f, 0.0f, 255, 10, 20, 22, 32));

    // The clip stack: short of the rect, around it, switched off.
    QuadBlit::PushClipRect(10, 20, 22, 31);
    EXPECT_FALSE(QuadBlit::CoversRect(opaque, 10, 20, 3.0f, 3.0f, 0.0f, 0.0f, 0.0f, 255, 10, 20, 22, 32));
    QuadBlit::SetClipEnabled(false);
    EXPECT_TRUE(QuadBlit::CoversRect(opaque, 10, 20, 3.0f, 3.0f, 0.0f, 0.0f, 0.0f, 255, 10, 20, 22, 32));
    QuadBlit::SetClipEnabled(true);
    QuadBlit::PopClipRect();
    QuadBlit::PushClipRect(-100, -100, 100, 100);
    EXPECT_TRUE(QuadBlit::CoversRect(opaque, 10, 20, 3.0f, 3.0f, 0.0f, 0.0f, 0.0f, 255, 10, 20, 22, 32));
    QuadBlit::PopClipRect();

    // A rect above the target's first row (a later strip's whole view): only
    // where the blit lands counts.
    EXPECT_TRUE(QuadBlit::CoversRect(opaque, 0, -8, 4.0f, 4.0f, 0.0f, 0.0f, 0.0f, 255, 0, -8, 16, 8));
}
