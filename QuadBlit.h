#pragma once
#include <cstdint>
#include <deki/assets/Texture2D.h>  // TextureFormat, for PixelLayout::FromTexture

namespace Deki
{
enum class ColorFormat;
}
namespace DekiRendering
{
class DirtyRegion;
}

/// Quad blitting with full 2D transforms: copies source pixels to a target
/// buffer with position, scale, rotation and alpha blending. Renderers can
/// produce raw pixels at 1x and leave transforms to QuadBlit.
namespace QuadBlit
{
/// A rectangle that output is restricted to.
struct ClipRect
{
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = INT32_MAX;
    int32_t bottom = INT32_MAX;

    bool IsSet() const { return right != INT32_MAX; }
};

/// Pushes a clip rect, intersected with the current one.
void PushClipRect(int32_t left, int32_t top, int32_t right, int32_t bottom);

void PopClipRect();

ClipRect GetCurrentClipRect();

/// Empties the clip stack. Call at frame start.
void ClearClipStack();

/// While false, blits ignore the clip stack.
void SetClipEnabled(bool enabled);

bool IsClipEnabled();

/// Current clip stack depth, for diagnostics.
int GetClipStackDepth();

/// Dirty-rect tracking. While set, every blit into `trackedTarget` adds its
/// clipped destination rectangle to `region`; blits into any other buffer (a
/// particle composite, a pass's scratch target) are ignored.
/// Standard2DRenderer sets it for the frame when RenderContext::trackDirty is
/// on and clears it afterwards. Pass nullptr to stop.
void SetDirtyTracking(DekiRendering::DirtyRegion* region, const uint8_t* trackedTarget);

/// Adds a rectangle to the tracked region, for code that writes the tracked
/// target without a blit. Does nothing when nothing is tracked.
void MarkDirty(int32_t left, int32_t top, int32_t right, int32_t bottom);

/// Marks the whole target changed (a whole-frame composite written directly).
void MarkAllDirty();

/// The buffer currently tracked, or nullptr.
const uint8_t* GetDirtyTrackedTarget();

/// How a source buffer stores one pixel.
///
/// QuadBlit takes the shape of a source, not a format enum: it lives in
/// deki-rendering and cannot name deki-2d's Deki::Texture2D::TextureFormat,
/// and a blitter only needs to know how to read the bytes.
///
/// It is a struct rather than three loose arguments, because swapping
/// hasAlpha and isRGB565 in a call would compile and blit wrong.
///
/// The named layouts are the shapes real sources come in. For any other
/// combination (a test, say), write the aggregate with designated
/// initialisers, which names the fields at the call site.
struct PixelLayout
{
    int32_t bytesPerPixel;
    bool hasAlpha;
    bool isRGB565;

    /// 16-bit colour, no alpha. Baked gradients, opaque sprites.
    static constexpr PixelLayout RGB565() { return { 2, false, true }; }

    /// 16-bit colour plus a separate alpha byte. Sprites with alpha, baked text.
    static constexpr PixelLayout RGB565A8() { return { 3, true, true }; }

    /// The RGB565A8 byte shape with its alpha ignored.
    static constexpr PixelLayout RGB565A8NoAlpha() { return { 3, false, true }; }

    /// 32-bit colour with alpha.
    static constexpr PixelLayout RGBA8888() { return { 4, true, false }; }

    /// 24-bit colour, no alpha.
    static constexpr PixelLayout RGB888() { return { 3, false, false }; }

    /// A single alpha byte per pixel. Font atlases.
    static constexpr PixelLayout Alpha8() { return { 1, true, false }; }

