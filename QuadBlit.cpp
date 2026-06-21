#include "QuadBlit.h"
#include <cmath>
#include <algorithm>
#include <cstring>

#ifndef DEKI_EDITOR
#include "DekiEngine.h"
#else
// Editor mode: define DekiColorFormat locally
enum class DekiColorFormat
{
    RGB565 = 0,
    RGB888 = 1,
    ARGB8888 = 2,
    RGB565A8 = 3
};
#endif

// Fast alpha blend approximation: (a * b + 128) >> 8 instead of (a * b) / 255
#define FAST_DIV255(x) (((x) + 128) >> 8)

namespace QuadBlit
{

// ============================================================================
// Kernel dispatch table
// ============================================================================
// Default-null. Platform modules call RegisterKernel(op, fn) at init to plug in
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

static constexpr int MAX_CLIP_STACK = 8;
static ClipRect s_ClipStack[MAX_CLIP_STACK];
static int s_ClipStackDepth = 0;
static bool s_ClipEnabled = true;

void PushClipRect(int32_t left, int32_t top, int32_t right, int32_t bottom)
{
    if (s_ClipStackDepth >= MAX_CLIP_STACK)
        return;

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
// Specialized BlitScaled: RGBA8888 source → RGB565 target
// ============================================================================

static DEKI_FAST_ATTR void BlitScaled_RGBA8888_to_RGB565(
    const Source& source, uint16_t* target16, int32_t targetWidth,
    int32_t destX, int32_t destY, int32_t destWidth, int32_t destHeight,
    const BlitBounds& bounds, bool hasTint, bool hasAlphaTint,
    uint8_t tintR, uint8_t tintG, uint8_t tintB, uint8_t tintA)
{

    const uint8_t* srcPixels = source.pixels;
    int32_t srcW = source.width;
    int32_t srcH = source.height;
    const int32_t srcStride = SourceStride(source);
    const bool    hasKey = source.hasChromaKey;
    const uint8_t keyR = source.keyR, keyG = source.keyG, keyB = source.keyB;

    // ── 1:1 scale fast path — sequential pointer access ──
    if (destWidth == srcW && destHeight == srcH)
    {
        for (int32_t py = bounds.startY; py < bounds.endY; py++)
        {
            const uint8_t* srcPtr = srcPixels + (py - destY) * srcStride + (bounds.startX - destX) * 4;
            uint16_t* dstRow = target16 + py * targetWidth;
            for (int32_t px = bounds.startX; px < bounds.endX; px++)
            {
                uint8_t a = srcPtr[3];
                if (a == 0) { srcPtr += 4; continue; }

                uint8_t r = srcPtr[0];
                uint8_t g = srcPtr[1];
                uint8_t b = srcPtr[2];
                srcPtr += 4;

                if (hasKey && r == keyR && g == keyG && b == keyB) continue;

                if (hasTint) { r = FAST_DIV255(r * tintR); g = FAST_DIV255(g * tintG); b = FAST_DIV255(b * tintB); }

                uint8_t effectiveAlpha = hasAlphaTint ? FAST_DIV255(a * tintA) : a;
                if (effectiveAlpha == 0) continue;

                if (effectiveAlpha == 255)
                {
                    dstRow[px] = ((r >> 3 << 11) | ((g >> 2) << 5) | (b >> 3));
                }
                else
                {
                    uint16_t bg = dstRow[px];
                    uint8_t bgR = (bg >> 11) << 3;
                    uint8_t bgG = ((bg >> 5) & 0x3F) << 2;
                    uint8_t bgB = (bg & 0x1F) << 3;
                    uint8_t invA = 255 - effectiveAlpha;
                    r = FAST_DIV255(r * effectiveAlpha + bgR * invA);
                    g = FAST_DIV255(g * effectiveAlpha + bgG * invA);
                    b = FAST_DIV255(b * effectiveAlpha + bgB * invA);
                    dstRow[px] = ((r >> 3 << 11) | ((g >> 2) << 5) | (b >> 3));
                }
            }
        }
        return;
    }

    // ── Scaled path — fixed-point 16.16 with precomputed row pointers ──
    const uint32_t xStep = ((uint32_t)srcW << 16) / (uint32_t)destWidth;
    const uint32_t yStep = ((uint32_t)srcH << 16) / (uint32_t)destHeight;

    for (int32_t py = bounds.startY; py < bounds.endY; py++)
    {
        int32_t srcY = ((uint32_t)(py - destY) * yStep) >> 16;
        const uint8_t* srcRow = srcPixels + srcY * srcStride;
        uint16_t* dstRow = target16 + py * targetWidth;

        uint32_t srcX_acc = (uint32_t)(bounds.startX - destX) * xStep;
        for (int32_t px = bounds.startX; px < bounds.endX; px++)
        {
            int32_t srcX = srcX_acc >> 16;
            srcX_acc += xStep;
            const uint8_t* pixel = srcRow + srcX * 4;

            uint8_t a = pixel[3];
            if (a == 0) continue;

            uint8_t r = pixel[0];
            uint8_t g = pixel[1];
            uint8_t b = pixel[2];

            if (hasKey && r == keyR && g == keyG && b == keyB) continue;

            if (hasTint)
            {
                r = FAST_DIV255(r * tintR);
                g = FAST_DIV255(g * tintG);
                b = FAST_DIV255(b * tintB);
            }

            uint8_t effectiveAlpha = hasAlphaTint ? FAST_DIV255(a * tintA) : a;
            if (effectiveAlpha == 0) continue;

            if (effectiveAlpha == 255)
            {
                dstRow[px] = ((r >> 3 << 11) | ((g >> 2) << 5) | (b >> 3));
            }
            else
            {
                uint16_t bg = dstRow[px];
                uint8_t bgR = (bg >> 11) << 3;
                uint8_t bgG = ((bg >> 5) & 0x3F) << 2;
                uint8_t bgB = (bg & 0x1F) << 3;

                uint8_t invA = 255 - effectiveAlpha;
                r = FAST_DIV255(r * effectiveAlpha + bgR * invA);
                g = FAST_DIV255(g * effectiveAlpha + bgG * invA);
                b = FAST_DIV255(b * effectiveAlpha + bgB * invA);
                dstRow[px] = ((r >> 3 << 11) | ((g >> 2) << 5) | (b >> 3));
            }
        }
    }
}

// ============================================================================
// Specialized BlitScaled: RGB565A8 source → RGB565 target
// ============================================================================

static DEKI_FAST_ATTR void BlitScaled_RGB565A8_to_RGB565(
    const Source& source, uint16_t* target16, int32_t targetWidth,
    int32_t destX, int32_t destY, int32_t destWidth, int32_t destHeight,
    const BlitBounds& bounds, bool hasTint, bool hasAlphaTint,
    uint8_t tintR, uint8_t tintG, uint8_t tintB, uint8_t tintA)
{

    const uint8_t* srcPixels = source.pixels;
    int32_t srcW = source.width;
    int32_t srcH = source.height;
    int32_t bpp = source.bytesPerPixel;
    const int32_t srcStride = SourceStride(source);
    const bool    hasKey = source.hasChromaKey;
    const uint8_t keyR = source.keyR, keyG = source.keyG, keyB = source.keyB;

    // ── 1:1 scale fast path — sequential pointer access, best PSRAM cache use ──
    if (destWidth == srcW && destHeight == srcH)
    {
        if (!source.hasAlpha)
        {
            if (!hasTint && !hasAlphaTint && !hasKey)
            {
                for (int32_t py = bounds.startY; py < bounds.endY; py++)
                {
                    const uint8_t* srcPtr = srcPixels + (py - destY) * srcStride + (bounds.startX - destX) * bpp;
                    uint16_t* dstRow = target16 + py * targetWidth;
                    for (int32_t px = bounds.startX; px < bounds.endX; px++)
                    {
                        dstRow[px] = *(const uint16_t*)srcPtr;
                        srcPtr += bpp;
                    }
                }
            }
            else
            {
                uint8_t effectiveA = hasAlphaTint ? tintA : 255;
                for (int32_t py = bounds.startY; py < bounds.endY; py++)
                {
                    const uint8_t* srcPtr = srcPixels + (py - destY) * srcStride + (bounds.startX - destX) * bpp;
                    uint16_t* dstRow = target16 + py * targetWidth;
                    for (int32_t px = bounds.startX; px < bounds.endX; px++)
                    {
                        uint16_t srcRGB = *(const uint16_t*)srcPtr;
                        srcPtr += bpp;
                        uint8_t r = (srcRGB >> 11) << 3;
                        uint8_t g = ((srcRGB >> 5) & 0x3F) << 2;
                        uint8_t b = (srcRGB & 0x1F) << 3;
                        if (hasKey && r == keyR && g == keyG && b == keyB) continue;
                        if (hasTint) { r = FAST_DIV255(r * tintR); g = FAST_DIV255(g * tintG); b = FAST_DIV255(b * tintB); }
                        if (effectiveA == 255)
                        {
                            dstRow[px] = ((r >> 3 << 11) | ((g >> 2) << 5) | (b >> 3));
                        }
                        else
                        {
                            uint16_t bg = dstRow[px];
                            uint8_t bgR = (bg >> 11) << 3;
                            uint8_t bgG = ((bg >> 5) & 0x3F) << 2;
                            uint8_t bgB = (bg & 0x1F) << 3;
                            uint8_t invA = 255 - effectiveA;
                            r = FAST_DIV255(r * effectiveA + bgR * invA);
                            g = FAST_DIV255(g * effectiveA + bgG * invA);
                            b = FAST_DIV255(b * effectiveA + bgB * invA);
                            dstRow[px] = ((r >> 3 << 11) | ((g >> 2) << 5) | (b >> 3));
                        }
                    }
                }
            }
        }
        else
        {
            // Alpha path at 1:1 — with optional per-row opaque span optimization
            const int16_t* rowSpans = source.alphaRowSpans;

            for (int32_t py = bounds.startY; py < bounds.endY; py++)
            {
                int32_t srcY = py - destY;
                const uint8_t* rowBase = srcPixels + srcY * srcStride;
                uint16_t* dstRow = target16 + py * targetWidth;
                int32_t srcStartX = bounds.startX - destX;

                // If we have row span data, split into: left-alpha | opaque-middle | right-alpha.
                // Chroma-key forces the per-pixel path because the "opaque" middle still needs
                // per-pixel matching against the key.
                if (rowSpans && !hasTint && !hasAlphaTint && !hasKey)
                {
                    int16_t opaqueStart = rowSpans[srcY * 2];
                    int16_t opaqueEnd = rowSpans[srcY * 2 + 1];

                    // Clamp opaque span to blit bounds (in source coordinates)
                    int32_t srcEndX = bounds.endX - destX;
                    int32_t clampedOpaqueStart = (opaqueStart < srcStartX) ? srcStartX : opaqueStart;
                    int32_t clampedOpaqueEnd = (opaqueEnd > srcEndX) ? srcEndX : opaqueEnd;

                    // Left alpha region
                    const uint8_t* srcPtr = rowBase + srcStartX * bpp;
                    for (int32_t sx = srcStartX; sx < clampedOpaqueStart && sx < srcEndX; sx++)
                    {
                        uint8_t a = srcPtr[2];
                        if (a == 0) { srcPtr += bpp; continue; }
                        uint16_t srcRGB = *(const uint16_t*)srcPtr;
                        srcPtr += bpp;
                        if (a == 255) { dstRow[destX + sx] = srcRGB; continue; }
                        uint16_t bg = dstRow[destX + sx];
                        uint8_t invA = 255 - a;
                        uint8_t r = FAST_DIV255(((srcRGB >> 11) << 3) * a + (((bg >> 11) << 3)) * invA);
                        uint8_t g = FAST_DIV255((((srcRGB >> 5) & 0x3F) << 2) * a + ((((bg >> 5) & 0x3F) << 2)) * invA);
                        uint8_t b = FAST_DIV255(((srcRGB & 0x1F) << 3) * a + (((bg & 0x1F) << 3)) * invA);
                        dstRow[destX + sx] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
                    }

                    // Opaque middle — direct copy, no alpha checks
                    if (clampedOpaqueStart < clampedOpaqueEnd)
                    {
                        srcPtr = rowBase + clampedOpaqueStart * bpp;
                        for (int32_t sx = clampedOpaqueStart; sx < clampedOpaqueEnd; sx++)
                        {
                            dstRow[destX + sx] = *(const uint16_t*)srcPtr;
                            srcPtr += bpp;
                        }
                    }

                    // Right alpha region
                    srcPtr = rowBase + clampedOpaqueEnd * bpp;
                    for (int32_t sx = clampedOpaqueEnd; sx < srcEndX; sx++)
                    {
                        uint8_t a = srcPtr[2];
                        if (a == 0) { srcPtr += bpp; continue; }
                        uint16_t srcRGB = *(const uint16_t*)srcPtr;
                        srcPtr += bpp;
                        if (a == 255) { dstRow[destX + sx] = srcRGB; continue; }
                        uint16_t bg = dstRow[destX + sx];
                        uint8_t invA = 255 - a;
                        uint8_t r = FAST_DIV255(((srcRGB >> 11) << 3) * a + (((bg >> 11) << 3)) * invA);
                        uint8_t g = FAST_DIV255((((srcRGB >> 5) & 0x3F) << 2) * a + ((((bg >> 5) & 0x3F) << 2)) * invA);
                        uint8_t b = FAST_DIV255(((srcRGB & 0x1F) << 3) * a + (((bg & 0x1F) << 3)) * invA);
                        dstRow[destX + sx] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
                    }
                }
                else
                {
                    // Per-pixel alpha blend (with tint and chroma-key support)
                    const uint8_t* srcPtr = rowBase + srcStartX * bpp;

                    // SIMD fast-path: untinted, unkeyed alpha blend with bpp==3
                    // (RGB565A8). Kernel handles a==0 skip and a==255 short-circuit
                    // internally. Preconditions: format, scale=1, no rotation, and
                    // no tint/key already hold; we additionally require 16-byte
                    // alignment on both source and destination row pointers.
                    {
                        RowKernelFn blendKernel = s_Kernels[(int)KernelOp::RGB565A8_Blend_Row];
                        uint16_t* dstPtrSimd = dstRow + bounds.startX;
                        if (blendKernel && bpp == 3 && !hasTint && !hasAlphaTint && !hasKey
                            && Aligned16(srcPtr, dstPtrSimd))
                        {
                            blendKernel(srcPtr, (uint8_t*)dstPtrSimd,
                                        bounds.endX - bounds.startX,
                                        255, 255, 255, 255);
                            continue;  // next row
                        }
                    }

                    for (int32_t px = bounds.startX; px < bounds.endX; px++)
                    {
                        uint8_t a = srcPtr[2];
                        if (a == 0) { srcPtr += bpp; continue; }

                        uint8_t effectiveAlpha = hasAlphaTint ? FAST_DIV255(a * tintA) : a;
                        if (effectiveAlpha == 0) { srcPtr += bpp; continue; }

                        uint16_t srcRGB = *(const uint16_t*)srcPtr;
                        srcPtr += bpp;

                        if (hasKey)
                        {
                            uint8_t kr = (srcRGB >> 11) << 3;
                            uint8_t kg = ((srcRGB >> 5) & 0x3F) << 2;
                            uint8_t kb = (srcRGB & 0x1F) << 3;
                            if (kr == keyR && kg == keyG && kb == keyB) continue;
                        }

                        if (!hasTint && effectiveAlpha == 255)
                        {
                            dstRow[px] = srcRGB;
                        }
                        else
                        {
                            uint8_t r = (srcRGB >> 11) << 3;
                            uint8_t g = ((srcRGB >> 5) & 0x3F) << 2;
                            uint8_t b = (srcRGB & 0x1F) << 3;
                            if (hasTint) { r = FAST_DIV255(r * tintR); g = FAST_DIV255(g * tintG); b = FAST_DIV255(b * tintB); }
                            if (effectiveAlpha == 255)
                            {
                                dstRow[px] = ((r >> 3 << 11) | ((g >> 2) << 5) | (b >> 3));
                            }
                            else
                            {
                                uint16_t bg = dstRow[px];
                                uint8_t bgR = (bg >> 11) << 3;
                                uint8_t bgG = ((bg >> 5) & 0x3F) << 2;
                                uint8_t bgB = (bg & 0x1F) << 3;
                                uint8_t invA = 255 - effectiveAlpha;
                                r = FAST_DIV255(r * effectiveAlpha + bgR * invA);
                                g = FAST_DIV255(g * effectiveAlpha + bgG * invA);
                                b = FAST_DIV255(b * effectiveAlpha + bgB * invA);
                                dstRow[px] = ((r >> 3 << 11) | ((g >> 2) << 5) | (b >> 3));
                            }
                        }
                    }
                }
            }
        }
        return;
    }

    // ── Scaled path — fixed-point 16.16 with precomputed row pointers ──
    const uint32_t xStep = ((uint32_t)srcW << 16) / (uint32_t)destWidth;
    const uint32_t yStep = ((uint32_t)srcH << 16) / (uint32_t)destHeight;

    if (!source.hasAlpha)
    {
        if (!hasTint && !hasAlphaTint && !hasKey)
        {
            for (int32_t py = bounds.startY; py < bounds.endY; py++)
            {
                int32_t srcY = ((uint32_t)(py - destY) * yStep) >> 16;
                const uint8_t* srcRow = srcPixels + srcY * srcStride;
                uint16_t* dstRow = target16 + py * targetWidth;
                uint32_t srcX_acc = (uint32_t)(bounds.startX - destX) * xStep;
                for (int32_t px = bounds.startX; px < bounds.endX; px++)
                {
                    int32_t srcX = srcX_acc >> 16;
                    srcX_acc += xStep;
                    dstRow[px] = *(const uint16_t*)(srcRow + srcX * bpp);
                }
            }
        }
        else
        {
            uint8_t effectiveA = hasAlphaTint ? tintA : 255;
            for (int32_t py = bounds.startY; py < bounds.endY; py++)
            {
                int32_t srcY = ((uint32_t)(py - destY) * yStep) >> 16;
                const uint8_t* srcRow = srcPixels + srcY * srcStride;
                uint16_t* dstRow = target16 + py * targetWidth;
                uint32_t srcX_acc = (uint32_t)(bounds.startX - destX) * xStep;
                for (int32_t px = bounds.startX; px < bounds.endX; px++)
                {
                    int32_t srcX = srcX_acc >> 16;
                    srcX_acc += xStep;
                    uint16_t srcRGB = *(const uint16_t*)(srcRow + srcX * bpp);
                    uint8_t r = (srcRGB >> 11) << 3;
                    uint8_t g = ((srcRGB >> 5) & 0x3F) << 2;
                    uint8_t b = (srcRGB & 0x1F) << 3;
                    if (hasKey && r == keyR && g == keyG && b == keyB) continue;
                    if (hasTint) { r = FAST_DIV255(r * tintR); g = FAST_DIV255(g * tintG); b = FAST_DIV255(b * tintB); }
                    if (effectiveA == 255)
                    {
                        dstRow[px] = ((r >> 3 << 11) | ((g >> 2) << 5) | (b >> 3));
                    }
                    else
                    {
                        uint16_t bg = dstRow[px];
                        uint8_t bgR = (bg >> 11) << 3;
                        uint8_t bgG = ((bg >> 5) & 0x3F) << 2;
                        uint8_t bgB = (bg & 0x1F) << 3;
                        uint8_t invA = 255 - effectiveA;
                        r = FAST_DIV255(r * effectiveA + bgR * invA);
                        g = FAST_DIV255(g * effectiveA + bgG * invA);
                        b = FAST_DIV255(b * effectiveA + bgB * invA);
                        dstRow[px] = ((r >> 3 << 11) | ((g >> 2) << 5) | (b >> 3));
                    }
                }
            }
        }
        return;
    }

    // Alpha path: per-pixel alpha reads (scaled)
    for (int32_t py = bounds.startY; py < bounds.endY; py++)
    {
        int32_t srcY = ((uint32_t)(py - destY) * yStep) >> 16;
        const uint8_t* srcRow = srcPixels + srcY * srcStride;
        uint16_t* dstRow = target16 + py * targetWidth;

        uint32_t srcX_acc = (uint32_t)(bounds.startX - destX) * xStep;
        for (int32_t px = bounds.startX; px < bounds.endX; px++)
        {
            int32_t srcX = srcX_acc >> 16;
            srcX_acc += xStep;
            const uint8_t* srcPx = srcRow + srcX * bpp;

            uint8_t a = srcPx[2];
            if (a == 0) continue;

            uint8_t effectiveAlpha = hasAlphaTint ? FAST_DIV255(a * tintA) : a;
            if (effectiveAlpha == 0) continue;

            uint16_t srcRGB = *(const uint16_t*)srcPx;

            if (hasKey)
            {
                uint8_t kr = (srcRGB >> 11) << 3;
                uint8_t kg = ((srcRGB >> 5) & 0x3F) << 2;
                uint8_t kb = (srcRGB & 0x1F) << 3;
                if (kr == keyR && kg == keyG && kb == keyB) continue;
            }

            if (!hasTint && effectiveAlpha == 255)
            {
                dstRow[px] = srcRGB;
            }
            else
            {
                uint8_t r = (srcRGB >> 11) << 3;
                uint8_t g = ((srcRGB >> 5) & 0x3F) << 2;
                uint8_t b = (srcRGB & 0x1F) << 3;

                if (hasTint)
                {
                    r = FAST_DIV255(r * tintR);
                    g = FAST_DIV255(g * tintG);
                    b = FAST_DIV255(b * tintB);
                }

                if (effectiveAlpha == 255)
                {
                    dstRow[px] = ((r >> 3 << 11) | ((g >> 2) << 5) | (b >> 3));
                }
                else
                {
                    uint16_t bg = dstRow[px];
                    uint8_t bgR = (bg >> 11) << 3;
                    uint8_t bgG = ((bg >> 5) & 0x3F) << 2;
                    uint8_t bgB = (bg & 0x1F) << 3;

                    uint8_t invA = 255 - effectiveAlpha;
                    r = FAST_DIV255(r * effectiveAlpha + bgR * invA);
                    g = FAST_DIV255(g * effectiveAlpha + bgG * invA);
                    b = FAST_DIV255(b * effectiveAlpha + bgB * invA);
                    dstRow[px] = ((r >> 3 << 11) | ((g >> 2) << 5) | (b >> 3));
                }
            }
        }
    }
}

// ============================================================================
// Specialized BlitScaled: RGB565 source (opaque) → RGB565 target
// ============================================================================

static DEKI_FAST_ATTR void BlitScaled_RGB565_to_RGB565(
    const Source& source, uint16_t* target16, int32_t targetWidth,
    int32_t destX, int32_t destY, int32_t destWidth, int32_t destHeight,
    const BlitBounds& bounds, bool hasTint, bool hasAlphaTint,
    uint8_t tintR, uint8_t tintG, uint8_t tintB, uint8_t tintA)
{

    const uint8_t* srcBytes = source.pixels;
    int32_t srcW = source.width;
    int32_t srcH = source.height;
    const int32_t srcStride = SourceStride(source);
    const bool    hasKey = source.hasChromaKey;
    const uint8_t keyR = source.keyR, keyG = source.keyG, keyB = source.keyB;
    const int16_t* chromaRowSpans = source.chromaRowSpans;

    // ── 1:1 scale fast path — sequential pointer access ──
    if (destWidth == srcW && destHeight == srcH)
    {
        // Row-span fast path: chroma-keyed source with precomputed non-key column ranges.
        // Inside [start, end): direct memcpy (every pixel is non-key). Outside: skip without read.
        if (hasKey && chromaRowSpans && !hasTint && !hasAlphaTint)
        {
            RowKernelFn copyKernel = s_Kernels[(int)KernelOp::RGB565_Copy_Row];
            for (int32_t py = bounds.startY; py < bounds.endY; py++)
            {
                int32_t srcY = py - destY;
                int16_t opaqueStart = chromaRowSpans[srcY * 2];
                int16_t opaqueEnd   = chromaRowSpans[srcY * 2 + 1];

                int32_t srcStartX = bounds.startX - destX;
                int32_t srcEndX   = bounds.endX   - destX;
                int32_t clampedStart = (opaqueStart < srcStartX) ? srcStartX : (int32_t)opaqueStart;
                int32_t clampedEnd   = (opaqueEnd   > srcEndX)   ? srcEndX   : (int32_t)opaqueEnd;
                if (clampedStart >= clampedEnd) continue;

                const uint16_t* srcPtr = (const uint16_t*)(srcBytes + srcY * srcStride) + clampedStart;
                uint16_t* dstPtr = target16 + py * targetWidth + (destX + clampedStart);
                int32_t rowPixels = clampedEnd - clampedStart;
                if (copyKernel && Aligned16(srcPtr, dstPtr))
                    copyKernel((const uint8_t*)srcPtr, (uint8_t*)dstPtr, rowPixels, 255, 255, 255, 255);
                else
                    memcpy(dstPtr, srcPtr, rowPixels * sizeof(uint16_t));
            }
            return;
        }

        if (!hasTint && !hasAlphaTint && !hasKey)
        {
            const int32_t rowPixels = bounds.endX - bounds.startX;
            RowKernelFn copyKernel = s_Kernels[(int)KernelOp::RGB565_Copy_Row];
            for (int32_t py = bounds.startY; py < bounds.endY; py++)
            {
                const uint16_t* srcPtr = (const uint16_t*)(srcBytes + (py - destY) * srcStride) + (bounds.startX - destX);
                uint16_t* dstPtr = target16 + py * targetWidth + bounds.startX;
                if (copyKernel && Aligned16(srcPtr, dstPtr))
                    copyKernel((const uint8_t*)srcPtr, (uint8_t*)dstPtr, rowPixels, 255, 255, 255, 255);
                else
                    memcpy(dstPtr, srcPtr, rowPixels * sizeof(uint16_t));
            }
        }
        else
        {
            for (int32_t py = bounds.startY; py < bounds.endY; py++)
            {
                const uint16_t* srcPtr = (const uint16_t*)(srcBytes + (py - destY) * srcStride) + (bounds.startX - destX);
                uint16_t* dstRow = target16 + py * targetWidth;
                for (int32_t px = bounds.startX; px < bounds.endX; px++)
                {
                    uint16_t srcRGB = *srcPtr++;
                    uint8_t r = (srcRGB >> 11) << 3;
                    uint8_t g = ((srcRGB >> 5) & 0x3F) << 2;
                    uint8_t b = (srcRGB & 0x1F) << 3;
                    if (hasKey && r == keyR && g == keyG && b == keyB) continue;
                    if (hasTint) { r = FAST_DIV255(r * tintR); g = FAST_DIV255(g * tintG); b = FAST_DIV255(b * tintB); }
                    if (hasAlphaTint && tintA < 255)
                    {
                        uint16_t bg = dstRow[px];
                        uint8_t bgR = (bg >> 11) << 3;
                        uint8_t bgG = ((bg >> 5) & 0x3F) << 2;
                        uint8_t bgB = (bg & 0x1F) << 3;
                        uint8_t invA = 255 - tintA;
                        r = FAST_DIV255(r * tintA + bgR * invA);
                        g = FAST_DIV255(g * tintA + bgG * invA);
                        b = FAST_DIV255(b * tintA + bgB * invA);
                    }
                    dstRow[px] = ((r >> 3 << 11) | ((g >> 2) << 5) | (b >> 3));
                }
            }
        }
        return;
    }

    // ── Scaled path — fixed-point 16.16 ──
    const uint32_t xStep = ((uint32_t)srcW << 16) / (uint32_t)destWidth;
    const uint32_t yStep = ((uint32_t)srcH << 16) / (uint32_t)destHeight;

    if (!hasTint && !hasAlphaTint && !hasKey)
    {
        for (int32_t py = bounds.startY; py < bounds.endY; py++)
        {
            int32_t srcY = ((uint32_t)(py - destY) * yStep) >> 16;
            const uint16_t* srcRow = (const uint16_t*)(srcBytes + srcY * srcStride);
            uint16_t* dstRow = target16 + py * targetWidth;

            uint32_t srcX_acc = (uint32_t)(bounds.startX - destX) * xStep;
            for (int32_t px = bounds.startX; px < bounds.endX; px++)
            {
                int32_t srcX = srcX_acc >> 16;
                srcX_acc += xStep;
                dstRow[px] = srcRow[srcX];
            }
        }
        return;
    }

    for (int32_t py = bounds.startY; py < bounds.endY; py++)
    {
        int32_t srcY = ((uint32_t)(py - destY) * yStep) >> 16;
        const uint16_t* srcRow = (const uint16_t*)(srcBytes + srcY * srcStride);
        uint16_t* dstRow = target16 + py * targetWidth;

        uint32_t srcX_acc = (uint32_t)(bounds.startX - destX) * xStep;
        for (int32_t px = bounds.startX; px < bounds.endX; px++)
        {
            int32_t srcX = srcX_acc >> 16;
            srcX_acc += xStep;
            uint16_t srcRGB = srcRow[srcX];

            uint8_t r = (srcRGB >> 11) << 3;
            uint8_t g = ((srcRGB >> 5) & 0x3F) << 2;
            uint8_t b = (srcRGB & 0x1F) << 3;

            if (hasKey && r == keyR && g == keyG && b == keyB) continue;

            if (hasTint)
            {
                r = FAST_DIV255(r * tintR);
                g = FAST_DIV255(g * tintG);
                b = FAST_DIV255(b * tintB);
            }

            if (hasAlphaTint && tintA < 255)
            {
                uint16_t bg = dstRow[px];
                uint8_t bgR = (bg >> 11) << 3;
                uint8_t bgG = ((bg >> 5) & 0x3F) << 2;
                uint8_t bgB = (bg & 0x1F) << 3;

                uint8_t invA = 255 - tintA;
                r = FAST_DIV255(r * tintA + bgR * invA);
                g = FAST_DIV255(g * tintA + bgG * invA);
                b = FAST_DIV255(b * tintA + bgB * invA);
            }

            dstRow[px] = ((r >> 3 << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
}

// ============================================================================
// Specialized BlitScaled: RGBA8888 source → ARGB8888 target
// ============================================================================

static DEKI_FAST_ATTR void BlitScaled_RGBA8888_to_ARGB8888(
    const Source& source, uint32_t* target32, int32_t targetWidth,
    int32_t destX, int32_t destY, int32_t destWidth, int32_t destHeight,
    const BlitBounds& bounds, bool hasTint, bool hasAlphaTint,
    uint8_t tintR, uint8_t tintG, uint8_t tintB, uint8_t tintA)
{
    const uint8_t* srcPixels = source.pixels;
    int32_t srcW = source.width;
    int32_t srcH = source.height;
    const int32_t srcStride = SourceStride(source);
    const bool    hasKey = source.hasChromaKey;
    const uint8_t keyR = source.keyR, keyG = source.keyG, keyB = source.keyB;

    const uint32_t xStep = ((uint32_t)srcW << 16) / (uint32_t)destWidth;
    const uint32_t yStep = ((uint32_t)srcH << 16) / (uint32_t)destHeight;

    for (int32_t py = bounds.startY; py < bounds.endY; py++)
    {
        int32_t srcY = ((uint32_t)(py - destY) * yStep) >> 16;
        const uint8_t* srcRow = srcPixels + srcY * srcStride;
        uint32_t* dstRow = target32 + py * targetWidth;

        uint32_t srcX_acc = (uint32_t)(bounds.startX - destX) * xStep;
        for (int32_t px = bounds.startX; px < bounds.endX; px++)
        {
            int32_t srcX = srcX_acc >> 16;
            srcX_acc += xStep;
            const uint8_t* pixel = srcRow + srcX * 4;

            uint8_t a = pixel[3];
            if (a == 0) continue;

            uint8_t r = pixel[0];
            uint8_t g = pixel[1];
            uint8_t b = pixel[2];

            if (hasKey && r == keyR && g == keyG && b == keyB) continue;

            if (hasTint)
            {
                r = FAST_DIV255(r * tintR);
                g = FAST_DIV255(g * tintG);
                b = FAST_DIV255(b * tintB);
            }

            uint8_t effectiveAlpha = hasAlphaTint ? FAST_DIV255(a * tintA) : a;
            if (effectiveAlpha == 0) continue;

            if (effectiveAlpha == 255)
            {
                dstRow[px] = (0xFF << 24) | (r << 16) | (g << 8) | b;
            }
            else
            {
                uint32_t bgPixel = dstRow[px];
                uint8_t bgR = (bgPixel >> 16) & 0xFF;
                uint8_t bgG = (bgPixel >> 8) & 0xFF;
                uint8_t bgB = bgPixel & 0xFF;

                uint8_t invA = 255 - effectiveAlpha;
                r = FAST_DIV255(r * effectiveAlpha + bgR * invA);
                g = FAST_DIV255(g * effectiveAlpha + bgG * invA);
                b = FAST_DIV255(b * effectiveAlpha + bgB * invA);
                dstRow[px] = (0xFF << 24) | (r << 16) | (g << 8) | b;
            }
        }
    }
}

// ============================================================================
// Specialized BlitScaled: RGB565A8 source → ARGB8888 target
// ============================================================================

static DEKI_FAST_ATTR void BlitScaled_RGB565A8_to_ARGB8888(
    const Source& source, uint32_t* target32, int32_t targetWidth,
    int32_t destX, int32_t destY, int32_t destWidth, int32_t destHeight,
    const BlitBounds& bounds, bool hasTint, bool hasAlphaTint,
    uint8_t tintR, uint8_t tintG, uint8_t tintB, uint8_t tintA)
{
    const uint8_t* srcPixels = source.pixels;
    int32_t srcW = source.width;
    int32_t srcH = source.height;
    const int32_t srcStride = SourceStride(source);
    const int32_t bpp = source.bytesPerPixel;
    const bool    hasKey = source.hasChromaKey;
    const uint8_t keyR = source.keyR, keyG = source.keyG, keyB = source.keyB;

    const uint32_t xStep = ((uint32_t)srcW << 16) / (uint32_t)destWidth;
    const uint32_t yStep = ((uint32_t)srcH << 16) / (uint32_t)destHeight;

    for (int32_t py = bounds.startY; py < bounds.endY; py++)
    {
        int32_t srcY = ((uint32_t)(py - destY) * yStep) >> 16;
        const uint8_t* srcRow = srcPixels + srcY * srcStride;
        uint32_t* dstRow = target32 + py * targetWidth;

        uint32_t srcX_acc = (uint32_t)(bounds.startX - destX) * xStep;
        for (int32_t px = bounds.startX; px < bounds.endX; px++)
        {
            int32_t srcX = srcX_acc >> 16;
            srcX_acc += xStep;
            const uint8_t* srcPx = srcRow + srcX * bpp;

            uint8_t a = srcPx[2];
            if (a == 0) continue;

            uint16_t srcRGB = *(const uint16_t*)srcPx;
            uint8_t r = (srcRGB >> 11) << 3;
            uint8_t g = ((srcRGB >> 5) & 0x3F) << 2;
            uint8_t b = (srcRGB & 0x1F) << 3;

            if (hasKey && r == keyR && g == keyG && b == keyB) continue;

            if (hasTint)
            {
                r = FAST_DIV255(r * tintR);
                g = FAST_DIV255(g * tintG);
                b = FAST_DIV255(b * tintB);
            }

            uint8_t effectiveAlpha = hasAlphaTint ? FAST_DIV255(a * tintA) : a;
            if (effectiveAlpha == 0) continue;

            if (effectiveAlpha == 255)
            {
                dstRow[px] = (0xFF << 24) | (r << 16) | (g << 8) | b;
            }
            else
            {
                uint32_t bgPixel = dstRow[px];
                uint8_t bgR = (bgPixel >> 16) & 0xFF;
                uint8_t bgG = (bgPixel >> 8) & 0xFF;
                uint8_t bgB = bgPixel & 0xFF;

                uint8_t invA = 255 - effectiveAlpha;
                r = FAST_DIV255(r * effectiveAlpha + bgR * invA);
                g = FAST_DIV255(g * effectiveAlpha + bgG * invA);
                b = FAST_DIV255(b * effectiveAlpha + bgB * invA);
                dstRow[px] = (0xFF << 24) | (r << 16) | (g << 8) | b;
            }
        }
    }
}

// ============================================================================
// Specialized BlitScaled: RGB565 source (opaque) → ARGB8888 target
// ============================================================================

static DEKI_FAST_ATTR void BlitScaled_RGB565_to_ARGB8888(
    const Source& source, uint32_t* target32, int32_t targetWidth,
    int32_t destX, int32_t destY, int32_t destWidth, int32_t destHeight,
    const BlitBounds& bounds, bool hasTint, bool hasAlphaTint,
    uint8_t tintR, uint8_t tintG, uint8_t tintB, uint8_t tintA)
{
    const uint8_t* srcBytes = source.pixels;
    int32_t srcW = source.width;
    int32_t srcH = source.height;
    const int32_t srcStride = SourceStride(source);
    const bool    hasKey = source.hasChromaKey;
    const uint8_t keyR = source.keyR, keyG = source.keyG, keyB = source.keyB;

    const uint32_t xStep = ((uint32_t)srcW << 16) / (uint32_t)destWidth;
    const uint32_t yStep = ((uint32_t)srcH << 16) / (uint32_t)destHeight;

    for (int32_t py = bounds.startY; py < bounds.endY; py++)
    {
        int32_t srcY = ((uint32_t)(py - destY) * yStep) >> 16;
        const uint16_t* srcRow = (const uint16_t*)(srcBytes + srcY * srcStride);
        uint32_t* dstRow = target32 + py * targetWidth;

        uint32_t srcX_acc = (uint32_t)(bounds.startX - destX) * xStep;
        for (int32_t px = bounds.startX; px < bounds.endX; px++)
        {
            int32_t srcX = srcX_acc >> 16;
            srcX_acc += xStep;
            uint16_t srcRGB = srcRow[srcX];

            uint8_t r = (srcRGB >> 11) << 3;
            uint8_t g = ((srcRGB >> 5) & 0x3F) << 2;
            uint8_t b = (srcRGB & 0x1F) << 3;

            if (hasKey && r == keyR && g == keyG && b == keyB) continue;

            if (hasTint)
            {
                r = FAST_DIV255(r * tintR);
                g = FAST_DIV255(g * tintG);
                b = FAST_DIV255(b * tintB);
            }

            if (hasAlphaTint && tintA < 255)
            {
                uint32_t bgPixel = dstRow[px];
                uint8_t bgR = (bgPixel >> 16) & 0xFF;
                uint8_t bgG = (bgPixel >> 8) & 0xFF;
                uint8_t bgB = bgPixel & 0xFF;

                uint8_t invA = 255 - tintA;
                r = FAST_DIV255(r * tintA + bgR * invA);
                g = FAST_DIV255(g * tintA + bgG * invA);
                b = FAST_DIV255(b * tintA + bgB * invA);
            }

            dstRow[px] = (0xFF << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

// ============================================================================
// Specialized BlitScaled: RGBA8888 source → RGB888 target
// ============================================================================

static DEKI_FAST_ATTR void BlitScaled_RGBA8888_to_RGB888(
    const Source& source, uint8_t* target, int32_t targetWidth,
    int32_t destX, int32_t destY, int32_t destWidth, int32_t destHeight,
    const BlitBounds& bounds, bool hasTint, bool hasAlphaTint,
    uint8_t tintR, uint8_t tintG, uint8_t tintB, uint8_t tintA)
{
    const uint8_t* srcPixels = source.pixels;
    int32_t srcW = source.width;
    int32_t srcH = source.height;
    const int32_t srcStride = SourceStride(source);
    const bool    hasKey = source.hasChromaKey;
    const uint8_t keyR = source.keyR, keyG = source.keyG, keyB = source.keyB;

    const uint32_t xStep = ((uint32_t)srcW << 16) / (uint32_t)destWidth;
    const uint32_t yStep = ((uint32_t)srcH << 16) / (uint32_t)destHeight;

    for (int32_t py = bounds.startY; py < bounds.endY; py++)
    {
        int32_t srcY = ((uint32_t)(py - destY) * yStep) >> 16;
        const uint8_t* srcRow = srcPixels + srcY * srcStride;

        uint32_t srcX_acc = (uint32_t)(bounds.startX - destX) * xStep;
        for (int32_t px = bounds.startX; px < bounds.endX; px++)
        {
            int32_t srcX = srcX_acc >> 16;
            srcX_acc += xStep;
            const uint8_t* pixel = srcRow + srcX * 4;

            uint8_t a = pixel[3];
            if (a == 0) continue;

            uint8_t r = pixel[0];
            uint8_t g = pixel[1];
            uint8_t b = pixel[2];

            if (hasKey && r == keyR && g == keyG && b == keyB) continue;

            if (hasTint)
            {
                r = FAST_DIV255(r * tintR);
                g = FAST_DIV255(g * tintG);
                b = FAST_DIV255(b * tintB);
            }

            uint8_t effectiveAlpha = hasAlphaTint ? FAST_DIV255(a * tintA) : a;
            if (effectiveAlpha == 0) continue;

            size_t dstIdx = (py * targetWidth + px) * 3;
            if (effectiveAlpha == 255)
            {
                target[dstIdx] = r;
                target[dstIdx + 1] = g;
                target[dstIdx + 2] = b;
            }
            else
            {
                uint8_t invA = 255 - effectiveAlpha;
                target[dstIdx]     = FAST_DIV255(r * effectiveAlpha + target[dstIdx]     * invA);
                target[dstIdx + 1] = FAST_DIV255(g * effectiveAlpha + target[dstIdx + 1] * invA);
                target[dstIdx + 2] = FAST_DIV255(b * effectiveAlpha + target[dstIdx + 2] * invA);
            }
        }
    }
}

// ============================================================================
// Specialized BlitScaled: RGB565A8 source → RGB888 target
// ============================================================================

static DEKI_FAST_ATTR void BlitScaled_RGB565A8_to_RGB888(
    const Source& source, uint8_t* target, int32_t targetWidth,
    int32_t destX, int32_t destY, int32_t destWidth, int32_t destHeight,
    const BlitBounds& bounds, bool hasTint, bool hasAlphaTint,
    uint8_t tintR, uint8_t tintG, uint8_t tintB, uint8_t tintA)
{
    const uint8_t* srcPixels = source.pixels;
    int32_t srcW = source.width;
    int32_t srcH = source.height;
    const int32_t srcStride = SourceStride(source);
    const int32_t bpp = source.bytesPerPixel;
    const bool    hasKey = source.hasChromaKey;
    const uint8_t keyR = source.keyR, keyG = source.keyG, keyB = source.keyB;

    const uint32_t xStep = ((uint32_t)srcW << 16) / (uint32_t)destWidth;
    const uint32_t yStep = ((uint32_t)srcH << 16) / (uint32_t)destHeight;

    for (int32_t py = bounds.startY; py < bounds.endY; py++)
    {
        int32_t srcY = ((uint32_t)(py - destY) * yStep) >> 16;
        const uint8_t* srcRow = srcPixels + srcY * srcStride;

        uint32_t srcX_acc = (uint32_t)(bounds.startX - destX) * xStep;
        for (int32_t px = bounds.startX; px < bounds.endX; px++)
        {
            int32_t srcX = srcX_acc >> 16;
            srcX_acc += xStep;
            const uint8_t* srcPx = srcRow + srcX * bpp;

            uint8_t a = srcPx[2];
            if (a == 0) continue;

            uint16_t srcRGB = *(const uint16_t*)srcPx;
            uint8_t r = (srcRGB >> 11) << 3;
            uint8_t g = ((srcRGB >> 5) & 0x3F) << 2;
            uint8_t b = (srcRGB & 0x1F) << 3;

            if (hasKey && r == keyR && g == keyG && b == keyB) continue;

            if (hasTint)
            {
                r = FAST_DIV255(r * tintR);
                g = FAST_DIV255(g * tintG);
                b = FAST_DIV255(b * tintB);
            }

            uint8_t effectiveAlpha = hasAlphaTint ? FAST_DIV255(a * tintA) : a;
            if (effectiveAlpha == 0) continue;

            size_t dstIdx = (py * targetWidth + px) * 3;
            if (effectiveAlpha == 255)
            {
                target[dstIdx] = r;
                target[dstIdx + 1] = g;
                target[dstIdx + 2] = b;
            }
            else
            {
                uint8_t invA = 255 - effectiveAlpha;
                target[dstIdx]     = FAST_DIV255(r * effectiveAlpha + target[dstIdx]     * invA);
                target[dstIdx + 1] = FAST_DIV255(g * effectiveAlpha + target[dstIdx + 1] * invA);
                target[dstIdx + 2] = FAST_DIV255(b * effectiveAlpha + target[dstIdx + 2] * invA);
            }
        }
    }
}

// ============================================================================
// Specialized BlitScaled: RGB565 source (opaque) → RGB888 target
// ============================================================================

static DEKI_FAST_ATTR void BlitScaled_RGB565_to_RGB888(
    const Source& source, uint8_t* target, int32_t targetWidth,
    int32_t destX, int32_t destY, int32_t destWidth, int32_t destHeight,
    const BlitBounds& bounds, bool hasTint, bool hasAlphaTint,
    uint8_t tintR, uint8_t tintG, uint8_t tintB, uint8_t tintA)
{
    const uint8_t* srcBytes = source.pixels;
    int32_t srcW = source.width;
    int32_t srcH = source.height;
    const int32_t srcStride = SourceStride(source);
    const bool    hasKey = source.hasChromaKey;
    const uint8_t keyR = source.keyR, keyG = source.keyG, keyB = source.keyB;

    const uint32_t xStep = ((uint32_t)srcW << 16) / (uint32_t)destWidth;
    const uint32_t yStep = ((uint32_t)srcH << 16) / (uint32_t)destHeight;

    for (int32_t py = bounds.startY; py < bounds.endY; py++)
    {
        int32_t srcY = ((uint32_t)(py - destY) * yStep) >> 16;
        const uint16_t* srcRow = (const uint16_t*)(srcBytes + srcY * srcStride);

        uint32_t srcX_acc = (uint32_t)(bounds.startX - destX) * xStep;
        for (int32_t px = bounds.startX; px < bounds.endX; px++)
        {
            int32_t srcX = srcX_acc >> 16;
            srcX_acc += xStep;
            uint16_t srcRGB = srcRow[srcX];

            uint8_t r = (srcRGB >> 11) << 3;
            uint8_t g = ((srcRGB >> 5) & 0x3F) << 2;
            uint8_t b = (srcRGB & 0x1F) << 3;

            if (hasKey && r == keyR && g == keyG && b == keyB) continue;

            if (hasTint)
            {
                r = FAST_DIV255(r * tintR);
                g = FAST_DIV255(g * tintG);
                b = FAST_DIV255(b * tintB);
            }

            size_t dstIdx = (py * targetWidth + px) * 3;

            if (hasAlphaTint && tintA < 255)
            {
                uint8_t invA = 255 - tintA;
                target[dstIdx]     = FAST_DIV255(r * tintA + target[dstIdx]     * invA);
                target[dstIdx + 1] = FAST_DIV255(g * tintA + target[dstIdx + 1] * invA);
                target[dstIdx + 2] = FAST_DIV255(b * tintA + target[dstIdx + 2] * invA);
            }
            else
            {
                target[dstIdx] = r;
                target[dstIdx + 1] = g;
                target[dstIdx + 2] = b;
            }
        }
    }
}

// ============================================================================
// RGB565A8 target helpers
// ============================================================================
//
// RGB565A8 target byte layout (matching the existing source-side layout):
//   byte 0: low byte of RGB565
//   byte 1: high byte of RGB565
//   byte 2: alpha (0 = uncovered, 255 = fully covered)

static inline void WriteRGB565A8(uint8_t* dst, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    uint16_t rgb565 = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    dst[0] = (uint8_t)(rgb565 & 0xFF);
    dst[1] = (uint8_t)((rgb565 >> 8) & 0xFF);
    dst[2] = a;
}

static inline void UnpackRGB565A8(const uint8_t* src,
                                   uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a)
{
    uint16_t rgb565 = (uint16_t)src[0] | ((uint16_t)src[1] << 8);
    r = (uint8_t)((rgb565 >> 11) << 3);
    g = (uint8_t)(((rgb565 >> 5) & 0x3F) << 2);
    b = (uint8_t)((rgb565 & 0x1F) << 3);
    a = src[2];
}

// Standard src-over alpha union: out.a = src.a + dst.a * (255 - src.a) / 255.
// On a freshly-cleared (dst.a == 0) target this collapses to out.a = src.a,
// which matches DarknessOverlayPass's "is covered" semantics.
static inline uint8_t AlphaUnion(uint8_t srcA, uint8_t dstA)
{
    if (srcA == 255) return 255;
    if (dstA == 0)   return srcA;
    return (uint8_t)(srcA + FAST_DIV255((uint16_t)dstA * (uint16_t)(255 - srcA)));
}

// ============================================================================
// Specialized BlitScaled: RGB565 source (opaque) → RGB565A8 target
// ============================================================================

static DEKI_FAST_ATTR void BlitScaled_RGB565_to_RGB565A8(
    const Source& source, uint8_t* target, int32_t targetWidth,
    int32_t destX, int32_t destY, int32_t destWidth, int32_t destHeight,
    const BlitBounds& bounds, bool hasTint, bool hasAlphaTint,
    uint8_t tintR, uint8_t tintG, uint8_t tintB, uint8_t tintA)
{
    const uint8_t* srcBytes = source.pixels;
    int32_t srcW = source.width;
    int32_t srcH = source.height;
    const int32_t srcStride = SourceStride(source);
    const bool    hasKey = source.hasChromaKey;
    const uint8_t keyR = source.keyR, keyG = source.keyG, keyB = source.keyB;

    // ── 1:1 scale fast path ──
    if (destWidth == srcW && destHeight == srcH)
    {
        if (!hasTint && !hasAlphaTint && !hasKey)
        {
            const int32_t rowPixels = bounds.endX - bounds.startX;
            RowKernelFn expandKernel = s_Kernels[(int)KernelOp::RGB565_to_RGB565A8_Row];
            for (int32_t py = bounds.startY; py < bounds.endY; py++)
            {
                const uint16_t* srcPtr = (const uint16_t*)(srcBytes + (py - destY) * srcStride) + (bounds.startX - destX);
                uint8_t* dstPtr = target + (py * targetWidth + bounds.startX) * 3;
                if (expandKernel && Aligned16(srcPtr, dstPtr))
                {
                    expandKernel((const uint8_t*)srcPtr, dstPtr, rowPixels, 255, 255, 255, 255);
                }
                else
                {
                    for (int32_t i = 0; i < rowPixels; i++)
                    {
                        uint16_t rgb565 = srcPtr[i];
                        dstPtr[i * 3]     = (uint8_t)(rgb565 & 0xFF);
                        dstPtr[i * 3 + 1] = (uint8_t)((rgb565 >> 8) & 0xFF);
                        dstPtr[i * 3 + 2] = 0xFF;
                    }
                }
            }
            return;
        }

        for (int32_t py = bounds.startY; py < bounds.endY; py++)
        {
            const uint16_t* srcPtr = (const uint16_t*)(srcBytes + (py - destY) * srcStride) + (bounds.startX - destX);
            uint8_t* dstRow = target + py * targetWidth * 3;
            for (int32_t px = bounds.startX; px < bounds.endX; px++)
            {
                uint16_t srcRGB = *srcPtr++;
                uint8_t r = (uint8_t)((srcRGB >> 11) << 3);
                uint8_t g = (uint8_t)(((srcRGB >> 5) & 0x3F) << 2);
                uint8_t b = (uint8_t)((srcRGB & 0x1F) << 3);
                if (hasKey && r == keyR && g == keyG && b == keyB) continue;
                if (hasTint) { r = FAST_DIV255(r * tintR); g = FAST_DIV255(g * tintG); b = FAST_DIV255(b * tintB); }
                uint8_t* dstPx = dstRow + px * 3;
                if (hasAlphaTint && tintA < 255)
                {
                    uint8_t bgR, bgG, bgB, bgA;
                    UnpackRGB565A8(dstPx, bgR, bgG, bgB, bgA);
                    uint8_t invA = (uint8_t)(255 - tintA);
                    r = FAST_DIV255(r * tintA + bgR * invA);
                    g = FAST_DIV255(g * tintA + bgG * invA);
                    b = FAST_DIV255(b * tintA + bgB * invA);
                    WriteRGB565A8(dstPx, r, g, b, AlphaUnion(tintA, bgA));
                }
                else
                {
                    WriteRGB565A8(dstPx, r, g, b, 0xFF);
                }
            }
        }
        return;
    }

    // ── Scaled path ──
    const uint32_t xStep = ((uint32_t)srcW << 16) / (uint32_t)destWidth;
    const uint32_t yStep = ((uint32_t)srcH << 16) / (uint32_t)destHeight;

    for (int32_t py = bounds.startY; py < bounds.endY; py++)
    {
        int32_t srcY = ((uint32_t)(py - destY) * yStep) >> 16;
        const uint16_t* srcRow = (const uint16_t*)(srcBytes + srcY * srcStride);
        uint8_t* dstRow = target + py * targetWidth * 3;

        uint32_t srcX_acc = (uint32_t)(bounds.startX - destX) * xStep;
        for (int32_t px = bounds.startX; px < bounds.endX; px++)
        {
            int32_t srcX = srcX_acc >> 16;
            srcX_acc += xStep;
            uint16_t srcRGB = srcRow[srcX];

            uint8_t r = (uint8_t)((srcRGB >> 11) << 3);
            uint8_t g = (uint8_t)(((srcRGB >> 5) & 0x3F) << 2);
            uint8_t b = (uint8_t)((srcRGB & 0x1F) << 3);

            if (hasKey && r == keyR && g == keyG && b == keyB) continue;
            if (hasTint) { r = FAST_DIV255(r * tintR); g = FAST_DIV255(g * tintG); b = FAST_DIV255(b * tintB); }

            uint8_t* dstPx = dstRow + px * 3;
            if (hasAlphaTint && tintA < 255)
            {
                uint8_t bgR, bgG, bgB, bgA;
                UnpackRGB565A8(dstPx, bgR, bgG, bgB, bgA);
                uint8_t invA = (uint8_t)(255 - tintA);
                r = FAST_DIV255(r * tintA + bgR * invA);
                g = FAST_DIV255(g * tintA + bgG * invA);
                b = FAST_DIV255(b * tintA + bgB * invA);
                WriteRGB565A8(dstPx, r, g, b, AlphaUnion(tintA, bgA));
            }
            else
            {
                WriteRGB565A8(dstPx, r, g, b, 0xFF);
            }
        }
    }
}

// ============================================================================
// Specialized BlitScaled: RGB565A8 source → RGB565A8 target
// ============================================================================

static DEKI_FAST_ATTR void BlitScaled_RGB565A8_to_RGB565A8(
    const Source& source, uint8_t* target, int32_t targetWidth,
    int32_t destX, int32_t destY, int32_t destWidth, int32_t destHeight,
    const BlitBounds& bounds, bool hasTint, bool hasAlphaTint,
    uint8_t tintR, uint8_t tintG, uint8_t tintB, uint8_t tintA)
{
    const uint8_t* srcPixels = source.pixels;
    int32_t srcW = source.width;
    int32_t srcH = source.height;
    const int32_t bpp = source.bytesPerPixel;
    const int32_t srcStride = SourceStride(source);
    const bool    hasKey = source.hasChromaKey;
    const uint8_t keyR = source.keyR, keyG = source.keyG, keyB = source.keyB;

    // ── 1:1 scale fast path ──
    if (destWidth == srcW && destHeight == srcH)
    {
        // Source has no per-pixel alpha (treat as fully opaque) — opaque copy path
        if (!source.hasAlpha)
        {
            const int32_t rowPixels = bounds.endX - bounds.startX;
            for (int32_t py = bounds.startY; py < bounds.endY; py++)
            {
                const uint8_t* srcPtr = srcPixels + (py - destY) * srcStride + (bounds.startX - destX) * bpp;
                uint8_t* dstPtr = target + (py * targetWidth + bounds.startX) * 3;

                if (!hasTint && !hasAlphaTint && !hasKey && bpp == 3)
                {
                    RowKernelFn copyKernel = s_Kernels[(int)KernelOp::RGB565A8_Copy_Row];
                    if (copyKernel && Aligned16(srcPtr, dstPtr))
                    {
                        copyKernel(srcPtr, dstPtr, rowPixels, 255, 255, 255, 255);
                        continue;
                    }
                    // Source bytes 0,1 are RGB565; byte 2 is whatever (often 0xFF
                    // for "opaque" RGB565A8 sources) — but since !source.hasAlpha
                    // we treat it as opaque and OVERWRITE alpha to 0xFF.
                    for (int32_t i = 0; i < rowPixels; i++)
                    {
                        dstPtr[i * 3]     = srcPtr[i * bpp];
                        dstPtr[i * 3 + 1] = srcPtr[i * bpp + 1];
                        dstPtr[i * 3 + 2] = 0xFF;
                    }
                    continue;
                }

                uint8_t effectiveA = hasAlphaTint ? tintA : 255;
                for (int32_t i = 0; i < rowPixels; i++)
                {
                    const uint8_t* srcPx = srcPtr + i * bpp;
                    uint8_t r, g, b, sa; UnpackRGB565A8(srcPx, r, g, b, sa);
                    if (hasKey && r == keyR && g == keyG && b == keyB) continue;
                    if (hasTint) { r = FAST_DIV255(r * tintR); g = FAST_DIV255(g * tintG); b = FAST_DIV255(b * tintB); }
                    uint8_t* dstPx = dstPtr + i * 3;
                    if (effectiveA == 255)
                    {
                        WriteRGB565A8(dstPx, r, g, b, 0xFF);
                    }
                    else
                    {
                        uint8_t bgR, bgG, bgB, bgA;
                        UnpackRGB565A8(dstPx, bgR, bgG, bgB, bgA);
                        uint8_t invA = (uint8_t)(255 - effectiveA);
                        r = FAST_DIV255(r * effectiveA + bgR * invA);
                        g = FAST_DIV255(g * effectiveA + bgG * invA);
                        b = FAST_DIV255(b * effectiveA + bgB * invA);
                        WriteRGB565A8(dstPx, r, g, b, AlphaUnion(effectiveA, bgA));
                    }
                }
            }
            return;
        }

        // Source has alpha — per-pixel blend path
        for (int32_t py = bounds.startY; py < bounds.endY; py++)
        {
            const uint8_t* srcPtr = srcPixels + (py - destY) * srcStride + (bounds.startX - destX) * bpp;
            uint8_t* dstPtr = target + (py * targetWidth + bounds.startX) * 3;
            const int32_t rowPixels = bounds.endX - bounds.startX;

            // SIMD fast-path: untinted, unkeyed alpha blend with bpp==3
            // (RGB565A8 → RGB565A8). Kernel handles a==0 skip and a==255
            // short-circuit internally.
            if (!hasTint && !hasAlphaTint && !hasKey && bpp == 3)
            {
                RowKernelFn blendKernel = s_Kernels[(int)KernelOp::RGB565A8_Blend_Row_Dest_RGB565A8];
                if (blendKernel && Aligned16(srcPtr, dstPtr))
                {
                    blendKernel(srcPtr, dstPtr, rowPixels, 255, 255, 255, 255);
                    continue;
                }
            }

            for (int32_t i = 0; i < rowPixels; i++)
            {
                const uint8_t* srcPx = srcPtr + i * bpp;
                uint8_t r, g, b, sa; UnpackRGB565A8(srcPx, r, g, b, sa);
                if (sa == 0) continue;

                uint8_t effectiveAlpha = hasAlphaTint ? FAST_DIV255((uint16_t)sa * (uint16_t)tintA) : sa;
                if (effectiveAlpha == 0) continue;

                if (hasKey && r == keyR && g == keyG && b == keyB) continue;

                if (hasTint) { r = FAST_DIV255(r * tintR); g = FAST_DIV255(g * tintG); b = FAST_DIV255(b * tintB); }

                uint8_t* dstPx = dstPtr + i * 3;
                if (effectiveAlpha == 255)
                {
                    WriteRGB565A8(dstPx, r, g, b, 0xFF);
                }
                else
                {
                    uint8_t bgR, bgG, bgB, bgA;
                    UnpackRGB565A8(dstPx, bgR, bgG, bgB, bgA);
                    uint8_t invA = (uint8_t)(255 - effectiveAlpha);
                    r = FAST_DIV255(r * effectiveAlpha + bgR * invA);
                    g = FAST_DIV255(g * effectiveAlpha + bgG * invA);
                    b = FAST_DIV255(b * effectiveAlpha + bgB * invA);
                    WriteRGB565A8(dstPx, r, g, b, AlphaUnion(effectiveAlpha, bgA));
                }
            }
        }
        return;
    }

    // ── Scaled path ──
    const uint32_t xStep = ((uint32_t)srcW << 16) / (uint32_t)destWidth;
    const uint32_t yStep = ((uint32_t)srcH << 16) / (uint32_t)destHeight;

    for (int32_t py = bounds.startY; py < bounds.endY; py++)
    {
        int32_t srcY = ((uint32_t)(py - destY) * yStep) >> 16;
        const uint8_t* srcRow = srcPixels + srcY * srcStride;
        uint8_t* dstRow = target + py * targetWidth * 3;

        uint32_t srcX_acc = (uint32_t)(bounds.startX - destX) * xStep;
        for (int32_t px = bounds.startX; px < bounds.endX; px++)
        {
            int32_t srcX = srcX_acc >> 16;
            srcX_acc += xStep;
            const uint8_t* srcPx = srcRow + srcX * bpp;

            uint8_t r, g, b, sa;
            UnpackRGB565A8(srcPx, r, g, b, sa);
            if (source.hasAlpha && sa == 0) continue;
            if (!source.hasAlpha) sa = 255;

            uint8_t effectiveAlpha = hasAlphaTint ? FAST_DIV255((uint16_t)sa * (uint16_t)tintA) : sa;
            if (effectiveAlpha == 0) continue;

            if (hasKey && r == keyR && g == keyG && b == keyB) continue;

            if (hasTint) { r = FAST_DIV255(r * tintR); g = FAST_DIV255(g * tintG); b = FAST_DIV255(b * tintB); }

            uint8_t* dstPx = dstRow + px * 3;
            if (effectiveAlpha == 255)
            {
                WriteRGB565A8(dstPx, r, g, b, 0xFF);
            }
            else
            {
                uint8_t bgR, bgG, bgB, bgA;
                UnpackRGB565A8(dstPx, bgR, bgG, bgB, bgA);
                uint8_t invA = (uint8_t)(255 - effectiveAlpha);
                r = FAST_DIV255(r * effectiveAlpha + bgR * invA);
                g = FAST_DIV255(g * effectiveAlpha + bgG * invA);
                b = FAST_DIV255(b * effectiveAlpha + bgB * invA);
                WriteRGB565A8(dstPx, r, g, b, AlphaUnion(effectiveAlpha, bgA));
            }
        }
    }
}

// ============================================================================
// Specialized BlitScaled: RGBA8888 source → RGB565A8 target
// ============================================================================

static DEKI_FAST_ATTR void BlitScaled_RGBA8888_to_RGB565A8(
    const Source& source, uint8_t* target, int32_t targetWidth,
    int32_t destX, int32_t destY, int32_t destWidth, int32_t destHeight,
    const BlitBounds& bounds, bool hasTint, bool hasAlphaTint,
    uint8_t tintR, uint8_t tintG, uint8_t tintB, uint8_t tintA)
{
    const uint8_t* srcPixels = source.pixels;
    int32_t srcW = source.width;
    int32_t srcH = source.height;
    const int32_t srcStride = SourceStride(source);
    const bool    hasKey = source.hasChromaKey;
    const uint8_t keyR = source.keyR, keyG = source.keyG, keyB = source.keyB;

    const uint32_t xStep = ((uint32_t)srcW << 16) / (uint32_t)destWidth;
    const uint32_t yStep = ((uint32_t)srcH << 16) / (uint32_t)destHeight;

    for (int32_t py = bounds.startY; py < bounds.endY; py++)
    {
        int32_t srcY = ((uint32_t)(py - destY) * yStep) >> 16;
        const uint8_t* srcRow = srcPixels + srcY * srcStride;
        uint8_t* dstRow = target + py * targetWidth * 3;

        uint32_t srcX_acc = (uint32_t)(bounds.startX - destX) * xStep;
        for (int32_t px = bounds.startX; px < bounds.endX; px++)
        {
            int32_t srcX = srcX_acc >> 16;
            srcX_acc += xStep;
            const uint8_t* pixel = srcRow + srcX * 4;

            uint8_t a = pixel[3];
            if (a == 0) continue;

            uint8_t r = pixel[0];
            uint8_t g = pixel[1];
            uint8_t b = pixel[2];

            if (hasKey && r == keyR && g == keyG && b == keyB) continue;

            if (hasTint)
            {
                r = FAST_DIV255(r * tintR);
                g = FAST_DIV255(g * tintG);
                b = FAST_DIV255(b * tintB);
            }

            uint8_t effectiveAlpha = hasAlphaTint ? FAST_DIV255((uint16_t)a * (uint16_t)tintA) : a;
            if (effectiveAlpha == 0) continue;

            uint8_t* dstPx = dstRow + px * 3;
            if (effectiveAlpha == 255)
            {
                WriteRGB565A8(dstPx, r, g, b, 0xFF);
            }
            else
            {
                uint8_t bgR, bgG, bgB, bgA;
                UnpackRGB565A8(dstPx, bgR, bgG, bgB, bgA);
                uint8_t invA = (uint8_t)(255 - effectiveAlpha);
                r = FAST_DIV255(r * effectiveAlpha + bgR * invA);
                g = FAST_DIV255(g * effectiveAlpha + bgG * invA);
                b = FAST_DIV255(b * effectiveAlpha + bgB * invA);
                WriteRGB565A8(dstPx, r, g, b, AlphaUnion(effectiveAlpha, bgA));
            }
        }
    }
}

// Forward declarations: helpers below are used by the generic dither path.
// Definitions remain alongside the rotation path further down.
static inline void ExtractSourcePixel(const Source& source, int32_t x, int32_t y,
                                       uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a);
static inline void WriteTargetPixel(uint8_t* target, int32_t x, int32_t y,
                                     int32_t width, DekiColorFormat format,
                                     uint8_t r, uint8_t g, uint8_t b);

// ============================================================================
// Ordered-dither alpha (8×8 Bayer matrix, https://en.wikipedia.org/wiki/Ordered_dithering)
// ============================================================================
//
// Replaces per-pixel alpha blend math with a screen-space threshold compare.
// Per pixel: if effectiveAlpha > BayerThreshold(px, py), write the source RGB
// opaquely; otherwise skip. No destination read, no multiply-add — much
// faster on PSRAM-bound targets. Trade-off: a fixed stippling pattern at any
// non-fully-opaque alpha. Solid (a==0 or a==255) pixels are written normally.

// 8×8 Bayer threshold matrix scaled to 0..255. Standard recurrent definition:
//   M_n[i][j] = ((Mn-1[i mod n/2][j mod n/2]) * 4) +
//               (((2 * (i / (n/2)) + (j / (n/2))) bit-reversed)),
// then re-mapped to (m + 1) * 256 / N - 1 for an N-cell matrix (here N=64).
static constexpr uint8_t BAYER_8X8[64] = {
      0, 128,  32, 160,   8, 136,  40, 168,
    192,  64, 224,  96, 200,  72, 232, 104,
     48, 176,  16, 144,  56, 184,  24, 152,
    240, 112, 208,  80, 248, 120, 216,  88,
     12, 140,  44, 172,   4, 132,  36, 164,
    204,  76, 236, 108, 196,  68, 228, 100,
     60, 188,  28, 156,  52, 180,  20, 148,
    252, 124, 220,  92, 244, 116, 212,  84,
};

static inline uint8_t BayerThreshold(int32_t px, int32_t py)
{
    return BAYER_8X8[((py & 7) << 3) | (px & 7)];
}

// ============================================================================
// Specialized dither: RGB565A8 source → RGB565 target
// ============================================================================
// Hot path — sprite blits onto the framebuffer on RGB565 displays.

static DEKI_FAST_ATTR void BlitScaled_RGB565A8_to_RGB565_Dither(
    const Source& source, uint16_t* target16, int32_t targetWidth,
    int32_t destX, int32_t destY, int32_t destWidth, int32_t destHeight,
    const BlitBounds& bounds, bool hasTint, bool hasAlphaTint,
    uint8_t tintR, uint8_t tintG, uint8_t tintB, uint8_t tintA)
{
    const uint8_t* srcPixels = source.pixels;
    int32_t srcW = source.width;
    int32_t srcH = source.height;
    const int32_t bpp = source.bytesPerPixel;
    const int32_t srcStride = SourceStride(source);
    const bool    hasKey = source.hasChromaKey;
    const uint8_t keyR = source.keyR, keyG = source.keyG, keyB = source.keyB;

    // Tight inner loop: read RGB565+alpha, threshold-compare, opaque-write or skip.
    auto runRow = [&](int32_t py, const uint8_t* srcRow, uint16_t* dstRow,
                       auto getSrcX) {
        for (int32_t px = bounds.startX; px < bounds.endX; px++)
        {
            int32_t srcX = getSrcX(px);
            const uint8_t* srcPx = srcRow + srcX * bpp;
            uint8_t a = srcPx[2];
            if (a == 0) continue;

            uint8_t effA = hasAlphaTint ? FAST_DIV255((uint16_t)a * (uint16_t)tintA) : a;
            if (effA == 0) continue;

            uint16_t srcRGB = *(const uint16_t*)srcPx;

            if (hasKey)
            {
                uint8_t kr = (srcRGB >> 11) << 3;
                uint8_t kg = ((srcRGB >> 5) & 0x3F) << 2;
                uint8_t kb = (srcRGB & 0x1F) << 3;
                if (kr == keyR && kg == keyG && kb == keyB) continue;
            }

            // Threshold compare. effA == 255 always passes (255 > any 0..252).
            // effA <= 0 already short-circuited above.
            if (effA <= BayerThreshold(px, py)) continue;

            if (hasTint)
            {
                uint8_t r = (srcRGB >> 11) << 3;
                uint8_t g = ((srcRGB >> 5) & 0x3F) << 2;
                uint8_t b = (srcRGB & 0x1F) << 3;
                r = FAST_DIV255(r * tintR);
                g = FAST_DIV255(g * tintG);
                b = FAST_DIV255(b * tintB);
                dstRow[px] = ((r >> 3 << 11) | ((g >> 2) << 5) | (b >> 3));
            }
            else
            {
                dstRow[px] = srcRGB;
            }
        }
    };

    if (destWidth == srcW && destHeight == srcH)
    {
        // 1:1 fast path — sequential source pointer.
        for (int32_t py = bounds.startY; py < bounds.endY; py++)
        {
            const uint8_t* srcRow = srcPixels + (py - destY) * srcStride;
            uint16_t* dstRow = target16 + py * targetWidth;
            runRow(py, srcRow, dstRow, [&](int32_t px) { return px - destX; });
        }
        return;
    }

    // Scaled path — 16.16 fixed-point step.
    const uint32_t xStep = ((uint32_t)srcW << 16) / (uint32_t)destWidth;
    const uint32_t yStep = ((uint32_t)srcH << 16) / (uint32_t)destHeight;
    for (int32_t py = bounds.startY; py < bounds.endY; py++)
    {
        int32_t srcY = ((uint32_t)(py - destY) * yStep) >> 16;
        const uint8_t* srcRow = srcPixels + srcY * srcStride;
        uint16_t* dstRow = target16 + py * targetWidth;
        runRow(py, srcRow, dstRow, [&](int32_t px) {
            return (int32_t)(((uint32_t)(px - destX) * xStep) >> 16);
        });
    }
}

// ============================================================================
// Generic dither path — covers all source × target format combinations
// ============================================================================
// Slower than specialized paths (per-pixel format dispatch via Extract /
// Write helpers, same approach as the rotation path). Used when no specialized
// dither path matches the source/target pair, so every dither blit lands
// somewhere — no silent fallback to alpha blend.

static DEKI_FAST_ATTR void BlitScaledDithered_Generic(
    const Source& source, uint8_t* target, int32_t targetWidth, int32_t /*targetHeight*/,
    DekiColorFormat targetFormat,
    int32_t destX, int32_t destY, int32_t destWidth, int32_t destHeight,
    const BlitBounds& bounds, bool hasTint, bool hasAlphaTint,
    uint8_t tintR, uint8_t tintG, uint8_t tintB, uint8_t tintA)
{
    int32_t srcW = source.width;
    int32_t srcH = source.height;
    const bool    hasKey = source.hasChromaKey;
    const uint8_t keyR = source.keyR, keyG = source.keyG, keyB = source.keyB;

    const uint32_t xStep = (destWidth == srcW)  ? 0 : ((uint32_t)srcW << 16) / (uint32_t)destWidth;
    const uint32_t yStep = (destHeight == srcH) ? 0 : ((uint32_t)srcH << 16) / (uint32_t)destHeight;

    for (int32_t py = bounds.startY; py < bounds.endY; py++)
    {
        int32_t srcY = (yStep == 0) ? (py - destY)
                                    : (int32_t)(((uint32_t)(py - destY) * yStep) >> 16);

        for (int32_t px = bounds.startX; px < bounds.endX; px++)
        {
            int32_t srcX = (xStep == 0) ? (px - destX)
                                        : (int32_t)(((uint32_t)(px - destX) * xStep) >> 16);

            uint8_t r, g, b, a;
            ExtractSourcePixel(source, srcX, srcY, r, g, b, a);
            if (a == 0) continue;

            if (hasKey && r == keyR && g == keyG && b == keyB) continue;

            if (hasTint)
            {
                r = FAST_DIV255(r * tintR);
                g = FAST_DIV255(g * tintG);
                b = FAST_DIV255(b * tintB);
            }

            uint8_t effA = hasAlphaTint ? FAST_DIV255((uint16_t)a * (uint16_t)tintA) : a;
            if (effA == 0) continue;
            if (effA <= BayerThreshold(px, py)) continue;

            WriteTargetPixel(target, px, py, targetWidth, targetFormat, r, g, b);
        }
    }
}

// ============================================================================
// BlitScaled dispatcher — selects specialized path based on formats
// ============================================================================

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

    bool hasTint = (tintR != 255 || tintG != 255 || tintB != 255);
    bool hasAlphaTint = (tintA != 255);

    // Ordered-dither path: only meaningful when the source has per-pixel alpha
    // (otherwise no partial-alpha pixels exist to dither). Falls through to the
    // standard alpha-blend dispatch when source is opaque RGB565 or hasAlpha=false.
    if (useOrderedDither && source.hasAlpha)
    {
        // Hot specialization: RGB565A8 → RGB565 (the typical sprite-to-framebuffer
        // case on embedded displays). All other (src, dst) combinations route to
        // the generic ExtractSourcePixel/WriteTargetPixel path.
        if (targetFormat == DekiColorFormat::RGB565
            && source.isRGB565
            && source.bytesPerPixel >= 3)
        {
            BlitScaled_RGB565A8_to_RGB565_Dither(
                source, (uint16_t*)target, targetWidth,
                destX, destY, destWidth, destHeight,
                bounds, hasTint, hasAlphaTint, tintR, tintG, tintB, tintA);
        }
        else
        {
            BlitScaledDithered_Generic(
                source, target, targetWidth, targetHeight, targetFormat,
                destX, destY, destWidth, destHeight,
                bounds, hasTint, hasAlphaTint, tintR, tintG, tintB, tintA);
        }
        return;
    }

    switch (targetFormat)
    {
        case DekiColorFormat::RGB565:
        {
            uint16_t* target16 = (uint16_t*)target;
            if (source.isRGB565)
            {
                // Dispatch by data layout (bytesPerPixel), not hasAlpha flag.
                // RGB565A8 format stores 3 bytes per pixel even when all alpha=255.
                if (source.bytesPerPixel >= 3)
                    BlitScaled_RGB565A8_to_RGB565(source, target16, targetWidth, destX, destY, destWidth, destHeight, bounds, hasTint, hasAlphaTint, tintR, tintG, tintB, tintA);
                else
                    BlitScaled_RGB565_to_RGB565(source, target16, targetWidth, destX, destY, destWidth, destHeight, bounds, hasTint, hasAlphaTint, tintR, tintG, tintB, tintA);
            }
            else
            {
                // RGBA8888 (TextComponent) or any 4bpp source
                BlitScaled_RGBA8888_to_RGB565(source, target16, targetWidth, destX, destY, destWidth, destHeight, bounds, hasTint, hasAlphaTint, tintR, tintG, tintB, tintA);
            }
            break;
        }
        case DekiColorFormat::ARGB8888:
        {
            uint32_t* target32 = (uint32_t*)target;
            if (source.isRGB565)
            {
                if (source.bytesPerPixel >= 3)
                    BlitScaled_RGB565A8_to_ARGB8888(source, target32, targetWidth, destX, destY, destWidth, destHeight, bounds, hasTint, hasAlphaTint, tintR, tintG, tintB, tintA);
                else
                    BlitScaled_RGB565_to_ARGB8888(source, target32, targetWidth, destX, destY, destWidth, destHeight, bounds, hasTint, hasAlphaTint, tintR, tintG, tintB, tintA);
            }
            else
            {
                BlitScaled_RGBA8888_to_ARGB8888(source, target32, targetWidth, destX, destY, destWidth, destHeight, bounds, hasTint, hasAlphaTint, tintR, tintG, tintB, tintA);
            }
            break;
        }
        case DekiColorFormat::RGB888:
        {
            if (source.isRGB565)
            {
                if (source.bytesPerPixel >= 3)
                    BlitScaled_RGB565A8_to_RGB888(source, target, targetWidth, destX, destY, destWidth, destHeight, bounds, hasTint, hasAlphaTint, tintR, tintG, tintB, tintA);
                else
                    BlitScaled_RGB565_to_RGB888(source, target, targetWidth, destX, destY, destWidth, destHeight, bounds, hasTint, hasAlphaTint, tintR, tintG, tintB, tintA);
            }
            else
            {
                BlitScaled_RGBA8888_to_RGB888(source, target, targetWidth, destX, destY, destWidth, destHeight, bounds, hasTint, hasAlphaTint, tintR, tintG, tintB, tintA);
            }
            break;
        }
        case DekiColorFormat::RGB565A8:
        {
            if (source.isRGB565)
            {
                if (source.bytesPerPixel >= 3)
                    BlitScaled_RGB565A8_to_RGB565A8(source, target, targetWidth, destX, destY, destWidth, destHeight, bounds, hasTint, hasAlphaTint, tintR, tintG, tintB, tintA);
                else
                    BlitScaled_RGB565_to_RGB565A8(source, target, targetWidth, destX, destY, destWidth, destHeight, bounds, hasTint, hasAlphaTint, tintR, tintG, tintB, tintA);
            }
            else
            {
                BlitScaled_RGBA8888_to_RGB565A8(source, target, targetWidth, destX, destY, destWidth, destHeight, bounds, hasTint, hasAlphaTint, tintR, tintG, tintB, tintA);
            }
            break;
        }
    }
}

// ============================================================================
// Blit with rotation — uses ExtractSourcePixel for flexibility
// ============================================================================

// Helper: Extract RGB from source pixel (used only in rotation path)
static inline void ExtractSourcePixel(const Source& source, int32_t x, int32_t y,
                                       uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a)
{
    const int32_t stride = SourceStride(source);
    const uint8_t* pixel = source.pixels + y * stride + x * source.bytesPerPixel;

    if (source.isRGB565)
    {
        uint16_t rgb565 = *(const uint16_t*)pixel;
        r = (rgb565 >> 11) << 3;
        g = ((rgb565 >> 5) & 0x3F) << 2;
        b = (rgb565 & 0x1F) << 3;
        a = source.hasAlpha ? pixel[2] : 255;
    }
    else if (source.bytesPerPixel == 4)
    {
        r = pixel[0];
        g = pixel[1];
        b = pixel[2];
        a = pixel[3];
    }
    else if (source.bytesPerPixel == 3)
    {
        r = pixel[0];
        g = pixel[1];
        b = pixel[2];
        a = 255;
    }
    else
    {
        r = g = b = 0;
        a = 255;
    }
}

// Helper: Write pixel to target buffer (used only in rotation path)
static inline void WriteTargetPixel(uint8_t* target, int32_t x, int32_t y,
                                     int32_t width, DekiColorFormat format,
                                     uint8_t r, uint8_t g, uint8_t b)
{

    switch (format)
    {
        case DekiColorFormat::RGB565:
        {
            uint16_t* buf16 = (uint16_t*)target;
            buf16[y * width + x] = ((r >> 3 << 11) | ((g >> 2) << 5) | (b >> 3));
            break;
        }
        case DekiColorFormat::RGB888:
        {
            size_t idx = (y * width + x) * 3;
            target[idx] = r;
            target[idx + 1] = g;
            target[idx + 2] = b;
            break;
        }
        case DekiColorFormat::ARGB8888:
        {
            uint32_t* buf32 = (uint32_t*)target;
            buf32[y * width + x] = (0xFF << 24) | (r << 16) | (g << 8) | b;
            break;
        }
        case DekiColorFormat::RGB565A8:
        {
            // Rotation path is the alpha-blend output stage; whatever blending
            // happened produced opaque RGB. Mark covered pixels with alpha=0xFF.
            uint16_t rgb565 = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            size_t idx = (size_t)(y * width + x) * 3;
            target[idx]     = (uint8_t)(rgb565 & 0xFF);
            target[idx + 1] = (uint8_t)((rgb565 >> 8) & 0xFF);
            target[idx + 2] = 0xFF;
            break;
        }
    }
}

// Helper: Read background pixel from target buffer (used only in rotation path)
static inline void GetTargetPixel(const uint8_t* target, int32_t x, int32_t y,
                                   int32_t width, DekiColorFormat format,
                                   uint8_t& r, uint8_t& g, uint8_t& b)
{
    switch (format)
    {
        case DekiColorFormat::RGB565:
        {
            uint16_t pixel = ((const uint16_t*)target)[y * width + x];
            r = (pixel >> 11) << 3;
            g = ((pixel >> 5) & 0x3F) << 2;
            b = (pixel & 0x1F) << 3;
            break;
        }
        case DekiColorFormat::RGB888:
        {
            size_t idx = (y * width + x) * 3;
            r = target[idx];
            g = target[idx + 1];
            b = target[idx + 2];
            break;
        }
        case DekiColorFormat::ARGB8888:
        {
            uint32_t pixel = ((const uint32_t*)target)[y * width + x];
            r = (pixel >> 16) & 0xFF;
            g = (pixel >> 8) & 0xFF;
            b = pixel & 0xFF;
            break;
        }
        case DekiColorFormat::RGB565A8:
        {
            size_t idx = (size_t)(y * width + x) * 3;
            uint16_t pixel = (uint16_t)target[idx] | ((uint16_t)target[idx + 1] << 8);
            r = (uint8_t)((pixel >> 11) << 3);
            g = (uint8_t)(((pixel >> 5) & 0x3F) << 2);
            b = (uint8_t)((pixel & 0x1F) << 3);
            break;
        }
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

    // Fast path: no rotation — use specialized BlitScaled
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

    bool hasTint = (tintR != 255 || tintG != 255 || tintB != 255);
    bool hasAlphaTint = (tintA != 255);

    float cosNegR = cosR;
    float sinNegR = -sinR;

    for (int32_t py = startY; py < endY; py++)
    {
        for (int32_t px = startX; px < endX; px++)
        {
            float dx = static_cast<float>(px - screenX);
            float dy = static_cast<float>(py - screenY);

            float localX = dx * cosNegR - dy * sinNegR + pivotSX;
            float localY = dx * sinNegR + dy * cosNegR + pivotSY;

            if (localX < 0 || localX >= destWidth || localY < 0 || localY >= destHeight)
                continue;

            int32_t srcX = static_cast<int32_t>(localX * source.width / destWidth);
            int32_t srcY = static_cast<int32_t>(localY * source.height / destHeight);

            if (srcX < 0 || srcX >= source.width || srcY < 0 || srcY >= source.height)
                continue;

            uint8_t r, g, b, a;
            ExtractSourcePixel(source, srcX, srcY, r, g, b, a);

            if (source.hasChromaKey &&
                r == source.keyR && g == source.keyG && b == source.keyB)
                continue;

            uint8_t effectiveAlpha = hasAlphaTint ? FAST_DIV255(a * tintA) : a;
            if (effectiveAlpha == 0)
                continue;

            if (useOrderedDither && source.hasAlpha)
            {
                // Threshold compare; no destination read, no per-channel blend.
                if (effectiveAlpha <= BayerThreshold(px, py))
                    continue;
            }
            else if (effectiveAlpha < 255)
            {
                uint8_t bgR, bgG, bgB;
                GetTargetPixel(target, px, py, targetWidth, targetFormat, bgR, bgG, bgB);
                uint8_t invA = 255 - effectiveAlpha;
                r = FAST_DIV255(r * effectiveAlpha + bgR * invA);
                g = FAST_DIV255(g * effectiveAlpha + bgG * invA);
                b = FAST_DIV255(b * effectiveAlpha + bgB * invA);
            }

            if (hasTint)
            {
                r = FAST_DIV255(r * tintR);
                g = FAST_DIV255(g * tintG);
                b = FAST_DIV255(b * tintB);
            }

            WriteTargetPixel(target, px, py, targetWidth, targetFormat, r, g, b);
        }
    }
}

} // namespace QuadBlit
