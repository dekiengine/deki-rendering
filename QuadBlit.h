#pragma once
#include <deki/assets/TextureFilter.h>
#include <cstddef>
#include <cstdint>
#include <deki/assets/Texture2D.h>  // TextureFormat, for PixelLayout::FromTexture

namespace Deki
{
enum class ColorFormat;
}
namespace DekiRendering
{
class DirtyRegion;
class Lighting2D;
struct DeferredClear;
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

/// The clip blits use now: the top of the stack while clipping is on, within
/// the target limit when there is one.
ClipRect GetCurrentClipRect();

/// Empties the clip stack and drops the target limit. Call at frame start.
void ClearClipStack();

/// A rect every blit stays inside, on top of the clip stack, even while the
/// stack is off (SetClipEnabled, a renderer's ignoreClip): a frame redrawn
/// only where it changed writes nothing else. Until ClearTargetLimit or
/// ClearClipStack.
void SetTargetLimit(int32_t left, int32_t top, int32_t right, int32_t bottom);

void ClearTargetLimit();

/// While false, blits ignore the clip stack.
void SetClipEnabled(bool enabled);

bool IsClipEnabled();

/// Current clip stack depth, for diagnostics.
int GetClipStackDepth();

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

/// One row's longest run of a single opaque colour: columns [start, end) are
/// all `color` (as stored) at alpha 255. start >= end for none.
struct UniformRun
{
    int16_t start = 0;
    int16_t end = 0;
    uint16_t color = 0;
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

    // How a GPU sink samples it (Deki::TextureFilter, the image's setting).
    // The CPU draws every source Point; so does the GPU for Point.
    Deki::TextureFilter filter = Deki::TextureFilter::Point;

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

    // A strip standing in for a whole picture: one column (width 1) drawn as
    // if it were stretchWidth columns wide, every column that one, or one row
    // (height 1) drawn as if stretchHeight rows tall. Placed, scaled and
    // covered as the whole picture would be, so it draws exactly what that
    // would, from a fraction of the memory: a gradient that changes only top
    // to bottom is one column. 0 draws the pixels as they are.
    int32_t stretchWidth = 0;
    int32_t stretchHeight = 0;

    // Mirror the source when sampling. Tiled's flags map straight onto these:
    // flipD transposes (x<->y, square sources only), then flipH mirrors
    // horizontally, then flipV vertically. Flipped blits take the generic
    // per-pixel path. A negative destWidth/destHeight is not a flip: it is
    // rejected and draws nothing.
    bool flipH = false;
    bool flipV = false;
    bool flipD = false;

    // Optional promises about the pixels, for a cheaper 1:1 copy. A source
    // in slow memory (a big bake in PSRAM) is then mostly not read at all.
    // uniformRows: every row is one colour (a vertical gradient, a flat
    // panel), so each destination row is filled from its source row's first
    // pixel. uniformColumns: every row equals the first (a horizontal
    // gradient), so only the first row is read. Set one only when it holds
    // for every pixel: the copy trusts it.
    bool uniformRows = false;
    bool uniformColumns = false;
    // With uniformRows, each row's colour (as stored), one per row, where it
    // is quick to read: a fill then reads nothing from the pixels. Drawn in
    // strips of columns, every strip reads every row's colour again.
    const uint16_t* rowColors = nullptr;

    // Optional per-row runs of fully transparent pixels (alpha 0), laid out
    // like alphaRowSpans ([start, end) per row; start >= end for none). A 1:1
    // blit skips them without reading: an outline or frame baked at a panel's
    // size is mostly the empty run inside it. Set it only when it holds.
    const int16_t* clearRowSpans = nullptr;

    // Optional per-row runs of one opaque colour (FindUniformRuns), one per
    // row. A 1:1 blend fills them without reading the source: a 9-slice
    // panel's middle in PSRAM is then written, not copied. Set it only when
    // it holds.
    const UniformRun* uniformRuns = nullptr;

    // The pixels never change while they live, so a scaled blit may keep a
    // copy made at the size it is drawn and draw that 1:1 next time (see
    // NextFrame). Set it only for pixels that stay as they are at this
    // address (a sprite's own); call ForgetScaled before they are freed.
    bool cacheScaled = false;

