#include "QuadBlit.h"
#include "PixelFormat.h"
#include "DirtyRegion.h"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <vector>

// Deki::ColorFormat comes from the engine header. Never re-declare it here: a
// second definition is a redefinition error once a unity build puts this file
// in a translation unit that includes DekiEngine.h.
#include <deki/Engine.h>
#include <deki/LogSystem.h>

// ============================================================================
// How this file is organised
// ============================================================================
//
// Every blit - scaled or rotated, any of the five source layouts onto any of
// the four target formats - runs one pixel pipeline, CompositePixel<SK, F>:
// read the source pixel, drop it if transparent or chroma-keyed, tint, apply
// the alpha tint, then dither / write opaque / blend with the destination.
// The source layout (SrcKind) and the target format are template parameters,
// so each blit resolves them once. The per-blit flags (tint, alpha tint, key,
// dither, flips) are runtime booleans hoisted out of the loops, and a separate
// "Plain" instantiation (none of them set) keeps the common
// sprite-onto-framebuffer loop tight.
//
// One pipeline keeps every format pair behaving the same way; do not add
// per-pair kernels that compute pixels differently. tests/GoldenBlitTests.cpp
// pins the output of every path.
//
// The 1:1 fast paths that matter on the ESP32 sit beside it and produce the
// same pixels: whole-row copies and the registered SIMD row kernels, the
// per-row opaque-span split for RGB565A8 sprites, and the chroma-key span copy
// for RGB565 sprites.

using DekiPixel::AlphaUnion;
using DekiPixel::BayerThreshold;
using DekiPixel::Div255;
using DekiPixel::PackRGB565;
using DekiPixel::UnpackRGB565;
using DekiPixel::SrcKind;
using DekiPixel::ReadSrcPixel;
using DekiPixel::ReadDstPixel;
using DekiPixel::WriteDstPixel;