    /// The layout of a texture asset stored in `format`. `hasAlpha` is
    /// separate because it belongs to the asset, not the format: a sprite
    /// stored as RGB565A8 may still declare itself opaque, and then its alpha
    /// plane is ignored.
    static PixelLayout FromTexture(Deki::Texture2D::TextureFormat format, bool hasAlpha)
    {
        const bool isRGB565 =
            (format == Deki::Texture2D::TextureFormat::RGB565 || format == Deki::Texture2D::TextureFormat::RGB565A8);
        return { static_cast<int32_t>(Deki::Texture2D::GetBytesPerPixel(format)), hasAlpha, isRGB565 };
    }
};

/// A source buffer.
struct Source
{
    const uint8_t* pixels;
    int32_t width;          // in pixels
    int32_t height;         // in pixels
    int32_t bytesPerPixel;  // 2, 3 or 4
    bool hasAlpha;
    uint8_t alphaOffset;  // byte offset of alpha: 2 for RGB565A8, 3 for RGBA8888

    bool isRGB565;  // RGB565 or RGB565A8

    // Optional per-row opaque spans, nullptr if none: packed int16_t pairs
    // [opaqueStart, opaqueEnd] per row.
    const int16_t* alphaRowSpans;

    // False (the default): the component keeps its buffer, as every component
    // in the tree does (sprites, baked text, baked gradients and the particle
    // composite all reuse one across frames).
    //
    // True hands the buffer to the renderer, which frees it through
    // Deki::Memory after the blit, so it must have come from Deki::Memory
    // (normally Deki::Buffer<T>::Release()).
    bool ownsPixels;

    // Bytes per row in source memory. 0 means tightly packed
    // (width * bytesPerPixel). Set it to point Source at a sub-rect of a
    // larger buffer (e.g. an atlas tile) without copying.
    int32_t stride = 0;

    // Optional chroma-key transparency. When hasChromaKey is true, blits skip
    // pixels whose RGB matches (keyR, keyG, keyB). For RGB565 sources the key
    // must be quantised to 5/6/5 (low bits zeroed), since source pixels are
    // read at that precision.
    bool hasChromaKey = false;
    uint8_t keyR = 0;
    uint8_t keyG = 0;
    uint8_t keyB = 0;

    // Optional per-row spans of non-key columns, laid out like alphaRowSpans.
    // When present, the 1:1 chroma path skips the per-pixel compare: pixels
    // in [start, end) are never the key and are written directly, and pixels
    // outside are always the key and skipped without reading. A row whose run
    // contains a key pixel is stored as (-1, -1) and compared pixel by pixel.
    const int16_t* chromaRowSpans = nullptr;

    // Source pixels per world unit. The renderer divides the world-to-screen
    // scale by it, so a 32x32 sprite with pixelsPerMeter 16 covers 2x2 world
    // units. The default 1 treats source pixels as world units.
    float pixelsPerMeter = 1.0f;