    // Optional: a number for these pixels that no other pixels share, taken
    // from NextRevision() whenever they are written (one each bake). A GPU
    // drawing the blits (BlitSink) keeps its upload of them until it
    // changes. 0 (the default) says nothing: unless cacheScaled promises the
    // pixels stay as they are, they are uploaded again every blit, which is
    // right, only slow. A revision kept while the pixels change in place
    // shows the old ones.
    uint32_t revision = 0;
};

/// A fresh, never-zero Source::revision, for whoever writes pixels a source
/// points at. Unique for the program's life (until it wraps after four
/// billion), so pixels freed and others written at the same address are never
/// mistaken for each other.
uint32_t NextRevision();

/// The size `source` is drawn as: its stretched size (Source::stretchWidth,
/// stretchHeight) where it has one, else its own.
inline int32_t DrawnWidth(const Source& source)
{
    return source.stretchWidth > 0 ? source.stretchWidth : source.width;
}
inline int32_t DrawnHeight(const Source& source)
{
    return source.stretchHeight > 0 ? source.stretchHeight : source.height;
}

/// The scaled-blit cache. A non-rotated blit of a cacheScaled source onto
/// RGB565, drawn at a size its pixels are not (an icon at twice its size),
/// is scaled into a copy once it has been drawn at that size in two frames,
/// and from then on the copy is drawn 1:1: the same pixels, through the 1:1
/// paths' runs instead of sampling every pixel every frame. Copies live in
/// external memory within a fixed budget, and one not drawn for a while is
/// dropped.
///
/// NextFrame starts a frame: the render system calls it once a frame.
void NextFrame();
/// Drops the copies made of pixels in [begin, begin + bytes), and tells the
/// blit sink (BlitSink::Forget): call it before freeing pixels a source marked
/// cacheScaled.
void ForgetScaled(const uint8_t* begin, size_t bytes);
/// Drops every copy.
void ClearScaledCache();
/// The bytes the copies hold, and how many there are (made or waiting for
/// their second frame).
size_t ScaledCacheBytes();
size_t ScaledCacheEntries();

/// Describes a source buffer of the given layout. `ownsPixels` hands the
/// buffer to the renderer, which frees it through Deki::Memory after the blit.
/// It defaults to false, the common and safe case: freeing a buffer the
/// component still uses next frame is a bug.
Source MakeSource(const uint8_t* pixels, int32_t width, int32_t height, PixelLayout layout, bool ownsPixels = false,
                  const int16_t* alphaRowSpans = nullptr);

/// Each row's longest run of one opaque colour in RGB565A8 pixels (3 bytes
/// each, rows `stride` bytes apart), for Source::uniformRuns. Runs shorter
/// than 8 pixels are not worth filling and are left out.
void FindUniformRuns(const uint8_t* pixels, int32_t width, int32_t height, int32_t stride, UniformRun* runs);

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

/// Whether Blit() with these arguments, under the current clip stack, writes
/// every pixel of the rect [left, right) x [top, bottom) of its target with
/// the source's own colours, so nothing there before shows through. No
/// whenever that is not certain: a source with alpha or a colour key, an alpha
/// tint, a rotation. The rect may reach outside the target (the whole view
/// around a strip): the blit's placement is checked, not the target's size.
bool CoversRect(const Source& source, int32_t screenX, int32_t screenY, float scaleX, float scaleY, float rotation,
                float pivotX, float pivotY, uint8_t tintA, int32_t left, int32_t top, int32_t right, int32_t bottom);

/// The target pixels [left, right) x [top, bottom) that Blit() with these
/// arguments can write, whatever the clip and the target's size: a renderer
/// drawing in strips skips the blit in a strip outside them. False when it
/// writes nothing.
bool BlitArea(const Source& source, int32_t screenX, int32_t screenY, float scaleX, float scaleY, float rotation,
              float pivotX, float pivotY, int32_t& left, int32_t& top, int32_t& right, int32_t& bottom);

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
// Blit sink: the frame's blits drawn by something else, a GPU
// ============================================================================
//
// A display that builds its frames on a GPU (GLPresenter) can draw
// the 2D there too. The render system asks the registered sink to take a
// frame (BeginSinkFrame); while it does, every Blit and BlitScaled into that
// frame's buffer goes to the sink, in call order, placed and clipped exactly
// as the CPU code would draw it, and nothing of it is drawn into the buffer.
// The buffer then only holds what code writes into it directly, which the
// sink is told of (NoteWritten) so it can compose it in its place.
//
// Without a sink, or into any other buffer, nothing changes: a blit tests one
// pointer. QuadBlit knows nothing of the GPU; the sink is an interface.

/// One blit, worked out as far as the CPU code works it out. Destination
/// pixel (px, py) of [left, right) x [top, bottom), already within the clip
/// and the target, samples source pixel (x, y):
///
///   x = (originSx + (px - originX) * dSxDx + (py - originY) * dSxDy) >> 16
///   y = (originSy + (px - originX) * dSyDx + (py - originY) * dSyDy) >> 16
///
/// in 32-bit integer arithmetic, shifted arithmetically. A pixel whose (x, y)
/// falls outside [0, sampleWidth) x [0, sampleHeight) is not drawn. Then x is
/// 0 where repeatColumn and y 0 where repeatRow (a stretched strip), the
/// source's flips apply (Source::flipH, flipV, and flipD only to a square
/// source), and the pixel goes through the pipeline: dropped at alpha 0 or the
/// chroma key (compared before the tint), its colour times the tint and its
/// alpha times tintA (each Div255 of the product), dropped at alpha 0, then
/// with orderedDither drawn opaque where its alpha exceeds the Bayer threshold
/// at (px, py) and dropped elsewhere, otherwise blended over the target.
struct SinkBlit
{
    const Source* source;
    int32_t left, top, right, bottom;
    int32_t originX, originY;
    int32_t originSx, originSy;          // 16.16
    int32_t dSxDx, dSyDx, dSxDy, dSyDy;  // 16.16
    int32_t sampleWidth, sampleHeight;
    bool repeatColumn, repeatRow;
    uint8_t tintR, tintG, tintB, tintA;
    bool orderedDither;  // already false for a source without alpha, as the CPU draws it
    ClipRect clip;       // the clip it was drawn under, which the rect is already within
};

/// What draws the blits of a frame instead of the CPU. Registered once
/// (SetBlitSink) by the package that has one; QuadBlit and the render system
/// call it.
class BlitSink
{
public:
    virtual ~BlitSink() = default;

