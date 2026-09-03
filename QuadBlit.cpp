#include "QuadBlit.h"
#include "PixelFormat.h"
#include <cmath>
#include <algorithm>
#include <cstring>

// DekiColorFormat comes from the engine header. The editor build used to re-declare it
// locally instead of including this; that is a second definition of the same type, which
// is a redefinition error the moment this file shares a translation unit with one that
// includes DekiEngine.h (unity build) — and its sibling RendererComponent.cpp already
// includes it in editor mode.
#include "DekiEngine.h"
#include "DekiLogSystem.h"

// ============================================================================
// How this file is organised
// ============================================================================
//
// Every blit - scaled or rotated, any of the five source layouts onto any of
// the four target formats - runs one pixel pipeline, CompositePixel<SK, F>:
// read the source pixel, drop it if transparent or chroma-keyed, tint, apply
// the alpha tint, then dither / write opaque / blend with the destination.
// The source layout (SrcKind) and the target format are template parameters,
// so each blit resolves them once; the per-blit flags (tint, alpha tint, key,
// dither, flips) are runtime booleans hoisted out of the loops, with a
// separate "Plain" instantiation (none of them set) that keeps the common
// sprite-onto-framebuffer loop tight.
//
// This replaced twelve hand-written per-format-pair kernels (2300 lines) that
// had drifted from each other: some honoured Source::hasAlpha and some did
// not, the generic paths wrote coverage alpha differently from the
// specialised ones, and a fix in one kernel rarely reached the others.
// tests/GoldenBlitTests.cpp pins the output of every path.
//
// The 1:1 fast paths that matter on the ESP32 are kept: whole-row copies and
// the registered SIMD row kernels, the per-row opaque-span split for
// RGB565A8 sprites and the chroma-key span copy for RGB565 sprites.

using DekiPixel::AlphaUnion;
using DekiPixel::BayerThreshold;
using DekiPixel::Div255;
using DekiPixel::PackRGB565;
using DekiPixel::UnpackRGB565;