    // Mirror the source when sampling. Tiled's flags map straight onto these:
    // flipD transposes (x<->y, square sources only), then flipH mirrors
    // horizontally, then flipV vertically. Flipped blits take the generic
    // per-pixel path. A negative destWidth/destHeight is not a flip: it is
    // rejected and draws nothing.
    bool flipH = false;
    bool flipV = false;
    bool flipD = false;
};

/// Describes a source buffer of the given layout. `ownsPixels` hands the
/// buffer to the renderer, which frees it through Deki::Memory after the blit.
/// It defaults to false, the common and safe case: freeing a buffer the
/// component still uses next frame is a bug.
Source MakeSource(const uint8_t* pixels, int32_t width, int32_t height, PixelLayout layout, bool ownsPixels = false,
                  const int16_t* alphaRowSpans = nullptr);

/// Blits `source` to the target with full 2D transforms.
///
/// screenX/screenY: screen position, after WorldToScreen.
/// scaleX/scaleY: world scale. rotation: world rotation in radians, engine
/// convention.
/// pivotX/pivotY: 0 = left/top, 0.5 = centre, 1 = right/bottom.
/// tintR/G/B: 255 means no tint. tintA: 255 opaque, 0 invisible.
void Blit(const Source& source, uint8_t* target, int32_t targetWidth, int32_t targetHeight,
          Deki::ColorFormat targetFormat, int32_t screenX, int32_t screenY, float scaleX, float scaleY, float rotation,
          float pivotX, float pivotY, uint8_t tintR = 255, uint8_t tintG = 255, uint8_t tintB = 255,
          uint8_t tintA = 255, bool useOrderedDither = false);

/// A faster blit without rotation, into the rect destX, destY, destWidth,
/// destHeight.
///
/// `useOrderedDither`: when the source has per-pixel alpha, partial-alpha
/// pixels are written with 8x8 Bayer ordered dithering instead of alpha
/// blending. That skips reading the destination and the per-channel blend
/// maths, so it is much faster on PSRAM-bound targets, at the cost of a fixed
/// stipple pattern at any partial alpha. Pixels with alpha 0 or 255 are
/// unaffected, and a source without alpha ignores the flag. See
/// https://en.wikipedia.org/wiki/Ordered_dithering.
void BlitScaled(const Source& source, uint8_t* target, int32_t targetWidth, int32_t targetHeight,
                Deki::ColorFormat targetFormat, int32_t destX, int32_t destY, int32_t destWidth, int32_t destHeight,
                uint8_t tintR = 255, uint8_t tintG = 255, uint8_t tintB = 255, uint8_t tintA = 255,
                bool useOrderedDither = false);

// ============================================================================
// Per-row kernel dispatch
// ============================================================================
//
// Platform packages can register hand-tuned SIMD kernels for specific format
// pairs. The blit dispatcher checks the preconditions (format, no rotation, no
// scale, alignment, no tint) up front; when they hold and a kernel is
// registered, the kernel runs, otherwise the scalar loop does. The choice is
// made from those conditions alone, never by retrying after a failure.

enum class KernelOp : uint8_t
{
    // RGB565 source to RGB565 dest, 1:1, opaque copy. src and dst point at
    // RGB565 pixels (2 bytes each). Used by the plain opaque copy and by the
    // opaque middle of the chroma-key row-span path.
    RGB565CopyRow,

    // RGB565A8 source to RGB565 dest, 1:1 alpha blend. src is 3 bytes/pixel
    // (RGB565 plus an alpha byte), dst is 2. The kernel handles the a == 0
    // skip and a == 255 fast path itself. Tint and chroma key must be absent;
    // the caller checks, the kernel does not.
    RGB565A8BlendRow,

    // RGB565 source to RGB565A8 dest, 1:1 opaque expand. src is 2
    // bytes/pixel, dst 3, laid out as [lo, hi, alpha]. Output alpha is 0xFF
    // for every pixel. Tint and chroma key must be absent.
    RGB565ToRGB565A8Row,

    // RGB565A8 source to RGB565A8 dest, 1:1 opaque copy. Both are 3
    // bytes/pixel. The caller guarantees the whole row is opaque (alpha 255
    // everywhere), so the kernel may skip alpha checks; in effect
    // `memcpy(dst, src, count*3)`. Tint and chroma key must be absent.
    RGB565A8CopyRow,

    // RGB565A8 source to RGB565A8 dest, 1:1 alpha blend with src-over alpha
    // union (out.a = src.a + dst.a * (255 - src.a) / 255). Both are 3
    // bytes/pixel. The kernel handles the a == 0 skip and a == 255 fast path
    // itself. Tint and chroma key must be absent.
    RGB565A8BlendRowDestRGB565A8,

    Count
};

// Per-row kernel signature.
//   src, dst      16-byte-aligned source and destination row pointers
//   pixelCount    number of pixels to process, not bytes
//   tintR/G/B/A   passed through for kernels that use them; the no-tint
//                 kernel slots receive (255, 255, 255, 255)
//
// The caller guarantees alignment, format, scale 1, rotation 0 and, where it
// applies, no tint, so the kernel may assume them and use unrolled or SIMD
// loads.
using RowKernelFn = void (*)(const uint8_t* src, uint8_t* dst, int32_t pixelCount, uint8_t tintR, uint8_t tintG,
                             uint8_t tintB, uint8_t tintA);

// Registers a kernel for `op`; nullptr clears it. Usually called once by a
// platform integration package when it starts.
void RegisterKernel(KernelOp op, RowKernelFn fn);

// The kernel registered for `op`, or nullptr. Public mainly for tests.
RowKernelFn GetKernel(KernelOp op);

}  // namespace QuadBlit