    /// Whether the sink takes the frame drawn into `target`, of `width` x
    /// `height` pixels in `format`, which starts as the opaque colour (clearR,
    /// clearG, clearB) a clear of the target would write. Taking it, the sink
    /// starts its frame from that colour and leaves `target` holding nothing,
    /// so that it collects only what is written into it directly.
    virtual bool BeginFrame(uint8_t* target, int32_t width, int32_t height, Deki::ColorFormat format, uint8_t clearR,
                            uint8_t clearG, uint8_t clearB) = 0;

    /// Draws one blit into the frame, after everything before it. False when
    /// it cannot (a source too big for it, say): the CPU then draws the blit
    /// into the target, and TargetWritten follows.
    virtual bool Draw(const SinkBlit& blit) = 0;

    /// Something was written into the target directly. What it holds goes
    /// over the frame before anything drawn after this, and the target is
    /// left holding nothing again.
    virtual void TargetWritten() = 0;

    /// The frame's last blit has been drawn. What the target still holds is
    /// the caller's to compose last (a display presenting the buffer).
    virtual void EndFrame() = 0;

    /// The pixels in [begin, begin + bytes) are going away (ForgetScaled):
    /// nothing kept of them may be used for pixels later at those addresses.
    virtual void Forget(const uint8_t* begin, size_t bytes) = 0;

    /// Lights the frame drawn so far with `lighting` (Lighting2D::Apply's
    /// light, worked out per pixel where the sink can). False when the sink
    /// cannot light: the renderer then says so, and the frame is not lit.
    virtual bool Light(const DekiRendering::Lighting2D& lighting)
    {
        (void)lighting;
        return false;
    }
};

/// Registers the sink, or removes it with null.
void SetBlitSink(BlitSink* sink);
BlitSink* GetBlitSink();

/// Starts a frame into `target` on the registered sink, if it takes it: from
/// then until EndSinkFrame, blits into `target` go to the sink. False, and the
/// CPU draws as usual, when there is no sink or it declines.
bool BeginSinkFrame(uint8_t* target, int32_t width, int32_t height, Deki::ColorFormat format, uint8_t clearR,
                    uint8_t clearG, uint8_t clearB);
void EndSinkFrame();

/// The buffer whose blits go to the sink now, or null outside a sink frame.
const uint8_t* SinkTarget();

/// Tells the sink that code wrote into `buffer` directly, when it is the sink
/// frame's target; nothing otherwise. The renderer calls it after the hooks
/// of a render pass that writes into the buffer (RenderPass::WritesBuffer).
void NoteWritten(const uint8_t* buffer);

/// Lights what the sink frame drew so far (BlitSink::Light). False outside a
/// sink frame, or when the sink cannot.
bool LightSinkFrame(const DekiRendering::Lighting2D& lighting);

/// The frame's clear while nothing has written `buffer` yet: the renderer
/// leaves it here (RenderContext::clear), and a blit into `buffer` runs it
/// first, as must a render pass before it writes there itself
/// (BeforeWriting). Null when there is none.
void SetPendingClear(const uint8_t* buffer, const DekiRendering::DeferredClear* clear);
const DekiRendering::DeferredClear* PendingClear();
/// Runs the pending clear, if there is one, and forgets it.
void RunPendingClear();
/// Forgets the pending clear without running it: what is drawn next covers it.
void DropPendingClear();
/// Runs `buffer`'s pending clear, if it has one. A render pass that writes
/// into the buffer other than through QuadBlit, and says so only after
/// (RenderPass::WritesBuffer false, NoteWritten), calls it before it does.
void BeforeWriting(const uint8_t* buffer);

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

    Count
};

// Per-row kernel signature.
//   src, dst      16-byte-aligned source and destination row pointers
//   pixelCount    number of pixels to process, not bytes
//   tintR/G/B/A   always (255, 255, 255, 255) for the ops above
//
// The caller guarantees alignment, format, scale 1, rotation 0 and no tint, so
// the kernel may assume them and use unrolled or SIMD loads.
using RowKernelFn = void (*)(const uint8_t* src, uint8_t* dst, int32_t pixelCount, uint8_t tintR, uint8_t tintG,
                             uint8_t tintB, uint8_t tintA);

// Registers a kernel for `op`; nullptr clears it. Usually called once by a
// platform integration package when it starts.
void RegisterKernel(KernelOp op, RowKernelFn fn);

// The kernel registered for `op`, or nullptr. Public mainly for tests.
RowKernelFn GetKernel(KernelOp op);

}  // namespace QuadBlit
