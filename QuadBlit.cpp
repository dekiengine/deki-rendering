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
    ARGB8888 = 2
};
#endif

// Fast alpha blend approximation: (a * b + 128) >> 8 instead of (a * b) / 255
#define FAST_DIV255(x) (((x) + 128) >> 8)

namespace QuadBlit
{

// ============================================================================
// Byte-swap for display-native RGB565 output
// ============================================================================

static bool s_ByteSwap = false;

void SetByteSwap(bool enabled) { s_ByteSwap = enabled; }
bool GetByteSwap() { return s_ByteSwap; }

// Swap bytes of RGB565 value for display controller endianness
static inline uint16_t SwapRGB565(uint16_t v)
{
    return (v >> 8) | (v << 8);
}

// Compile-time elimination: editor never byte-swaps
// Device builds cache s_ByteSwap in a local const at function entry
#ifdef DEKI_EDITOR
#define CACHE_BYTE_SWAP
static inline uint16_t ToDisplay565(uint16_t v) { return v; }
static inline uint16_t FromDisplay565(uint16_t v) { return v; }
#else
#define CACHE_BYTE_SWAP const bool _doSwap = s_ByteSwap;
static inline uint16_t ToDisplay565_impl(uint16_t v, bool doSwap)
{
    return doSwap ? SwapRGB565(v) : v;
}
static inline uint16_t FromDisplay565_impl(uint16_t v, bool doSwap)
{
    return doSwap ? SwapRGB565(v) : v;
}
#define ToDisplay565(v) ToDisplay565_impl((v), _doSwap)
#define FromDisplay565(v) FromDisplay565_impl((v), _doSwap)
#endif

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
                  bool ownsPixels)
{
    Source src;
    src.pixels = pixels;
    src.width = width;
    src.height = height;
    src.bytesPerPixel = bytesPerPixel;
    src.hasAlpha = hasAlpha;
    src.isRGB565 = isRGB565;
    src.ownsPixels = ownsPixels;

    if (hasAlpha)
        src.alphaOffset = isRGB565 ? 2 : 3;
    else
        src.alphaOffset = 0;

    return src;
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
    CACHE_BYTE_SWAP
    const uint8_t* srcPixels = source.pixels;
    int32_t srcW = source.width;
    int32_t srcH = source.height;
    const int32_t srcStride = srcW * 4;

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

                if (hasTint) { r = FAST_DIV255(r * tintR); g = FAST_DIV255(g * tintG); b = FAST_DIV255(b * tintB); }

                uint8_t effectiveAlpha = hasAlphaTint ? FAST_DIV255(a * tintA) : a;
                if (effectiveAlpha == 0) continue;

                if (effectiveAlpha == 255)
                {
                    dstRow[px] = ToDisplay565(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                }
                else
                {
                    uint16_t bg = FromDisplay565(dstRow[px]);
                    uint8_t bgR = (bg >> 11) << 3;
                    uint8_t bgG = ((bg >> 5) & 0x3F) << 2;
                    uint8_t bgB = (bg & 0x1F) << 3;
                    uint8_t invA = 255 - effectiveAlpha;
                    r = FAST_DIV255(r * effectiveAlpha + bgR * invA);
                    g = FAST_DIV255(g * effectiveAlpha + bgG * invA);
                    b = FAST_DIV255(b * effectiveAlpha + bgB * invA);
                    dstRow[px] = ToDisplay565(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
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
                dstRow[px] = ToDisplay565(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            }
            else
            {
                uint16_t bg = FromDisplay565(dstRow[px]);
                uint8_t bgR = (bg >> 11) << 3;
                uint8_t bgG = ((bg >> 5) & 0x3F) << 2;
                uint8_t bgB = (bg & 0x1F) << 3;

                uint8_t invA = 255 - effectiveAlpha;
                r = FAST_DIV255(r * effectiveAlpha + bgR * invA);
                g = FAST_DIV255(g * effectiveAlpha + bgG * invA);
                b = FAST_DIV255(b * effectiveAlpha + bgB * invA);
                dstRow[px] = ToDisplay565(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
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
    CACHE_BYTE_SWAP
    const uint8_t* srcPixels = source.pixels;
    int32_t srcW = source.width;
    int32_t srcH = source.height;
    int32_t bpp = source.bytesPerPixel;
    const int32_t srcStride = srcW * bpp;

    // ── 1:1 scale fast path — sequential pointer access, best PSRAM cache use ──
    if (destWidth == srcW && destHeight == srcH)
    {
        if (!source.hasAlpha)
        {
            if (!hasTint && !hasAlphaTint)
            {
                for (int32_t py = bounds.startY; py < bounds.endY; py++)
                {
                    const uint8_t* srcPtr = srcPixels + (py - destY) * srcStride + (bounds.startX - destX) * bpp;
                    uint16_t* dstRow = target16 + py * targetWidth;
                    for (int32_t px = bounds.startX; px < bounds.endX; px++)
                    {
                        dstRow[px] = ToDisplay565(*(const uint16_t*)srcPtr);
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
                        if (hasTint) { r = FAST_DIV255(r * tintR); g = FAST_DIV255(g * tintG); b = FAST_DIV255(b * tintB); }
                        if (effectiveA == 255)
                        {
                            dstRow[px] = ToDisplay565(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                        }
                        else
                        {
                            uint16_t bg = FromDisplay565(dstRow[px]);
                            uint8_t bgR = (bg >> 11) << 3;
                            uint8_t bgG = ((bg >> 5) & 0x3F) << 2;
                            uint8_t bgB = (bg & 0x1F) << 3;
                            uint8_t invA = 255 - effectiveA;
                            r = FAST_DIV255(r * effectiveA + bgR * invA);
                            g = FAST_DIV255(g * effectiveA + bgG * invA);
                            b = FAST_DIV255(b * effectiveA + bgB * invA);
                            dstRow[px] = ToDisplay565(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                        }
                    }
                }
            }
        }
        else
        {
            // Alpha path at 1:1 — sequential pointer access
            for (int32_t py = bounds.startY; py < bounds.endY; py++)
            {
                const uint8_t* srcPtr = srcPixels + (py - destY) * srcStride + (bounds.startX - destX) * bpp;
                uint16_t* dstRow = target16 + py * targetWidth;
                for (int32_t px = bounds.startX; px < bounds.endX; px++)
                {
                    uint8_t a = srcPtr[2];
                    if (a == 0) { srcPtr += bpp; continue; }

                    uint8_t effectiveAlpha = hasAlphaTint ? FAST_DIV255(a * tintA) : a;
                    if (effectiveAlpha == 0) { srcPtr += bpp; continue; }

                    uint16_t srcRGB = *(const uint16_t*)srcPtr;
                    srcPtr += bpp;

                    if (!hasTint && effectiveAlpha == 255)
                    {
                        dstRow[px] = ToDisplay565(srcRGB);
                    }
                    else
                    {
                        uint8_t r = (srcRGB >> 11) << 3;
                        uint8_t g = ((srcRGB >> 5) & 0x3F) << 2;
                        uint8_t b = (srcRGB & 0x1F) << 3;
                        if (hasTint) { r = FAST_DIV255(r * tintR); g = FAST_DIV255(g * tintG); b = FAST_DIV255(b * tintB); }
                        if (effectiveAlpha == 255)
                        {
                            dstRow[px] = ToDisplay565(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                        }
                        else
                        {
                            uint16_t bg = FromDisplay565(dstRow[px]);
                            uint8_t bgR = (bg >> 11) << 3;
                            uint8_t bgG = ((bg >> 5) & 0x3F) << 2;
                            uint8_t bgB = (bg & 0x1F) << 3;
                            uint8_t invA = 255 - effectiveAlpha;
                            r = FAST_DIV255(r * effectiveAlpha + bgR * invA);
                            g = FAST_DIV255(g * effectiveAlpha + bgG * invA);
                            b = FAST_DIV255(b * effectiveAlpha + bgB * invA);
                            dstRow[px] = ToDisplay565(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
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
        if (!hasTint && !hasAlphaTint)
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
                    dstRow[px] = ToDisplay565(*(const uint16_t*)(srcRow + srcX * bpp));
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
                    if (hasTint) { r = FAST_DIV255(r * tintR); g = FAST_DIV255(g * tintG); b = FAST_DIV255(b * tintB); }
                    if (effectiveA == 255)
                    {
                        dstRow[px] = ToDisplay565(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                    }
                    else
                    {
                        uint16_t bg = FromDisplay565(dstRow[px]);
                        uint8_t bgR = (bg >> 11) << 3;
                        uint8_t bgG = ((bg >> 5) & 0x3F) << 2;
                        uint8_t bgB = (bg & 0x1F) << 3;
                        uint8_t invA = 255 - effectiveA;
                        r = FAST_DIV255(r * effectiveA + bgR * invA);
                        g = FAST_DIV255(g * effectiveA + bgG * invA);
                        b = FAST_DIV255(b * effectiveA + bgB * invA);
                        dstRow[px] = ToDisplay565(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
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

            if (!hasTint && effectiveAlpha == 255)
            {
                dstRow[px] = ToDisplay565(srcRGB);
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
                    dstRow[px] = ToDisplay565(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                }
                else
                {
                    uint16_t bg = FromDisplay565(dstRow[px]);
                    uint8_t bgR = (bg >> 11) << 3;
                    uint8_t bgG = ((bg >> 5) & 0x3F) << 2;
                    uint8_t bgB = (bg & 0x1F) << 3;

                    uint8_t invA = 255 - effectiveAlpha;
                    r = FAST_DIV255(r * effectiveAlpha + bgR * invA);
                    g = FAST_DIV255(g * effectiveAlpha + bgG * invA);
                    b = FAST_DIV255(b * effectiveAlpha + bgB * invA);
                    dstRow[px] = ToDisplay565(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
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
    CACHE_BYTE_SWAP
    const uint16_t* srcPixels = (const uint16_t*)source.pixels;
    int32_t srcW = source.width;
    int32_t srcH = source.height;

    // ── 1:1 scale fast path — sequential pointer access ──
    if (destWidth == srcW && destHeight == srcH)
    {
        if (!hasTint && !hasAlphaTint)
        {
            const int32_t rowPixels = bounds.endX - bounds.startX;
#ifndef DEKI_EDITOR
            if (!_doSwap)
            {
                // Source already in display byte order — bulk memcpy per row
                for (int32_t py = bounds.startY; py < bounds.endY; py++)
                {
                    const uint16_t* srcPtr = srcPixels + (py - destY) * srcW + (bounds.startX - destX);
                    uint16_t* dstPtr = target16 + py * targetWidth + bounds.startX;
                    memcpy(dstPtr, srcPtr, rowPixels * sizeof(uint16_t));
                }
            }
            else
#endif
            {
                for (int32_t py = bounds.startY; py < bounds.endY; py++)
                {
                    const uint16_t* srcPtr = srcPixels + (py - destY) * srcW + (bounds.startX - destX);
                    uint16_t* dstRow = target16 + py * targetWidth;
                    for (int32_t px = bounds.startX; px < bounds.endX; px++)
                    {
                        dstRow[px] = ToDisplay565(*srcPtr++);
                    }
                }
            }
        }
        else
        {
            for (int32_t py = bounds.startY; py < bounds.endY; py++)
            {
                const uint16_t* srcPtr = srcPixels + (py - destY) * srcW + (bounds.startX - destX);
                uint16_t* dstRow = target16 + py * targetWidth;
                for (int32_t px = bounds.startX; px < bounds.endX; px++)
                {
                    uint16_t srcRGB = *srcPtr++;
                    uint8_t r = (srcRGB >> 11) << 3;
                    uint8_t g = ((srcRGB >> 5) & 0x3F) << 2;
                    uint8_t b = (srcRGB & 0x1F) << 3;
                    if (hasTint) { r = FAST_DIV255(r * tintR); g = FAST_DIV255(g * tintG); b = FAST_DIV255(b * tintB); }
                    if (hasAlphaTint && tintA < 255)
                    {
                        uint16_t bg = FromDisplay565(dstRow[px]);
                        uint8_t bgR = (bg >> 11) << 3;
                        uint8_t bgG = ((bg >> 5) & 0x3F) << 2;
                        uint8_t bgB = (bg & 0x1F) << 3;
                        uint8_t invA = 255 - tintA;
                        r = FAST_DIV255(r * tintA + bgR * invA);
                        g = FAST_DIV255(g * tintA + bgG * invA);
                        b = FAST_DIV255(b * tintA + bgB * invA);
                    }
                    dstRow[px] = ToDisplay565(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                }
            }
        }
        return;
    }

    // ── Scaled path — fixed-point 16.16 ──
    const uint32_t xStep = ((uint32_t)srcW << 16) / (uint32_t)destWidth;
    const uint32_t yStep = ((uint32_t)srcH << 16) / (uint32_t)destHeight;

    if (!hasTint && !hasAlphaTint)
    {
        for (int32_t py = bounds.startY; py < bounds.endY; py++)
        {
            int32_t srcY = ((uint32_t)(py - destY) * yStep) >> 16;
            const uint16_t* srcRow = srcPixels + srcY * srcW;
            uint16_t* dstRow = target16 + py * targetWidth;

            uint32_t srcX_acc = (uint32_t)(bounds.startX - destX) * xStep;
            for (int32_t px = bounds.startX; px < bounds.endX; px++)
            {
                int32_t srcX = srcX_acc >> 16;
                srcX_acc += xStep;
                dstRow[px] = ToDisplay565(srcRow[srcX]);
            }
        }
        return;
    }

    for (int32_t py = bounds.startY; py < bounds.endY; py++)
    {
        int32_t srcY = ((uint32_t)(py - destY) * yStep) >> 16;
        const uint16_t* srcRow = srcPixels + srcY * srcW;
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

            if (hasTint)
            {
                r = FAST_DIV255(r * tintR);
                g = FAST_DIV255(g * tintG);
                b = FAST_DIV255(b * tintB);
            }

            if (hasAlphaTint && tintA < 255)
            {
                uint16_t bg = FromDisplay565(dstRow[px]);
                uint8_t bgR = (bg >> 11) << 3;
                uint8_t bgG = ((bg >> 5) & 0x3F) << 2;
                uint8_t bgB = (bg & 0x1F) << 3;

                uint8_t invA = 255 - tintA;
                r = FAST_DIV255(r * tintA + bgR * invA);
                g = FAST_DIV255(g * tintA + bgG * invA);
                b = FAST_DIV255(b * tintA + bgB * invA);
            }

            dstRow[px] = ToDisplay565(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
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

    const uint32_t xStep = ((uint32_t)srcW << 16) / (uint32_t)destWidth;
    const uint32_t yStep = ((uint32_t)srcH << 16) / (uint32_t)destHeight;

    for (int32_t py = bounds.startY; py < bounds.endY; py++)
    {
        int32_t srcY = ((uint32_t)(py - destY) * yStep) >> 16;
        uint32_t* dstRow = target32 + py * targetWidth;

        uint32_t srcX_acc = (uint32_t)(bounds.startX - destX) * xStep;
        for (int32_t px = bounds.startX; px < bounds.endX; px++)
        {
            int32_t srcX = srcX_acc >> 16;
            srcX_acc += xStep;
            const uint8_t* pixel = srcPixels + (srcY * srcW + srcX) * 4;

            uint8_t a = pixel[3];
            if (a == 0) continue;

            uint8_t r = pixel[0];
            uint8_t g = pixel[1];
            uint8_t b = pixel[2];

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

    const uint32_t xStep = ((uint32_t)srcW << 16) / (uint32_t)destWidth;
    const uint32_t yStep = ((uint32_t)srcH << 16) / (uint32_t)destHeight;

    for (int32_t py = bounds.startY; py < bounds.endY; py++)
    {
        int32_t srcY = ((uint32_t)(py - destY) * yStep) >> 16;
        uint32_t* dstRow = target32 + py * targetWidth;

        uint32_t srcX_acc = (uint32_t)(bounds.startX - destX) * xStep;
        for (int32_t px = bounds.startX; px < bounds.endX; px++)
        {
            int32_t srcX = srcX_acc >> 16;
            srcX_acc += xStep;
            size_t srcIdx = (srcY * srcW + srcX) * 3;

            uint8_t a = srcPixels[srcIdx + 2];
            if (a == 0) continue;

            uint16_t srcRGB = *(const uint16_t*)(srcPixels + srcIdx);
            uint8_t r = (srcRGB >> 11) << 3;
            uint8_t g = ((srcRGB >> 5) & 0x3F) << 2;
            uint8_t b = (srcRGB & 0x1F) << 3;

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
    const uint16_t* srcPixels = (const uint16_t*)source.pixels;
    int32_t srcW = source.width;
    int32_t srcH = source.height;

    const uint32_t xStep = ((uint32_t)srcW << 16) / (uint32_t)destWidth;
    const uint32_t yStep = ((uint32_t)srcH << 16) / (uint32_t)destHeight;

    for (int32_t py = bounds.startY; py < bounds.endY; py++)
    {
        int32_t srcY = ((uint32_t)(py - destY) * yStep) >> 16;
        const uint16_t* srcRow = srcPixels + srcY * srcW;
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

    const uint32_t xStep = ((uint32_t)srcW << 16) / (uint32_t)destWidth;
    const uint32_t yStep = ((uint32_t)srcH << 16) / (uint32_t)destHeight;

    for (int32_t py = bounds.startY; py < bounds.endY; py++)
    {
        int32_t srcY = ((uint32_t)(py - destY) * yStep) >> 16;

        uint32_t srcX_acc = (uint32_t)(bounds.startX - destX) * xStep;
        for (int32_t px = bounds.startX; px < bounds.endX; px++)
        {
            int32_t srcX = srcX_acc >> 16;
            srcX_acc += xStep;
            const uint8_t* pixel = srcPixels + (srcY * srcW + srcX) * 4;

            uint8_t a = pixel[3];
            if (a == 0) continue;

            uint8_t r = pixel[0];
            uint8_t g = pixel[1];
            uint8_t b = pixel[2];

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

    const uint32_t xStep = ((uint32_t)srcW << 16) / (uint32_t)destWidth;
    const uint32_t yStep = ((uint32_t)srcH << 16) / (uint32_t)destHeight;

    for (int32_t py = bounds.startY; py < bounds.endY; py++)
    {
        int32_t srcY = ((uint32_t)(py - destY) * yStep) >> 16;

        uint32_t srcX_acc = (uint32_t)(bounds.startX - destX) * xStep;
        for (int32_t px = bounds.startX; px < bounds.endX; px++)
        {
            int32_t srcX = srcX_acc >> 16;
            srcX_acc += xStep;
            size_t srcIdx = (srcY * srcW + srcX) * 3;

            uint8_t a = srcPixels[srcIdx + 2];
            if (a == 0) continue;

            uint16_t srcRGB = *(const uint16_t*)(srcPixels + srcIdx);
            uint8_t r = (srcRGB >> 11) << 3;
            uint8_t g = ((srcRGB >> 5) & 0x3F) << 2;
            uint8_t b = (srcRGB & 0x1F) << 3;

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
    const uint16_t* srcPixels = (const uint16_t*)source.pixels;
    int32_t srcW = source.width;
    int32_t srcH = source.height;

    const uint32_t xStep = ((uint32_t)srcW << 16) / (uint32_t)destWidth;
    const uint32_t yStep = ((uint32_t)srcH << 16) / (uint32_t)destHeight;

    for (int32_t py = bounds.startY; py < bounds.endY; py++)
    {
        int32_t srcY = ((uint32_t)(py - destY) * yStep) >> 16;
        const uint16_t* srcRow = srcPixels + srcY * srcW;

        uint32_t srcX_acc = (uint32_t)(bounds.startX - destX) * xStep;
        for (int32_t px = bounds.startX; px < bounds.endX; px++)
        {
            int32_t srcX = srcX_acc >> 16;
            srcX_acc += xStep;
            uint16_t srcRGB = srcRow[srcX];

            uint8_t r = (srcRGB >> 11) << 3;
            uint8_t g = ((srcRGB >> 5) & 0x3F) << 2;
            uint8_t b = (srcRGB & 0x1F) << 3;

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
                uint8_t tintA)
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
    }
}

// ============================================================================
// Blit with rotation — uses ExtractSourcePixel for flexibility
// ============================================================================

// Helper: Extract RGB from source pixel (used only in rotation path)
static inline void ExtractSourcePixel(const Source& source, int32_t x, int32_t y,
                                       uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a)
{
    size_t pixelIndex = y * source.width + x;
    size_t byteIndex = pixelIndex * source.bytesPerPixel;
    const uint8_t* pixel = source.pixels + byteIndex;

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
    CACHE_BYTE_SWAP
    switch (format)
    {
        case DekiColorFormat::RGB565:
        {
            uint16_t* buf16 = (uint16_t*)target;
            buf16[y * width + x] = ToDisplay565(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
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
          uint8_t tintA)
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
                   tintR, tintG, tintB, tintA);
        return;
    }

    // Rotated path
    const float PI = 3.14159265358979323846f;
    float radians = rotation * PI / 180.0f;
    float cosR = std::cos(radians);
    float sinR = std::sin(radians);

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

            uint8_t effectiveAlpha = hasAlphaTint ? FAST_DIV255(a * tintA) : a;
            if (effectiveAlpha == 0)
                continue;

            if (effectiveAlpha < 255)
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