namespace QuadBlit
{

// ============================================================================
// Kernel dispatch table
// ============================================================================
// Null by default. Platform packages call RegisterKernel(op, fn) at init to
// plug in SIMD implementations. The blit dispatcher looks for an entry only
// when all preconditions hold (format, no scale, no rotation, alignment, and
// no tint where it applies).

static RowKernelFn s_Kernels[(int)KernelOp::Count] = {};

// KernelOp is uint8_t, so only the upper bound is checked. A `(int)op < 0`
// test is always false, which GCC 15 warns about and ESP-IDF 6 makes an error.
void RegisterKernel(KernelOp op, RowKernelFn fn)
{
    if ((int)op >= (int)KernelOp::Count)
    {
        return;
    }
    s_Kernels[(int)op] = fn;
}

RowKernelFn GetKernel(KernelOp op)
{
    if ((int)op >= (int)KernelOp::Count)
    {
        return nullptr;
    }
    return s_Kernels[(int)op];
}

// True when both pointers are 16-byte aligned, as the SIMD (PIE) kernels need.
static inline bool Aligned16(const void* a, const void* b)
{
    return ((uintptr_t)a & 0xF) == 0 && ((uintptr_t)b & 0xF) == 0;
}

// ============================================================================
// Clip rect stack
// ============================================================================

// Grows with the nesting depth of the scene, with no fixed limit. Capacity
// persists across frames (ClearClipStack only clears).
static std::vector<ClipRect> s_ClipStack;
static bool s_ClipEnabled = true;

void PushClipRect(int32_t left, int32_t top, int32_t right, int32_t bottom)
{
    ClipRect rect = { left, top, right, bottom };

    // Intersect with the parent clip rect.
    if (!s_ClipStack.empty())
    {
        const ClipRect& parent = s_ClipStack.back();
        rect.left = std::max(rect.left, parent.left);
        rect.top = std::max(rect.top, parent.top);
        rect.right = std::min(rect.right, parent.right);
        rect.bottom = std::min(rect.bottom, parent.bottom);
    }

    s_ClipStack.push_back(rect);
}

void PopClipRect()
{
    if (!s_ClipStack.empty())
    {
        s_ClipStack.pop_back();
    }
}

ClipRect GetCurrentClipRect()
{
    if (!s_ClipEnabled || s_ClipStack.empty())
    {
        return ClipRect{};
    }
    return s_ClipStack.back();
}

void ClearClipStack()
{
    s_ClipStack.clear();
    s_ClipEnabled = true;
}

void SetClipEnabled(bool enabled)
{
    s_ClipEnabled = enabled;
}

bool IsClipEnabled()
{
    return s_ClipEnabled;
}

int GetClipStackDepth()
{
    return static_cast<int>(s_ClipStack.size());
}

// ============================================================================
// Dirty-rect tracking
// ============================================================================

static DekiRendering::DirtyRegion* s_DirtyRegion = nullptr;
static const uint8_t* s_DirtyTarget = nullptr;

void SetDirtyTracking(DekiRendering::DirtyRegion* region, const uint8_t* trackedTarget)
{
    s_DirtyRegion = region;
    s_DirtyTarget = region ? trackedTarget : nullptr;
}

void MarkDirty(int32_t left, int32_t top, int32_t right, int32_t bottom)
{
    if (s_DirtyRegion)
    {
        s_DirtyRegion->Add(left, top, right, bottom);
    }
}

void MarkAllDirty()
{
    if (s_DirtyRegion)
    {
        s_DirtyRegion->SetFull();
    }
}

const uint8_t* GetDirtyTrackedTarget()
{
    return s_DirtyTarget;
}

// The rectangle a blit is about to write, once clipped, is exactly what the
// frame changed there.
static inline void NoteBlitRect(const uint8_t* target, int32_t startX, int32_t startY, int32_t endX, int32_t endY)
{
    if (s_DirtyRegion && target == s_DirtyTarget)
    {
        s_DirtyRegion->Add(startX, startY, endX, endY);
    }
}

// ============================================================================
// Source creation
// ============================================================================

Source MakeSource(const uint8_t* pixels, int32_t width, int32_t height, PixelLayout layout, bool ownsPixels,
                  const int16_t* alphaRowSpans)
{
    Source src;
    src.pixels = pixels;
    src.width = width;
    src.height = height;
    src.bytesPerPixel = layout.bytesPerPixel;
    src.hasAlpha = layout.hasAlpha;
    src.isRGB565 = layout.isRGB565;
    src.ownsPixels = ownsPixels;
    src.alphaRowSpans = alphaRowSpans;
    src.stride = 0;
    src.hasChromaKey = false;
    src.keyR = 0;
    src.keyG = 0;
    src.keyB = 0;
    src.chromaRowSpans = nullptr;

    if (layout.hasAlpha)
    {
        src.alphaOffset = layout.isRGB565 ? 2 : 3;
    }
    else
    {
        src.alphaOffset = 0;
    }

    return src;
}

// Bytes per row of a Source buffer. Source::stride 0 means tightly packed
// (each row right after the previous); a non-zero stride lets a Source point
// at a sub-rect of a larger buffer (e.g. a tile in an atlas) without a copy.
static inline int32_t SourceStride(const Source& s)
{
    return s.stride ? s.stride : s.width * s.bytesPerPixel;
}

// Maps a sampled source coordinate through the Source's flip flags. This is
// the inverse of Tiled's order (transpose, then H, then V), so it applies V,
// then H, then D.
static inline void ApplyFlips(const Source& s, int32_t& x, int32_t& y)
{
    if (s.flipV)
    {
        y = s.height - 1 - y;
    }
    if (s.flipH)
    {
        x = s.width - 1 - x;
    }
    if (s.flipD)
    {
        // A transpose only makes sense for a square source; Tiled sets it only
        // on tiles, which are square. Anything else keeps its orientation.
        if (s.width == s.height)
        {
            const int32_t t = x;
            x = y;
            y = t;
        }
    }
}

static inline bool HasFlips(const Source& s)
{
    return s.flipH || s.flipV || s.flipD;
}

// ============================================================================
// Clipping bounds, shared by BlitScaled and Blit
// ============================================================================

struct BlitBounds
{
    int32_t startX, startY, endX, endY;
};

static inline bool ComputeClipBounds(int32_t destX, int32_t destY, int32_t destWidth, int32_t destHeight,
                                     int32_t targetWidth, int32_t targetHeight, BlitBounds& out)
{
    ClipRect clip = GetCurrentClipRect();

    out.startX = std::max<int32_t>(0, std::max(destX, clip.left));
    out.startY = std::max<int32_t>(0, std::max(destY, clip.top));
    out.endX = std::min<int32_t>(targetWidth, std::min(destX + destWidth, clip.right));
    out.endY = std::min<int32_t>(targetHeight, std::min(destY + destHeight, clip.bottom));
    return out.startX < out.endX && out.startY < out.endY;
}

// ============================================================================
// Pixel formats
// ============================================================================

// The SrcKind of a Source; see SrcKind in PixelFormat.h for the layouts.
static inline SrcKind KindOf(const Source& s)
{
    if (s.isRGB565)
    {
        return s.bytesPerPixel >= 3 ? SrcKind::RGB565A8 : SrcKind::RGB565;
    }
    if (s.bytesPerPixel == 4)
    {
        return SrcKind::RGBA8888;
    }
    if (s.bytesPerPixel == 3)
    {
        return SrcKind::RGB888;
    }
    return SrcKind::ALPHA8;
}

// ============================================================================
// The pixel pipeline
// ============================================================================

struct BlitParams
{
    bool hasTint = false;
    bool hasAlphaTint = false;
    bool hasKey = false;
    bool dither = false;  // ordered dither instead of alpha blend (alpha sources only)
    bool flips = false;
    uint8_t tintR = 255, tintG = 255, tintB = 255, tintA = 255;
    uint8_t keyR = 0, keyG = 0, keyB = 0;
};

// Composites the source pixel at `sp` onto destination pixel `idx`; px, py
// pick the dither threshold. Plain means no tint, alpha tint, key or dither:
// the tight loop for the common sprite blit.
template <SrcKind SK, Deki::ColorFormat F, bool Plain>
static inline void CompositePixel(const Source& source, const uint8_t* sp, uint8_t* target, size_t idx, int32_t px,
                                  int32_t py, const BlitParams& p)
{
    uint8_t r, g, b, a;
    ReadSrcPixel<SK>(sp, source.hasAlpha, r, g, b, a);
    if (a == 0)
    {
        return;
    }

    if constexpr (Plain)
    {
        if (a == 255)
        {
            WriteDstPixel<F>(target, idx, r, g, b, 255);
            return;
        }
        uint8_t bgR, bgG, bgB, bgA;
        ReadDstPixel<F>(target, idx, bgR, bgG, bgB, bgA);
        const uint32_t invA = 255u - a;
        WriteDstPixel<F>(target, idx, Div255(r * a + bgR * invA), Div255(g * a + bgG * invA),
                         Div255(b * a + bgB * invA), AlphaUnion(a, bgA));
        return;
    }
    else
    {
        // The key is compared against the untinted colour.
        if (p.hasKey && r == p.keyR && g == p.keyG && b == p.keyB)
        {
            return;
        }

        if (p.hasTint)
        {
            r = Div255(r * p.tintR);
            g = Div255(g * p.tintG);
            b = Div255(b * p.tintB);
        }

        const uint8_t effA = p.hasAlphaTint ? Div255(a * p.tintA) : a;
        if (effA == 0)
        {
            return;
        }

        if (p.dither)
        {
            // 255 always passes, since the matrix tops out at 252.
            if (effA <= BayerThreshold(px, py))
            {
                return;
            }
            WriteDstPixel<F>(target, idx, r, g, b, 255);
            return;
        }

        if (effA == 255)
        {
            WriteDstPixel<F>(target, idx, r, g, b, 255);
            return;
        }

        uint8_t bgR, bgG, bgB, bgA;
        ReadDstPixel<F>(target, idx, bgR, bgG, bgB, bgA);
        const uint32_t invA = 255u - effA;
        WriteDstPixel<F>(target, idx, Div255(r * effA + bgR * invA), Div255(g * effA + bgG * invA),
                         Div255(b * effA + bgB * invA), AlphaUnion(effA, bgA));
    }
}

// ============================================================================
// 1:1 row fast paths: opaque copies, span splits, SIMD hooks
// ============================================================================
// Each returns true when it handled the whole blit. They exist only for
// speed: the pipeline above produces the same pixels.

// RGB565 -> RGB565, no tint or key: row copy (the SIMD kernel when aligned).
static DEKI_FAST_ATTR bool CopyRows_RGB565(const Source& source, uint16_t* target16, int32_t targetWidth, int32_t destX,
                                           int32_t destY, const BlitBounds& b)
{
    const int32_t stride = SourceStride(source);
    const int32_t rowPixels = b.endX - b.startX;
    RowKernelFn copyKernel = s_Kernels[(int)KernelOp::RGB565CopyRow];
    for (int32_t py = b.startY; py < b.endY; py++)
    {
        const uint16_t* srcPtr = (const uint16_t*)(source.pixels + (py - destY) * stride) + (b.startX - destX);
        uint16_t* dstPtr = target16 + py * targetWidth + b.startX;
        if (copyKernel && Aligned16(srcPtr, dstPtr))
        {
            copyKernel((const uint8_t*)srcPtr, (uint8_t*)dstPtr, rowPixels, 255, 255, 255, 255);
        }
        else
        {
            memcpy(dstPtr, srcPtr, rowPixels * sizeof(uint16_t));
        }
    }
    return true;
}

// RGB565 -> RGB565 with a chroma key and per-row non-key spans, no tint.
// Inside [start, end) no pixel is the key (straight copy); outside, every
// pixel is the key (skipped without a read). A row stored as (-1, -1) has a
// key pixel inside its run and is compared pixel by pixel.
static DEKI_FAST_ATTR bool CopyRows_RGB565_ChromaSpans(const Source& source, uint16_t* target16, int32_t targetWidth,
                                                       int32_t destX, int32_t destY, const BlitBounds& b)
{
    const int32_t stride = SourceStride(source);
    const int16_t* spans = source.chromaRowSpans;
    RowKernelFn copyKernel = s_Kernels[(int)KernelOp::RGB565CopyRow];
    // The key is quantised to 5/6/5, so comparing packed pixels is the same
    // test the per-pixel pipeline makes on the unpacked channels.
    const uint16_t key565 =
        static_cast<uint16_t>(((source.keyR >> 3) << 11) | ((source.keyG >> 2) << 5) | (source.keyB >> 3));
    for (int32_t py = b.startY; py < b.endY; py++)
    {
        const int32_t srcY = py - destY;
        const int32_t srcStartX = b.startX - destX;
        const int32_t srcEndX = b.endX - destX;
        if (spans[srcY * 2] < 0)
        {
            const uint16_t* srcRow = (const uint16_t*)(source.pixels + srcY * stride);
            uint16_t* dstRow = target16 + py * targetWidth + destX;
            for (int32_t x = srcStartX; x < srcEndX; ++x)
            {
                const uint16_t v = srcRow[x];
                if (v != key565)
                {
                    dstRow[x] = v;
                }
            }
            continue;
        }
        const int32_t clampedStart = std::max<int32_t>(spans[srcY * 2], srcStartX);
        const int32_t clampedEnd = std::min<int32_t>(spans[srcY * 2 + 1], srcEndX);
        if (clampedStart >= clampedEnd)
        {
            continue;
        }
        const uint16_t* srcPtr = (const uint16_t*)(source.pixels + srcY * stride) + clampedStart;
        uint16_t* dstPtr = target16 + py * targetWidth + (destX + clampedStart);
        const int32_t rowPixels = clampedEnd - clampedStart;
        if (copyKernel && Aligned16(srcPtr, dstPtr))
        {
            copyKernel((const uint8_t*)srcPtr, (uint8_t*)dstPtr, rowPixels, 255, 255, 255, 255);
        }
        else
        {
            memcpy(dstPtr, srcPtr, rowPixels * sizeof(uint16_t));
        }
    }
    return true;
}

// RGB565A8 with alpha -> RGB565, no tint or key: a per-row opaque-span split
// (left blend | opaque copy | right blend) when spans are available,
// otherwise the SIMD blend kernel when aligned. Rows neither can take go
// through the plain pipeline.
static DEKI_FAST_ATTR bool BlendRows_RGB565A8_to_RGB565(const Source& source, uint16_t* target16, int32_t targetWidth,
                                                        int32_t destX, int32_t destY, const BlitBounds& b)
{
    const int32_t stride = SourceStride(source);
    const int32_t bpp = source.bytesPerPixel;
    const int16_t* rowSpans = source.alphaRowSpans;
    RowKernelFn blendKernel = s_Kernels[(int)KernelOp::RGB565A8BlendRow];
    const BlitParams plain;

    for (int32_t py = b.startY; py < b.endY; py++)
    {
        const int32_t srcY = py - destY;
        const uint8_t* rowBase = source.pixels + srcY * stride;
        uint16_t* dstRow = target16 + py * targetWidth;
        const int32_t srcStartX = b.startX - destX;
        const int32_t srcEndX = b.endX - destX;
        const size_t rowIdx = (size_t)py * (size_t)targetWidth;

        if (rowSpans)
        {
            const int32_t clampedStart = std::max<int32_t>(rowSpans[srcY * 2], srcStartX);
            const int32_t clampedEnd = std::min<int32_t>(rowSpans[srcY * 2 + 1], srcEndX);

            // Left alpha region.
            for (int32_t sx = srcStartX; sx < clampedStart && sx < srcEndX; sx++)
            {
                CompositePixel<SrcKind::RGB565A8, Deki::ColorFormat::RGB565, true>(
                    source, rowBase + sx * bpp, (uint8_t*)target16, rowIdx + destX + sx, destX + sx, py, plain);
            }

            // Opaque middle: direct copy, no alpha checks.
            const uint8_t* srcPtr = rowBase + clampedStart * bpp;
            for (int32_t sx = clampedStart; sx < clampedEnd; sx++, srcPtr += bpp)
            {
                memcpy(&dstRow[destX + sx], srcPtr, 2);
            }

            // Right alpha region. Starts at the clip start when the opaque span
            // ends before it or is empty; starting at opaqueEnd then would
            // write pixels the clip rect excludes.
            for (int32_t sx = std::max(clampedEnd, srcStartX); sx < srcEndX; sx++)
            {
                CompositePixel<SrcKind::RGB565A8, Deki::ColorFormat::RGB565, true>(
                    source, rowBase + sx * bpp, (uint8_t*)target16, rowIdx + destX + sx, destX + sx, py, plain);
            }
            continue;
        }

        const uint8_t* srcPtr = rowBase + srcStartX * bpp;
        uint16_t* dstPtr = dstRow + b.startX;
        if (blendKernel && bpp == 3 && Aligned16(srcPtr, dstPtr))
        {
            blendKernel(srcPtr, (uint8_t*)dstPtr, b.endX - b.startX, 255, 255, 255, 255);
            continue;
        }
        for (int32_t px = b.startX; px < b.endX; px++, srcPtr += bpp)
        {
            CompositePixel<SrcKind::RGB565A8, Deki::ColorFormat::RGB565, true>(source, srcPtr, (uint8_t*)target16,
                                                                               rowIdx + px, px, py, plain);
        }
    }
    return true;
}

// RGB565 -> RGB565A8, no tint or key: opaque expand (the SIMD kernel when aligned).
static DEKI_FAST_ATTR bool ExpandRows_RGB565_to_RGB565A8(const Source& source, uint8_t* target, int32_t targetWidth,
                                                         int32_t destX, int32_t destY, const BlitBounds& b)
{
    const int32_t stride = SourceStride(source);
    const int32_t rowPixels = b.endX - b.startX;
    RowKernelFn expandKernel = s_Kernels[(int)KernelOp::RGB565ToRGB565A8Row];
    for (int32_t py = b.startY; py < b.endY; py++)
    {
        const uint16_t* srcPtr = (const uint16_t*)(source.pixels + (py - destY) * stride) + (b.startX - destX);
        uint8_t* dstPtr = target + (py * targetWidth + b.startX) * 3;
        if (expandKernel && Aligned16(srcPtr, dstPtr))
        {
            expandKernel((const uint8_t*)srcPtr, dstPtr, rowPixels, 255, 255, 255, 255);
            continue;
        }
        for (int32_t i = 0; i < rowPixels; i++)
        {
            memcpy(dstPtr + i * 3, srcPtr + i, 2);
            dstPtr[i * 3 + 2] = 0xFF;
        }
    }
    return true;
}

// RGB565A8 -> RGB565A8, no tint or key: opaque copy when the source declares
// no alpha, otherwise the SIMD blend kernel when aligned.
static DEKI_FAST_ATTR bool Rows_RGB565A8_to_RGB565A8(const Source& source, uint8_t* target, int32_t targetWidth,
                                                     int32_t destX, int32_t destY, const BlitBounds& b)
{
    const int32_t stride = SourceStride(source);
    const int32_t bpp = source.bytesPerPixel;
    if (bpp != 3)
    {
        return false;
    }
    const int32_t rowPixels = b.endX - b.startX;
    const BlitParams plain;
    for (int32_t py = b.startY; py < b.endY; py++)
    {
        const uint8_t* srcPtr = source.pixels + (py - destY) * stride + (b.startX - destX) * bpp;
        uint8_t* dstPtr = target + (py * targetWidth + b.startX) * 3;
        if (!source.hasAlpha)
        {
            RowKernelFn copyKernel = s_Kernels[(int)KernelOp::RGB565A8CopyRow];
            if (copyKernel && Aligned16(srcPtr, dstPtr))
            {
                copyKernel(srcPtr, dstPtr, rowPixels, 255, 255, 255, 255);
                continue;
            }
            // Byte 2 of the source is ignored: the source declares itself opaque.
            for (int32_t i = 0; i < rowPixels; i++)
            {
                dstPtr[i * 3] = srcPtr[i * 3];
                dstPtr[i * 3 + 1] = srcPtr[i * 3 + 1];
                dstPtr[i * 3 + 2] = 0xFF;
            }
            continue;
        }
        RowKernelFn blendKernel = s_Kernels[(int)KernelOp::RGB565A8BlendRowDestRGB565A8];
        if (blendKernel && Aligned16(srcPtr, dstPtr))
        {
            blendKernel(srcPtr, dstPtr, rowPixels, 255, 255, 255, 255);
            continue;
        }
        const size_t rowIdx = (size_t)py * (size_t)targetWidth;
        for (int32_t px = b.startX; px < b.endX; px++, srcPtr += 3)
        {
            CompositePixel<SrcKind::RGB565A8, Deki::ColorFormat::RGB565A8, true>(source, srcPtr, target, rowIdx + px,
                                                                                 px, py, plain);
        }
    }
    return true;
}

// ============================================================================
// Scaled blit: 16.16 fixed-point stepping (1:1 is a step of 65536)
// ============================================================================

template <SrcKind SK, Deki::ColorFormat F, bool Plain>
static DEKI_FAST_ATTR void BlitRows(const Source& source, uint8_t* target, int32_t targetWidth, int32_t destX,
                                    int32_t destY, int32_t destWidth, int32_t destHeight, const BlitBounds& b,
                                    const BlitParams& P)
{
    const int32_t bpp = source.bytesPerPixel;
    const int32_t stride = SourceStride(source);
    const uint32_t xStep = ((uint32_t)source.width << 16) / (uint32_t)destWidth;
    const uint32_t yStep = ((uint32_t)source.height << 16) / (uint32_t)destHeight;

    for (int32_t py = b.startY; py < b.endY; py++)
    {
        const int32_t srcY = (int32_t)(((uint32_t)(py - destY) * yStep) >> 16);
        const uint8_t* srcRow = source.pixels + srcY * stride;
        const size_t rowIdx = (size_t)py * (size_t)targetWidth;

        uint32_t acc = (uint32_t)(b.startX - destX) * xStep;
        for (int32_t px = b.startX; px < b.endX; px++)
        {
            const int32_t srcX = (int32_t)(acc >> 16);
            acc += xStep;
            const uint8_t* sp;
            if constexpr (!Plain)
            {
                if (P.flips)
                {
                    // Copy per pixel: a transpose must not rewrite the row's Y.
                    int32_t fx = srcX, fy = srcY;
                    ApplyFlips(source, fx, fy);
                    sp = source.pixels + fy * stride + fx * bpp;
                }
                else
                {
                    sp = srcRow + srcX * bpp;
                }
            }
            else
            {
                sp = srcRow + srcX * bpp;
            }
            CompositePixel<SK, F, Plain>(source, sp, target, rowIdx + px, px, py, P);
        }
    }
}

template <Deki::ColorFormat F, bool Plain>
static void BlitRowsForTarget(SrcKind kind, const Source& source, uint8_t* target, int32_t targetWidth, int32_t destX,
                              int32_t destY, int32_t destWidth, int32_t destHeight, const BlitBounds& b,
                              const BlitParams& p)
{
    switch (kind)
    {
        case SrcKind::RGB565:
            BlitRows<SrcKind::RGB565, F, Plain>(source, target, targetWidth, destX, destY, destWidth, destHeight, b, p);
            break;
        case SrcKind::RGB565A8:
            BlitRows<SrcKind::RGB565A8, F, Plain>(source, target, targetWidth, destX, destY, destWidth, destHeight, b,
                                                  p);
            break;
        case SrcKind::RGBA8888:
            BlitRows<SrcKind::RGBA8888, F, Plain>(source, target, targetWidth, destX, destY, destWidth, destHeight, b,
                                                  p);
            break;
        case SrcKind::RGB888:
            BlitRows<SrcKind::RGB888, F, Plain>(source, target, targetWidth, destX, destY, destWidth, destHeight, b, p);
            break;
        case SrcKind::ALPHA8:
            BlitRows<SrcKind::ALPHA8, F, Plain>(source, target, targetWidth, destX, destY, destWidth, destHeight, b, p);
            break;
    }
}

template <bool Plain>
static void BlitRowsDispatch(SrcKind kind, Deki::ColorFormat targetFormat, const Source& source, uint8_t* target,
                             int32_t targetWidth, int32_t destX, int32_t destY, int32_t destWidth, int32_t destHeight,
                             const BlitBounds& b, const BlitParams& p)
{
    switch (targetFormat)
    {
        case Deki::ColorFormat::RGB565:
            BlitRowsForTarget<Deki::ColorFormat::RGB565, Plain>(kind, source, target, targetWidth, destX, destY,
                                                                destWidth, destHeight, b, p);
            break;
        case Deki::ColorFormat::RGB888:
            BlitRowsForTarget<Deki::ColorFormat::RGB888, Plain>(kind, source, target, targetWidth, destX, destY,
                                                                destWidth, destHeight, b, p);
            break;
        case Deki::ColorFormat::ARGB8888:
            BlitRowsForTarget<Deki::ColorFormat::ARGB8888, Plain>(kind, source, target, targetWidth, destX, destY,
                                                                  destWidth, destHeight, b, p);
            break;
        case Deki::ColorFormat::RGB565A8:
            BlitRowsForTarget<Deki::ColorFormat::RGB565A8, Plain>(kind, source, target, targetWidth, destX, destY,
                                                                  destWidth, destHeight, b, p);
            break;
    }
}

void BlitScaled(const Source& source, uint8_t* target, int32_t targetWidth, int32_t targetHeight,
                Deki::ColorFormat targetFormat, int32_t destX, int32_t destY, int32_t destWidth, int32_t destHeight,
                uint8_t tintR, uint8_t tintG, uint8_t tintB, uint8_t tintA, bool useOrderedDither)
{
    if (!source.pixels || !target || source.width <= 0 || source.height <= 0)
    {
        return;
    }
    if (tintA == 0)
    {
        return;
    }
    if (destWidth <= 0 || destHeight <= 0)
    {
        return;
    }

    BlitBounds bounds;
    if (!ComputeClipBounds(destX, destY, destWidth, destHeight, targetWidth, targetHeight, bounds))
    {
        return;
    }
    NoteBlitRect(target, bounds.startX, bounds.startY, bounds.endX, bounds.endY);

    BlitParams p;
    p.hasTint = (tintR != 255 || tintG != 255 || tintB != 255);
    p.hasAlphaTint = (tintA != 255);
    p.hasKey = source.hasChromaKey;
    // Dithering only applies when the source has alpha to dither.
    p.dither = useOrderedDither && source.hasAlpha;
    p.flips = HasFlips(source);
    p.tintR = tintR;
    p.tintG = tintG;
    p.tintB = tintB;
    p.tintA = tintA;
    p.keyR = source.keyR;
    p.keyG = source.keyG;
    p.keyB = source.keyB;

    const SrcKind kind = KindOf(source);
    const bool oneToOne = (destWidth == source.width && destHeight == source.height);
    const bool plain = !p.hasTint && !p.hasAlphaTint && !p.hasKey && !p.dither && !p.flips;

    // 1:1 row fast paths: the same pixels as the pipeline, in fewer instructions.
    if (oneToOne && !p.hasTint && !p.hasAlphaTint && !p.dither && !p.flips)
    {
        if (targetFormat == Deki::ColorFormat::RGB565)
        {
            uint16_t* target16 = (uint16_t*)target;
            if (kind == SrcKind::RGB565 && p.hasKey && source.chromaRowSpans)
            {
                CopyRows_RGB565_ChromaSpans(source, target16, targetWidth, destX, destY, bounds);
                return;
            }
            if (plain && kind == SrcKind::RGB565)
            {
                CopyRows_RGB565(source, target16, targetWidth, destX, destY, bounds);
                return;
            }
            if (plain && kind == SrcKind::RGB565A8 && source.hasAlpha)
            {
                BlendRows_RGB565A8_to_RGB565(source, target16, targetWidth, destX, destY, bounds);
                return;
            }
        }
        else if (targetFormat == Deki::ColorFormat::RGB565A8 && plain)
        {
            if (kind == SrcKind::RGB565)
            {
                ExpandRows_RGB565_to_RGB565A8(source, target, targetWidth, destX, destY, bounds);
                return;
            }
            if (kind == SrcKind::RGB565A8 &&
                Rows_RGB565A8_to_RGB565A8(source, target, targetWidth, destX, destY, bounds))
            {
                return;
            }
        }
    }

    if (plain)
    {
        BlitRowsDispatch<true>(kind, targetFormat, source, target, targetWidth, destX, destY, destWidth, destHeight,
                               bounds, p);
    }
    else
    {
        BlitRowsDispatch<false>(kind, targetFormat, source, target, targetWidth, destX, destY, destWidth, destHeight,
                                bounds, p);
    }
}

// ============================================================================
// Rotated blit
// ============================================================================
// Inverse-maps every destination pixel of the rotated quad's bounding box back
// into the source with a 16.16 fixed-point DDA: two adds per pixel, then the
// same pipeline as the scaled path.

struct RotatedBlitArgs
{
    int32_t startX, startY, endX, endY;  // destination rows/columns to visit
    int32_t rowSx, rowSy;                // 16.16 source coords of (startX, startY)
    int32_t dSxDx, dSyDx;                // per-column source step
    int32_t dSxDy, dSyDy;                // per-row source step
};

template <SrcKind SK, Deki::ColorFormat F>
static DEKI_FAST_ATTR void RotatedBlitT(const Source& source, uint8_t* target, int32_t targetWidth,
                                        const RotatedBlitArgs& a, const BlitParams& P)
{
    const int32_t stride = SourceStride(source);
    const int32_t bpp = source.bytesPerPixel;
    const uint32_t srcW = (uint32_t)source.width, srcH = (uint32_t)source.height;

    int32_t rowSx = a.rowSx, rowSy = a.rowSy;
    for (int32_t py = a.startY; py < a.endY; py++, rowSx += a.dSxDy, rowSy += a.dSyDy)
    {
        int32_t sx = rowSx, sy = rowSy;
        const size_t rowIdx = (size_t)py * (size_t)targetWidth;
        for (int32_t px = a.startX; px < a.endX; px++, sx += a.dSxDx, sy += a.dSyDx)
        {
            // Arithmetic shift keeps negatives negative; the unsigned compare
            // then rejects them together with the far edge in one test.
            int32_t ix = sx >> 16, iy = sy >> 16;
            if ((uint32_t)ix >= srcW || (uint32_t)iy >= srcH)
            {
                continue;
            }
            if (P.flips)
            {
                ApplyFlips(source, ix, iy);
            }
            CompositePixel<SK, F, false>(source, source.pixels + iy * stride + ix * bpp, target, rowIdx + px, px, py,
                                         P);
        }
    }
}

template <Deki::ColorFormat F>
static void RotatedBlitForTarget(SrcKind kind, const Source& source, uint8_t* target, int32_t targetWidth,
                                 const RotatedBlitArgs& a, const BlitParams& p)
{
    switch (kind)
    {
        case SrcKind::RGB565: RotatedBlitT<SrcKind::RGB565, F>(source, target, targetWidth, a, p); break;
        case SrcKind::RGB565A8: RotatedBlitT<SrcKind::RGB565A8, F>(source, target, targetWidth, a, p); break;
        case SrcKind::RGBA8888: RotatedBlitT<SrcKind::RGBA8888, F>(source, target, targetWidth, a, p); break;
        case SrcKind::RGB888: RotatedBlitT<SrcKind::RGB888, F>(source, target, targetWidth, a, p); break;
        case SrcKind::ALPHA8: RotatedBlitT<SrcKind::ALPHA8, F>(source, target, targetWidth, a, p); break;
    }
}

void Blit(const Source& source, uint8_t* target, int32_t targetWidth, int32_t targetHeight,
          Deki::ColorFormat targetFormat, int32_t screenX, int32_t screenY, float scaleX, float scaleY, float rotation,
          float pivotX, float pivotY, uint8_t tintR, uint8_t tintG, uint8_t tintB, uint8_t tintA, bool useOrderedDither)
{
    if (!source.pixels || !target || source.width <= 0 || source.height <= 0)
    {
        return;
    }

    if (tintA == 0)
    {
        return;
    }

    float destWidth = source.width * scaleX;
    float destHeight = source.height * scaleY;

    if (destWidth <= 0 || destHeight <= 0)
    {
        return;
    }

    // No rotation: the faster scaled blit.
    if (rotation == 0.0f)
    {
        int32_t destX = screenX - static_cast<int32_t>(std::floor(destWidth * pivotX));
        int32_t destY = screenY - static_cast<int32_t>(std::floor(destHeight * pivotY));
        BlitScaled(source, target, targetWidth, targetHeight, targetFormat, destX, destY,
                   static_cast<int32_t>(destWidth), static_cast<int32_t>(destHeight), tintR, tintG, tintB, tintA,
                   useOrderedDither);
        return;
    }

    // Rotated. `rotation` is in radians, engine convention.
    float cosR = std::cos(rotation);
    float sinR = std::sin(rotation);

    float pivotSX = destWidth * pivotX;
    float pivotSY = destHeight * pivotY;

    float corners[4][2] = { { -pivotSX, -pivotSY },
                            { destWidth - pivotSX, -pivotSY },
                            { -pivotSX, destHeight - pivotSY },
                            { destWidth - pivotSX, destHeight - pivotSY } };

    float minX = 0, maxX = 0, minY = 0, maxY = 0;
    for (int i = 0; i < 4; i++)
    {
        float rx = corners[i][0] * cosR - corners[i][1] * sinR;
        float ry = corners[i][0] * sinR + corners[i][1] * cosR;
        if (i == 0)
        {
            minX = maxX = rx;
            minY = maxY = ry;
        }
        else
        {
            minX = std::min(minX, rx);
            maxX = std::max(maxX, rx);
            minY = std::min(minY, ry);
            maxY = std::max(maxY, ry);
        }
    }

    ClipRect clip = GetCurrentClipRect();
    int32_t startX = std::max<int32_t>(0, std::max(screenX + static_cast<int32_t>(std::floor(minX)), clip.left));
    int32_t startY = std::max<int32_t>(0, std::max(screenY + static_cast<int32_t>(std::floor(minY)), clip.top));
    int32_t endX =
        std::min<int32_t>(targetWidth, std::min(screenX + static_cast<int32_t>(std::floor(maxX + 1)), clip.right));
    int32_t endY =
        std::min<int32_t>(targetHeight, std::min(screenY + static_cast<int32_t>(std::floor(maxY + 1)), clip.bottom));

    if (startX >= endX || startY >= endY)
    {
        return;
    }
    NoteBlitRect(target, startX, startY, endX, endY);

    BlitParams p;
    p.hasTint = (tintR != 255 || tintG != 255 || tintB != 255);
    p.hasAlphaTint = (tintA != 255);
    p.hasKey = source.hasChromaKey;
    p.dither = useOrderedDither && source.hasAlpha;
    p.flips = HasFlips(source);
    p.tintR = tintR;
    p.tintG = tintG;
    p.tintB = tintB;
    p.tintA = tintA;
    p.keyR = source.keyR;
    p.keyG = source.keyG;
    p.keyB = source.keyB;

    // Fixed-point inverse mapping. For destination pixel (px, py):
    //   localX =  dx*cosR + dy*sinR + pivotSX
    //   localY = -dx*sinR + dy*cosR + pivotSY
    //   srcX   = localX * srcW / destWidth,  srcY = localY * srcH / destHeight
    // which is affine in (px, py), so it is evaluated once at the box corner and
    // stepped per column and per row in 16.16.
    const float sxScale = source.width / destWidth;
    const float syScale = source.height / destHeight;
    const float dx0 = static_cast<float>(startX - screenX);
    const float dy0 = static_cast<float>(startY - screenY);
    const float localX0 = dx0 * cosR + dy0 * sinR + pivotSX;
    const float localY0 = -dx0 * sinR + dy0 * cosR + pivotSY;

    RotatedBlitArgs args;
    args.startX = startX;
    args.startY = startY;
    args.endX = endX;
    args.endY = endY;
    args.rowSx = static_cast<int32_t>(std::lround(localX0 * sxScale * 65536.0f));
    args.rowSy = static_cast<int32_t>(std::lround(localY0 * syScale * 65536.0f));
    args.dSxDx = static_cast<int32_t>(std::lround(cosR * sxScale * 65536.0f));
    args.dSyDx = static_cast<int32_t>(std::lround(-sinR * syScale * 65536.0f));
    args.dSxDy = static_cast<int32_t>(std::lround(sinR * sxScale * 65536.0f));
    args.dSyDy = static_cast<int32_t>(std::lround(cosR * syScale * 65536.0f));

    const SrcKind kind = KindOf(source);
    switch (targetFormat)
    {
        case Deki::ColorFormat::RGB565:
            RotatedBlitForTarget<Deki::ColorFormat::RGB565>(kind, source, target, targetWidth, args, p);
            break;
        case Deki::ColorFormat::RGB888:
            RotatedBlitForTarget<Deki::ColorFormat::RGB888>(kind, source, target, targetWidth, args, p);
            break;
        case Deki::ColorFormat::ARGB8888:
            RotatedBlitForTarget<Deki::ColorFormat::ARGB8888>(kind, source, target, targetWidth, args, p);
            break;
        case Deki::ColorFormat::RGB565A8:
            RotatedBlitForTarget<Deki::ColorFormat::RGB565A8>(kind, source, target, targetWidth, args, p);
            break;
    }
}

}  // namespace QuadBlit