namespace QuadBlit
{

// ============================================================================
// Kernel dispatch table
// ============================================================================
// Default-null. Platform packages call RegisterKernel(op, fn) at init to plug in
// SIMD implementations. The blit dispatcher checks for a non-null entry only
// when all preconditions hold (format, no scale, no rotation, alignment, no
// tint where applicable).

static RowKernelFn s_Kernels[(int)KernelOp::Count] = {};

void RegisterKernel(KernelOp op, RowKernelFn fn)
{
    if ((int)op < 0 || (int)op >= (int)KernelOp::Count) return;
    s_Kernels[(int)op] = fn;
}

RowKernelFn GetKernel(KernelOp op)
{
    if ((int)op < 0 || (int)op >= (int)KernelOp::Count) return nullptr;
    return s_Kernels[(int)op];
}

// True when both pointers are 16-byte aligned (PIE / cacheline-friendly).
static inline bool Aligned16(const void* a, const void* b)
{
    return ((uintptr_t)a & 0xF) == 0 && ((uintptr_t)b & 0xF) == 0;
}

// ============================================================================
// Clip Rect Stack Implementation
// ============================================================================

static constexpr int MAX_CLIP_STACK = 16;
static ClipRect s_ClipStack[MAX_CLIP_STACK];
static int s_ClipStackDepth = 0;
static bool s_ClipEnabled = true;
// Pushes refused for lack of a slot. Their matching pops are absorbed so the
// stack stays balanced: the overflowed levels draw with the deepest rect that
// did fit (over-clipped), instead of a pop discarding a live rect and every
// later draw going unclipped with no diagnostic.
static int s_ClipOverflow = 0;

void PushClipRect(int32_t left, int32_t top, int32_t right, int32_t bottom)
{
    if (s_ClipStackDepth >= MAX_CLIP_STACK)
    {
        if (s_ClipOverflow == 0)
            DEKI_LOG_WARNING("QuadBlit: clip stack overflow (depth %d); nested clips beyond this use the parent rect",
                             MAX_CLIP_STACK);
        s_ClipOverflow++;
        return;
    }

    ClipRect rect = { left, top, right, bottom };

    // Intersect with parent clip rect
    if (s_ClipStackDepth > 0)
    {
        const ClipRect& parent = s_ClipStack[s_ClipStackDepth - 1];
        rect.left = std::max(rect.left, parent.left);
        rect.top = std::max(rect.top, parent.top);
        rect.right = std::min(rect.right, parent.right);
        rect.bottom = std::min(rect.bottom, parent.bottom);
    }

    s_ClipStack[s_ClipStackDepth++] = rect;
}

void PopClipRect()
{
    if (s_ClipOverflow > 0)
    {
        s_ClipOverflow--;
        return;
    }
    if (s_ClipStackDepth > 0)
        s_ClipStackDepth--;
}

ClipRect GetCurrentClipRect()
{
    if (!s_ClipEnabled)
        return ClipRect{};

    if (s_ClipStackDepth > 0)
        return s_ClipStack[s_ClipStackDepth - 1];
    return ClipRect{};
}

void ClearClipStack()
{
    s_ClipStackDepth = 0;
    s_ClipOverflow = 0;
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
    return s_ClipStackDepth;
}

// ============================================================================
// Source Creation
// ============================================================================

Source MakeSource(const uint8_t* pixels, int32_t width, int32_t height,
                  int32_t bytesPerPixel, bool hasAlpha, bool isRGB565,
                  bool ownsPixels, const int16_t* alphaRowSpans)
{
    Source src;
    src.pixels = pixels;
    src.width = width;
    src.height = height;
    src.bytesPerPixel = bytesPerPixel;
    src.hasAlpha = hasAlpha;
    src.isRGB565 = isRGB565;
    src.ownsPixels = ownsPixels;
    src.alphaRowSpans = alphaRowSpans;
    src.stride = 0;
    src.hasChromaKey = false;
    src.keyR = 0;
    src.keyG = 0;
    src.keyB = 0;
    src.chromaRowSpans = nullptr;

    if (hasAlpha)
        src.alphaOffset = isRGB565 ? 2 : 3;
    else
        src.alphaOffset = 0;

    return src;
}

// Effective bytes-per-row of a Source buffer. Source::stride == 0 means the
// buffer is tightly packed (one row immediately follows the previous);
// non-zero stride lets a Source point at a sub-rect of a larger buffer
// (e.g. a tile inside an atlas) without a copy.
static inline int32_t SourceStride(const Source& s)
{
    return s.stride ? s.stride : s.width * s.bytesPerPixel;
}

// Map a sampled source coordinate through the Source's flip flags. Inverse
// of Tiled's order (transpose, then H, then V), so applied V, H, then D.
static inline void ApplyFlips(const Source& s, int32_t& x, int32_t& y)
{
    if (s.flipV) y = s.height - 1 - y;
    if (s.flipH) x = s.width - 1 - x;
    if (s.flipD)
    {
        // A transpose only makes sense for a square source; Tiled only sets
        // it on tiles, which are square. Anything else keeps its orientation.
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
// Clipping bounds helper (shared by BlitScaled and Blit)
// ============================================================================

struct BlitBounds
{
    int32_t startX, startY, endX, endY;
};

static inline bool ComputeClipBounds(int32_t destX, int32_t destY,
                                      int32_t destWidth, int32_t destHeight,
                                      int32_t targetWidth, int32_t targetHeight,
                                      BlitBounds& out)
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

// Source layouts. RGB565A8 is any isRGB565 source with 3+ bytes per pixel,
// whether or not it declares alpha (Source::hasAlpha decides whether byte 2
// is read); RGBA8888 is the 4-byte non-565 layout, RGB888 3 bytes, ALPHA8 a
// coverage-only byte (a font/icon atlas drawn as a sprite: its colour is the
// tint, white when untinted).
enum class SrcKind { RGB565, RGB565A8, RGBA8888, RGB888, ALPHA8 };

static inline SrcKind KindOf(const Source& s)
{
    if (s.isRGB565) return s.bytesPerPixel >= 3 ? SrcKind::RGB565A8 : SrcKind::RGB565;
    if (s.bytesPerPixel == 4) return SrcKind::RGBA8888;
    if (s.bytesPerPixel == 3) return SrcKind::RGB888;
    return SrcKind::ALPHA8;
}

template <SrcKind SK>
static inline void ReadSrcPixel(const uint8_t* p, bool hasAlpha, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a)
{
    if constexpr (SK == SrcKind::RGB565 || SK == SrcKind::RGB565A8)
    {
        uint16_t v;
        memcpy(&v, p, 2);
        UnpackRGB565(v, r, g, b);
        if constexpr (SK == SrcKind::RGB565A8)
            a = hasAlpha ? p[2] : 255;
        else
            a = 255;
    }
    else if constexpr (SK == SrcKind::RGBA8888)
    {
        r = p[0]; g = p[1]; b = p[2]; a = p[3];
        (void)hasAlpha;
    }
    else if constexpr (SK == SrcKind::RGB888)
    {
        r = p[0]; g = p[1]; b = p[2]; a = 255;
        (void)hasAlpha;
    }
    else
    {
        r = g = b = 255;
        a = p[0];
        (void)hasAlpha;
    }
}

// Destination read: colour plus coverage alpha (255 for formats without one).
template <DekiColorFormat F>
static inline void ReadDstPixel(const uint8_t* target, size_t idx, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a)
{
    if constexpr (F == DekiColorFormat::RGB565)
    {
        UnpackRGB565(((const uint16_t*)target)[idx], r, g, b);
        a = 255;
    }
    else if constexpr (F == DekiColorFormat::RGB888)
    {
        r = target[idx * 3]; g = target[idx * 3 + 1]; b = target[idx * 3 + 2];
        a = 255;
    }
    else if constexpr (F == DekiColorFormat::ARGB8888)
    {
        const uint32_t v = ((const uint32_t*)target)[idx];
        r = (v >> 16) & 0xFF; g = (v >> 8) & 0xFF; b = v & 0xFF;
        a = 255;
    }
    else  // RGB565A8: [lo, hi, alpha]
    {
        const uint16_t v = (uint16_t)target[idx * 3] | ((uint16_t)target[idx * 3 + 1] << 8);
        UnpackRGB565(v, r, g, b);
        a = target[idx * 3 + 2];
    }
}

// Destination write with the coverage alpha the format keeps (ignored by the
// formats without one).
template <DekiColorFormat F>
static inline void WriteDstPixel(uint8_t* target, size_t idx, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if constexpr (F == DekiColorFormat::RGB565)
    {
        ((uint16_t*)target)[idx] = PackRGB565(r, g, b);
        (void)a;
    }
    else if constexpr (F == DekiColorFormat::RGB888)
    {
        target[idx * 3] = r; target[idx * 3 + 1] = g; target[idx * 3 + 2] = b;
        (void)a;
    }
    else if constexpr (F == DekiColorFormat::ARGB8888)
    {
        ((uint32_t*)target)[idx] = (0xFFu << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        (void)a;
    }
    else
    {
        const uint16_t v = PackRGB565(r, g, b);
        target[idx * 3] = (uint8_t)(v & 0xFF);
        target[idx * 3 + 1] = (uint8_t)(v >> 8);
        target[idx * 3 + 2] = a;
    }
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

// One source pixel at `sp` onto destination pixel `idx` (at px, py for the
// dither threshold). Plain = no tint, no alpha tint, no key, no dither: the
// tight loop for the common sprite blit.
template <SrcKind SK, DekiColorFormat F, bool Plain>
static inline void CompositePixel(const Source& source, const uint8_t* sp, uint8_t* target, size_t idx,
                                  int32_t px, int32_t py, const BlitParams& P)
{
    uint8_t r, g, b, a;
    ReadSrcPixel<SK>(sp, source.hasAlpha, r, g, b, a);
    if (a == 0)
        return;

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
        WriteDstPixel<F>(target, idx,
                         Div255(r * a + bgR * invA),
                         Div255(g * a + bgG * invA),
                         Div255(b * a + bgB * invA),
                         AlphaUnion(a, bgA));
        return;
    }
    else
    {
        // The key is compared against the untinted colour.
        if (P.hasKey && r == P.keyR && g == P.keyG && b == P.keyB)
            return;

        if (P.hasTint)
        {
            r = Div255(r * P.tintR);
            g = Div255(g * P.tintG);
            b = Div255(b * P.tintB);
        }

        const uint8_t effA = P.hasAlphaTint ? Div255(a * P.tintA) : a;
        if (effA == 0)
            return;

        if (P.dither)
        {
            // Threshold compare: 255 always passes (the matrix tops out at 252).
            if (effA <= BayerThreshold(px, py))
                return;
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
        WriteDstPixel<F>(target, idx,
                         Div255(r * effA + bgR * invA),
                         Div255(g * effA + bgG * invA),
                         Div255(b * effA + bgB * invA),
                         AlphaUnion(effA, bgA));
    }
}

// ============================================================================
// 1:1 row fast paths (opaque copies, span splits, SIMD hooks)
// ============================================================================
// Each returns true when it handled the whole blit. They exist for speed only:
// the pipeline above produces the same pixels.

// RGB565 -> RGB565, no tint/key: row copy (SIMD kernel when aligned).
static DEKI_FAST_ATTR bool CopyRows_RGB565(const Source& source, uint16_t* target16, int32_t targetWidth,
                                           int32_t destX, int32_t destY, const BlitBounds& b)
{
    const int32_t stride = SourceStride(source);
    const int32_t rowPixels = b.endX - b.startX;
    RowKernelFn copyKernel = s_Kernels[(int)KernelOp::RGB565_Copy_Row];
    for (int32_t py = b.startY; py < b.endY; py++)
    {
        const uint16_t* srcPtr = (const uint16_t*)(source.pixels + (py - destY) * stride) + (b.startX - destX);
        uint16_t* dstPtr = target16 + py * targetWidth + b.startX;
        if (copyKernel && Aligned16(srcPtr, dstPtr))
            copyKernel((const uint8_t*)srcPtr, (uint8_t*)dstPtr, rowPixels, 255, 255, 255, 255);
        else
            memcpy(dstPtr, srcPtr, rowPixels * sizeof(uint16_t));
    }
    return true;
}

// RGB565 -> RGB565 with a chroma key and per-row non-key spans, no tint:
// inside [start, end) every pixel is non-key (straight copy), outside every
// pixel is the key (skipped without a read).
static DEKI_FAST_ATTR bool CopyRows_RGB565_ChromaSpans(const Source& source, uint16_t* target16, int32_t targetWidth,
                                                       int32_t destX, int32_t destY, const BlitBounds& b)
{
    const int32_t stride = SourceStride(source);
    const int16_t* spans = source.chromaRowSpans;
    RowKernelFn copyKernel = s_Kernels[(int)KernelOp::RGB565_Copy_Row];
    for (int32_t py = b.startY; py < b.endY; py++)
    {
        const int32_t srcY = py - destY;
        const int32_t srcStartX = b.startX - destX;
        const int32_t srcEndX = b.endX - destX;
        const int32_t clampedStart = std::max<int32_t>(spans[srcY * 2], srcStartX);
        const int32_t clampedEnd = std::min<int32_t>(spans[srcY * 2 + 1], srcEndX);
        if (clampedStart >= clampedEnd)
            continue;
        const uint16_t* srcPtr = (const uint16_t*)(source.pixels + srcY * stride) + clampedStart;
        uint16_t* dstPtr = target16 + py * targetWidth + (destX + clampedStart);
        const int32_t rowPixels = clampedEnd - clampedStart;
        if (copyKernel && Aligned16(srcPtr, dstPtr))
            copyKernel((const uint8_t*)srcPtr, (uint8_t*)dstPtr, rowPixels, 255, 255, 255, 255);
        else
            memcpy(dstPtr, srcPtr, rowPixels * sizeof(uint16_t));
    }
    return true;
}

// RGB565A8 (with alpha) -> RGB565, no tint/key: per-row opaque-span split
// (left blend | opaque copy | right blend) when spans are available,
// otherwise the SIMD blend kernel when aligned. Rows the kernel or the spans
// cannot take go through the plain pipeline.
static DEKI_FAST_ATTR bool BlendRows_RGB565A8_to_RGB565(const Source& source, uint16_t* target16, int32_t targetWidth,
                                                        int32_t destX, int32_t destY, const BlitBounds& b)
{
    const int32_t stride = SourceStride(source);
    const int32_t bpp = source.bytesPerPixel;
    const int16_t* rowSpans = source.alphaRowSpans;
    RowKernelFn blendKernel = s_Kernels[(int)KernelOp::RGB565A8_Blend_Row];
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

            // Left alpha region
            for (int32_t sx = srcStartX; sx < clampedStart && sx < srcEndX; sx++)
                CompositePixel<SrcKind::RGB565A8, DekiColorFormat::RGB565, true>(
                    source, rowBase + sx * bpp, (uint8_t*)target16, rowIdx + destX + sx, destX + sx, py, plain);

            // Opaque middle: direct copy, no alpha checks
            const uint8_t* srcPtr = rowBase + clampedStart * bpp;
            for (int32_t sx = clampedStart; sx < clampedEnd; sx++, srcPtr += bpp)
                memcpy(&dstRow[destX + sx], srcPtr, 2);

            // Right alpha region. Starts at the clip start when the opaque span
            // ends before it (or is empty): starting at opaqueEnd wrote pixels
            // the clip rect had excluded.
            for (int32_t sx = std::max(clampedEnd, srcStartX); sx < srcEndX; sx++)
                CompositePixel<SrcKind::RGB565A8, DekiColorFormat::RGB565, true>(
                    source, rowBase + sx * bpp, (uint8_t*)target16, rowIdx + destX + sx, destX + sx, py, plain);
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
            CompositePixel<SrcKind::RGB565A8, DekiColorFormat::RGB565, true>(
                source, srcPtr, (uint8_t*)target16, rowIdx + px, px, py, plain);
    }
    return true;
}

// RGB565 -> RGB565A8, no tint/key: opaque expand (SIMD kernel when aligned).
static DEKI_FAST_ATTR bool ExpandRows_RGB565_to_RGB565A8(const Source& source, uint8_t* target, int32_t targetWidth,
                                                         int32_t destX, int32_t destY, const BlitBounds& b)
{
    const int32_t stride = SourceStride(source);
    const int32_t rowPixels = b.endX - b.startX;
    RowKernelFn expandKernel = s_Kernels[(int)KernelOp::RGB565_to_RGB565A8_Row];
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

// RGB565A8 -> RGB565A8, no tint/key: opaque copy when the source declares
// no alpha, otherwise the SIMD blend kernel when aligned.
static DEKI_FAST_ATTR bool Rows_RGB565A8_to_RGB565A8(const Source& source, uint8_t* target, int32_t targetWidth,
                                                     int32_t destX, int32_t destY, const BlitBounds& b)
{
    const int32_t stride = SourceStride(source);
    const int32_t bpp = source.bytesPerPixel;
    if (bpp != 3)
        return false;
    const int32_t rowPixels = b.endX - b.startX;
    const BlitParams plain;
    for (int32_t py = b.startY; py < b.endY; py++)
    {
        const uint8_t* srcPtr = source.pixels + (py - destY) * stride + (b.startX - destX) * bpp;
        uint8_t* dstPtr = target + (py * targetWidth + b.startX) * 3;
        if (!source.hasAlpha)
        {
            RowKernelFn copyKernel = s_Kernels[(int)KernelOp::RGB565A8_Copy_Row];
            if (copyKernel && Aligned16(srcPtr, dstPtr))
            {
                copyKernel(srcPtr, dstPtr, rowPixels, 255, 255, 255, 255);
                continue;
            }
            // Byte 2 of the source is whatever; the pixel is opaque by declaration.
            for (int32_t i = 0; i < rowPixels; i++)
            {
                dstPtr[i * 3] = srcPtr[i * 3];
                dstPtr[i * 3 + 1] = srcPtr[i * 3 + 1];
                dstPtr[i * 3 + 2] = 0xFF;
            }
            continue;
        }
        RowKernelFn blendKernel = s_Kernels[(int)KernelOp::RGB565A8_Blend_Row_Dest_RGB565A8];
        if (blendKernel && Aligned16(srcPtr, dstPtr))
        {
            blendKernel(srcPtr, dstPtr, rowPixels, 255, 255, 255, 255);
            continue;
        }
        const size_t rowIdx = (size_t)py * (size_t)targetWidth;
        for (int32_t px = b.startX; px < b.endX; px++, srcPtr += 3)
            CompositePixel<SrcKind::RGB565A8, DekiColorFormat::RGB565A8, true>(source, srcPtr, target, rowIdx + px, px, py, plain);
    }
    return true;
}

// ============================================================================
// Scaled blit: 16.16 fixed-point stepping (1:1 is the step 65536 case)
// ============================================================================

template <SrcKind SK, DekiColorFormat F, bool Plain>
static DEKI_FAST_ATTR void BlitRows(const Source& source, uint8_t* target, int32_t targetWidth,
                                    int32_t destX, int32_t destY, int32_t destWidth, int32_t destHeight,
                                    const BlitBounds& b, const BlitParams& P)
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
                    // Per-pixel copies: a transpose must not rewrite the row's Y.
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

template <DekiColorFormat F, bool Plain>
static void BlitRowsForTarget(SrcKind kind, const Source& source, uint8_t* target, int32_t targetWidth,
                              int32_t destX, int32_t destY, int32_t destWidth, int32_t destHeight,
                              const BlitBounds& b, const BlitParams& P)
{
    switch (kind)
    {
        case SrcKind::RGB565:   BlitRows<SrcKind::RGB565, F, Plain>(source, target, targetWidth, destX, destY, destWidth, destHeight, b, P); break;
        case SrcKind::RGB565A8: BlitRows<SrcKind::RGB565A8, F, Plain>(source, target, targetWidth, destX, destY, destWidth, destHeight, b, P); break;
        case SrcKind::RGBA8888: BlitRows<SrcKind::RGBA8888, F, Plain>(source, target, targetWidth, destX, destY, destWidth, destHeight, b, P); break;
        case SrcKind::RGB888:   BlitRows<SrcKind::RGB888, F, Plain>(source, target, targetWidth, destX, destY, destWidth, destHeight, b, P); break;
        case SrcKind::ALPHA8:   BlitRows<SrcKind::ALPHA8, F, Plain>(source, target, targetWidth, destX, destY, destWidth, destHeight, b, P); break;
    }
}

template <bool Plain>
static void BlitRowsDispatch(SrcKind kind, DekiColorFormat targetFormat, const Source& source, uint8_t* target,
                             int32_t targetWidth, int32_t destX, int32_t destY, int32_t destWidth, int32_t destHeight,
                             const BlitBounds& b, const BlitParams& P)
{
    switch (targetFormat)
    {
        case DekiColorFormat::RGB565:   BlitRowsForTarget<DekiColorFormat::RGB565, Plain>(kind, source, target, targetWidth, destX, destY, destWidth, destHeight, b, P); break;
        case DekiColorFormat::RGB888:   BlitRowsForTarget<DekiColorFormat::RGB888, Plain>(kind, source, target, targetWidth, destX, destY, destWidth, destHeight, b, P); break;
        case DekiColorFormat::ARGB8888: BlitRowsForTarget<DekiColorFormat::ARGB8888, Plain>(kind, source, target, targetWidth, destX, destY, destWidth, destHeight, b, P); break;
        case DekiColorFormat::RGB565A8: BlitRowsForTarget<DekiColorFormat::RGB565A8, Plain>(kind, source, target, targetWidth, destX, destY, destWidth, destHeight, b, P); break;
    }
}

void BlitScaled(const Source& source,
                uint8_t* target,
                int32_t targetWidth,
                int32_t targetHeight,
                DekiColorFormat targetFormat,
                int32_t destX,
                int32_t destY,
                int32_t destWidth,
                int32_t destHeight,
                uint8_t tintR,
                uint8_t tintG,
                uint8_t tintB,
                uint8_t tintA,
                bool useOrderedDither)
{
    if (!source.pixels || !target || source.width <= 0 || source.height <= 0)
        return;
    if (tintA == 0)
        return;
    if (destWidth <= 0 || destHeight <= 0)
        return;

    BlitBounds bounds;
    if (!ComputeClipBounds(destX, destY, destWidth, destHeight, targetWidth, targetHeight, bounds))
        return;

    BlitParams P;
    P.hasTint = (tintR != 255 || tintG != 255 || tintB != 255);
    P.hasAlphaTint = (tintA != 255);
    P.hasKey = source.hasChromaKey;
    // Dithering only has partial-alpha pixels to work on when the source has alpha.
    P.dither = useOrderedDither && source.hasAlpha;
    P.flips = HasFlips(source);
    P.tintR = tintR; P.tintG = tintG; P.tintB = tintB; P.tintA = tintA;
    P.keyR = source.keyR; P.keyG = source.keyG; P.keyB = source.keyB;

    const SrcKind kind = KindOf(source);
    const bool oneToOne = (destWidth == source.width && destHeight == source.height);
    const bool plain = !P.hasTint && !P.hasAlphaTint && !P.hasKey && !P.dither && !P.flips;

    // 1:1 row fast paths (same pixels as the pipeline, fewer instructions).
    if (oneToOne && !P.hasTint && !P.hasAlphaTint && !P.dither && !P.flips)
    {
        if (targetFormat == DekiColorFormat::RGB565)
        {
            uint16_t* target16 = (uint16_t*)target;
            if (kind == SrcKind::RGB565 && P.hasKey && source.chromaRowSpans)
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
        else if (targetFormat == DekiColorFormat::RGB565A8 && plain)
        {
            if (kind == SrcKind::RGB565)
            {
                ExpandRows_RGB565_to_RGB565A8(source, target, targetWidth, destX, destY, bounds);
                return;
            }
            if (kind == SrcKind::RGB565A8 && Rows_RGB565A8_to_RGB565A8(source, target, targetWidth, destX, destY, bounds))
                return;
        }
    }

    if (plain)
        BlitRowsDispatch<true>(kind, targetFormat, source, target, targetWidth, destX, destY, destWidth, destHeight, bounds, P);
    else
        BlitRowsDispatch<false>(kind, targetFormat, source, target, targetWidth, destX, destY, destWidth, destHeight, bounds, P);
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

template <SrcKind SK, DekiColorFormat F>
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
                continue;
            if (P.flips) ApplyFlips(source, ix, iy);
            CompositePixel<SK, F, false>(source, source.pixels + iy * stride + ix * bpp, target, rowIdx + px, px, py, P);
        }
    }
}

template <DekiColorFormat F>
static void RotatedBlitForTarget(SrcKind kind, const Source& source, uint8_t* target, int32_t targetWidth,
                                 const RotatedBlitArgs& a, const BlitParams& P)
{
    switch (kind)
    {
        case SrcKind::RGB565:   RotatedBlitT<SrcKind::RGB565, F>(source, target, targetWidth, a, P); break;
        case SrcKind::RGB565A8: RotatedBlitT<SrcKind::RGB565A8, F>(source, target, targetWidth, a, P); break;
        case SrcKind::RGBA8888: RotatedBlitT<SrcKind::RGBA8888, F>(source, target, targetWidth, a, P); break;
        case SrcKind::RGB888:   RotatedBlitT<SrcKind::RGB888, F>(source, target, targetWidth, a, P); break;
        case SrcKind::ALPHA8:   RotatedBlitT<SrcKind::ALPHA8, F>(source, target, targetWidth, a, P); break;
    }
}

void Blit(const Source& source,
          uint8_t* target,
          int32_t targetWidth,
          int32_t targetHeight,
          DekiColorFormat targetFormat,
          int32_t screenX,
          int32_t screenY,
          float scaleX,
          float scaleY,
          float rotation,
          float pivotX,
          float pivotY,
          uint8_t tintR,
          uint8_t tintG,
          uint8_t tintB,
          uint8_t tintA,
          bool useOrderedDither)
{
    if (!source.pixels || !target || source.width <= 0 || source.height <= 0)
        return;

    if (tintA == 0)
        return;

    float destWidth = source.width * scaleX;
    float destHeight = source.height * scaleY;

    if (destWidth <= 0 || destHeight <= 0)
        return;

    // Fast path: no rotation — the scaled blit
    if (rotation == 0.0f)
    {
        int32_t destX = screenX - static_cast<int32_t>(std::floor(destWidth * pivotX));
        int32_t destY = screenY - static_cast<int32_t>(std::floor(destHeight * pivotY));
        BlitScaled(source, target, targetWidth, targetHeight, targetFormat,
                   destX, destY, static_cast<int32_t>(destWidth), static_cast<int32_t>(destHeight),
                   tintR, tintG, tintB, tintA, useOrderedDither);
        return;
    }

    // Rotated path. `rotation` is in radians (engine convention).
    float cosR = std::cos(rotation);
    float sinR = std::sin(rotation);

    float pivotSX = destWidth * pivotX;
    float pivotSY = destHeight * pivotY;

    float corners[4][2] = {
        { -pivotSX, -pivotSY },
        { destWidth - pivotSX, -pivotSY },
        { -pivotSX, destHeight - pivotSY },
        { destWidth - pivotSX, destHeight - pivotSY }
    };

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
    int32_t endX = std::min<int32_t>(targetWidth, std::min(screenX + static_cast<int32_t>(std::floor(maxX + 1)), clip.right));
    int32_t endY = std::min<int32_t>(targetHeight, std::min(screenY + static_cast<int32_t>(std::floor(maxY + 1)), clip.bottom));

    if (startX >= endX || startY >= endY)
        return;

    BlitParams P;
    P.hasTint = (tintR != 255 || tintG != 255 || tintB != 255);
    P.hasAlphaTint = (tintA != 255);
    P.hasKey = source.hasChromaKey;
    P.dither = useOrderedDither && source.hasAlpha;
    P.flips = HasFlips(source);
    P.tintR = tintR; P.tintG = tintG; P.tintB = tintB; P.tintA = tintA;
    P.keyR = source.keyR; P.keyG = source.keyG; P.keyB = source.keyB;

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
    args.startX = startX; args.startY = startY; args.endX = endX; args.endY = endY;
    args.rowSx = static_cast<int32_t>(std::lround(localX0 * sxScale * 65536.0f));
    args.rowSy = static_cast<int32_t>(std::lround(localY0 * syScale * 65536.0f));
    args.dSxDx = static_cast<int32_t>(std::lround(cosR * sxScale * 65536.0f));
    args.dSyDx = static_cast<int32_t>(std::lround(-sinR * syScale * 65536.0f));
    args.dSxDy = static_cast<int32_t>(std::lround(sinR * sxScale * 65536.0f));
    args.dSyDy = static_cast<int32_t>(std::lround(cosR * syScale * 65536.0f));

    const SrcKind kind = KindOf(source);
    switch (targetFormat)
    {
        case DekiColorFormat::RGB565:   RotatedBlitForTarget<DekiColorFormat::RGB565>(kind, source, target, targetWidth, args, P); break;
        case DekiColorFormat::RGB888:   RotatedBlitForTarget<DekiColorFormat::RGB888>(kind, source, target, targetWidth, args, P); break;
        case DekiColorFormat::ARGB8888: RotatedBlitForTarget<DekiColorFormat::ARGB8888>(kind, source, target, targetWidth, args, P); break;
        case DekiColorFormat::RGB565A8: RotatedBlitForTarget<DekiColorFormat::RGB565A8>(kind, source, target, targetWidth, args, P); break;
    }
}

} // namespace QuadBlit
