#include "QuadBlit.h"
#include "DekiRenderer.h"
#include "PixelFormat.h"
#include "DirtyRegion.h"
#include "RenderProfiler.h"
#include "WholeScaleRows.h"
#include "ExternalAllocator.h"
#include <cmath>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <new>
#include <vector>

// Deki::ColorFormat comes from the engine header. Never re-declare it here: a
// second definition is a redefinition error once a unity build puts this file
// in a translation unit that includes DekiEngine.h.
#include <deki/Engine.h>
#include <deki/LogSystem.h>
#include <deki/providers/Memory.h>

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
using DekiPixel::FromMemory565;
using DekiPixel::Load565;
using DekiPixel::LoadU16;
using DekiPixel::PackRGB565;
using DekiPixel::StoreU16;
using DekiPixel::ToMemory565;
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

static RowKernelFn s_Kernels[static_cast<int>(KernelOp::Count)] = {};

// KernelOp is uint8_t, so only the upper bound is checked. A `(int)op < 0`
// test is always false, which GCC 15 warns about and ESP-IDF 6 makes an error.
void RegisterKernel(KernelOp op, RowKernelFn fn)
{
    if (static_cast<int>(op) >= static_cast<int>(KernelOp::Count))
    {
        return;
    }
    s_Kernels[static_cast<int>(op)] = fn;
}

RowKernelFn GetKernel(KernelOp op)
{
    if (static_cast<int>(op) >= static_cast<int>(KernelOp::Count))
    {
        return nullptr;
    }
    return s_Kernels[static_cast<int>(op)];
}

static ParallelRowsFn s_ParallelRows = nullptr;
static int32_t s_ParallelMinPixels = 0;

// Playing a recorded frame back (PlayRecording), on each thread that does:
// the clip its blits keep to, and the state of its own they use in place of
// QuadBlit's shared state (the clip stack, the tint tables, the row split,
// the copies running, the profiler). Null otherwise.
struct PlayState;
static thread_local PlayState* t_Play = nullptr;
static thread_local const ClipRect* t_PlayClip = nullptr;

// The buffer whose blits are recorded (BeginRecording), or null.
static const uint8_t* s_RecordTarget = nullptr;

void RegisterParallelRows(ParallelRowsFn fn, int32_t minPixels)
{
    s_ParallelRows = fn;
    s_ParallelMinPixels = minPixels;
}

ParallelRowsFn GetParallelRows()
{
    return s_ParallelRows;
}

static RectCopyFn s_RectCopy = nullptr;
static RectCopyWaitFn s_RectCopyWait = nullptr;
static int32_t s_RectCopyMinPixels = 0;

// The copy still running, if any: into rows [s_CopyY0, s_CopyY1) of the
// buffer s_CopyTarget, of which s_CopyWaited rows were waited for.
static const uint8_t* s_CopyTarget = nullptr;
static int32_t s_CopyY0 = 0;
static int32_t s_CopyY1 = 0;
static int32_t s_CopyWaited = 0;

void FinishCopies()
{
    if (s_CopyTarget)
    {
        s_CopyTarget = nullptr;
        s_RectCopyWait(INT32_MAX);
    }
}

void RegisterRectCopy(RectCopyFn fn, RectCopyWaitFn wait, int32_t minPixels)
{
    FinishCopies();
    s_RectCopy = fn;
    s_RectCopyWait = wait;
    s_RectCopyMinPixels = minPixels;
}

RectCopyFn GetRectCopy()
{
    return s_RectCopy;
}

// Before rows [.., endY) of `buffer` are read or written: the copy running
// into it has passed them.
static void WaitForCopiedRows(const uint8_t* buffer, int32_t endY)
{
    if (t_Play || !s_CopyTarget || buffer != s_CopyTarget || endY <= s_CopyY0)
    {
        return;
    }
    if (endY >= s_CopyY1)
    {
        FinishCopies();
        return;
    }
    const int32_t rows = endY - s_CopyY0;
    if (rows > s_CopyWaited)
    {
        s_RectCopyWait(rows);
        s_CopyWaited = rows;
    }
}


// ============================================================================
// Path timing, for the render profiler
// ============================================================================
// Which loop drew a blit, and what it cost per destination pixel inside the
// clip: the fast paths, and the generic pipeline they fall back to. One test
// per blit when the profiler is off.

enum class BlitPath : uint32_t
{
    CopyRows,
    ChromaSpans,
    BlendSpans,
    TintRows,
    TintScaledRows,
    ExpandRows,
    Rows565A8,
    GenericScaledPlain,
    GenericScaled,
    GenericRotated,
    AlphaRows,
    StretchRows,
    TintRotatedRows,
    RunRows,
    WholeScaleRows,
    CopyDma,  // a whole rectangle, by the platform's DMA (RegisterRectCopy)
    RepeatRows,
    None
};

// One word each, so a report line splits on spaces.
constexpr const char* kBlitPathNames[] = {
    "CopyRows",   "ChromaSpans", "BlendSpans",         "TintRows",      "TintScaledRows",
    "ExpandRows", "Rows565A8",   "GenericScaledPlain", "GenericScaled", "GenericRotated",
    "AlphaRows",  "StretchRows", "TintRotatedRows",    "RunRows",       "WholeScaleRows",
    "CopyDma",    "RepeatRows",
};

// Adds a blit's path to the profiler, or to the playing thread's totals.
static void NotePath(BlitPath path, uint64_t us, uint64_t pixels);

struct PathScope
{
    BlitPath path = BlitPath::None;
    const uint64_t pixels;
    const bool on;
    const uint64_t start;

    explicit PathScope(uint64_t destPixels)
        : pixels(destPixels),
          on(DekiRendering::RenderProfiler::IsEnabled()),
          start(on ? DekiRendering::RenderProfiler::Now() : 0)
    {
    }

    ~PathScope()
    {
        if (on && path != BlitPath::None)
        {
            NotePath(path, DekiRendering::RenderProfiler::Now() - start, pixels);
        }
    }
};

// True when both pointers are 16-byte aligned, as the SIMD (PIE) kernels need.
static inline bool Aligned16(const void* a, const void* b)
{
    return (reinterpret_cast<uintptr_t>(a) & 0xF) == 0 && (reinterpret_cast<uintptr_t>(b) & 0xF) == 0;
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

static ClipRect s_Limit;  // SetTargetLimit; not set (IsSet false) when none

ClipRect GetCurrentClipRect()
{
    if (t_PlayClip)
    {
        return *t_PlayClip;
    }
    ClipRect clip = (!s_ClipEnabled || s_ClipStack.empty()) ? ClipRect{} : s_ClipStack.back();
    if (s_Limit.IsSet())
    {
        clip.left = std::max(clip.left, s_Limit.left);
        clip.top = std::max(clip.top, s_Limit.top);
        clip.right = std::min(clip.right, s_Limit.right);
        clip.bottom = std::min(clip.bottom, s_Limit.bottom);
    }
    return clip;
}

void ClearClipStack()
{
    s_ClipStack.clear();
    s_ClipEnabled = true;
    s_Limit = ClipRect{};
}

void SetTargetLimit(int32_t left, int32_t top, int32_t right, int32_t bottom)
{
    s_Limit = ClipRect{ left, top, right, bottom };
}

void ClearTargetLimit()
{
    s_Limit = ClipRect{};
}

void SwapClipState(ClipState& state)
{
    std::swap(s_ClipStack, state.stack);
    std::swap(s_Limit, state.limit);
    std::swap(s_ClipEnabled, state.enabled);
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
// Blit sink
// ============================================================================
// s_SinkTarget is null outside a sink frame, and a blit's target never is, so
// a blit tests one pointer and costs nothing more without a sink.

static BlitSink* s_Sink = nullptr;
static const uint8_t* s_SinkTarget = nullptr;
static std::atomic<uint32_t> s_SourceRevision{ 0 };

uint32_t NextRevision()
{
    uint32_t revision = s_SourceRevision.fetch_add(1) + 1;
    // Zero means untracked. A wrap after four billion bakes is far away, but
    // skip it rather than hand out the one value that disables caching.
    if (revision == 0)
    {
        revision = s_SourceRevision.fetch_add(1) + 1;
    }
    return revision;
}

void SetBlitSink(BlitSink* sink)
{
    s_Sink = sink;
    s_SinkTarget = nullptr;
}

BlitSink* GetBlitSink()
{
    return s_Sink;
}

bool BeginSinkFrame(uint8_t* target, int32_t width, int32_t height, Deki::ColorFormat format, uint8_t clearR,
                    uint8_t clearG, uint8_t clearB)
{
    s_SinkTarget = nullptr;
    if (!s_Sink || !target || !s_Sink->BeginFrame(target, width, height, format, clearR, clearG, clearB))
    {
        return false;
    }
    s_SinkTarget = target;
    return true;
}

void EndSinkFrame()
{
    if (s_Sink && s_SinkTarget)
    {
        s_Sink->EndFrame();
    }
    s_SinkTarget = nullptr;
}

const uint8_t* SinkTarget()
{
    return s_SinkTarget;
}

// The buffer whose next write is noted (WatchWrites), and where.
static const uint8_t* s_WatchTarget = nullptr;
static bool* s_Watched = nullptr;

void WatchWrites(const uint8_t* buffer, bool* written)
{
    s_WatchTarget = written ? buffer : nullptr;
    s_Watched = s_WatchTarget ? written : nullptr;
}

static inline void NoteWatched(const uint8_t* buffer)
{
    if (buffer == s_WatchTarget && buffer)
    {
        *s_Watched = true;
        s_WatchTarget = nullptr;
        s_Watched = nullptr;
    }
}

// The frame's clear while nothing has written its buffer: a blit tests one
// pointer for it.
static const uint8_t* s_ClearTarget = nullptr;
static const DekiRendering::DeferredClear* s_PendingClear = nullptr;

void SetPendingClear(const uint8_t* buffer, const DekiRendering::DeferredClear* clear)
{
    s_PendingClear = buffer ? clear : nullptr;
    s_ClearTarget = s_PendingClear ? buffer : nullptr;
}

const DekiRendering::DeferredClear* PendingClear()
{
    return s_PendingClear;
}

void RunPendingClear()
{
    if (s_ClearTarget && s_ClearTarget == s_CopyTarget)
    {
        FinishCopies();  // the clear covers every row
    }
    const DekiRendering::DeferredClear* clear = s_PendingClear;
    s_PendingClear = nullptr;
    s_ClearTarget = nullptr;
    if (clear && clear->run)
    {
        clear->run(clear->owner);
    }
}

void DropPendingClear()
{
    s_PendingClear = nullptr;
    s_ClearTarget = nullptr;
}

bool TakePendingClear(const uint8_t* buffer, uint8_t* pixel)
{
    if (!buffer || buffer != s_ClearTarget || !s_PendingClear->pixel ||
        !s_PendingClear->pixel(s_PendingClear->owner, pixel))
    {
        return false;
    }
    DropPendingClear();
    return true;
}

bool PendingClearColour(const uint8_t* buffer, uint8_t* pixel)
{
    return buffer && buffer == s_ClearTarget && s_PendingClear->colour &&
           s_PendingClear->colour(s_PendingClear->owner, pixel);
}

void BeforeWriting(const uint8_t* buffer)
{
    NoteWatched(buffer);
    if (buffer && buffer == s_RecordTarget)
    {
        AbandonRecording("a render pass writes into the frame itself");
    }
    // A pass writing on its own may touch any row.
    if (buffer && buffer == s_CopyTarget)
    {
        FinishCopies();
    }
    if (buffer && buffer == s_ClearTarget)
    {
        RunPendingClear();
    }
}

// BeforeWriting for a blit that touches rows [.., endY) only.
static void BeforeWritingRows(const uint8_t* buffer, int32_t endY)
{
    if (t_Play)
    {
        return;  // a strip of its own: no clear pending, no copy running into it
    }
    NoteWatched(buffer);
    if (buffer && buffer == s_ClearTarget)
    {
        // The clear covers every row.
        FinishCopies();
        RunPendingClear();
        return;
    }
    WaitForCopiedRows(buffer, endY);
}

void NoteWritten(const uint8_t* buffer)
{
    NoteWatched(buffer);
    if (buffer && buffer == s_RecordTarget)
    {
        AbandonRecording("something wrote into the frame itself");
    }
    if (buffer && buffer == s_SinkTarget)
    {
        s_Sink->TargetWritten();
    }
}

bool LightSinkFrame(const DekiRendering::Lighting2D& lighting)
{
    return s_Sink && s_SinkTarget && s_Sink->Light(lighting);
}

bool BeginLitSinkFrame()
{
    return s_Sink && s_SinkTarget && s_Sink->BeginLit();
}

// Hands a blit to the sink; true when it drew it. When it cannot, the CPU
// draws it into the target, so the sink is told the target was written.
static bool DrawOnSink(const SinkBlit& blit)
{
    if (s_Sink->Draw(blit))
    {
        return true;
    }
    s_Sink->TargetWritten();
    return false;
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

void FindUniformRuns(const uint8_t* pixels, int32_t width, int32_t height, int32_t stride, UniformRun* runs)
{
    constexpr int32_t kMinRun = 8;
    for (int32_t y = 0; y < height; y++)
    {
        const uint8_t* row = pixels + static_cast<size_t>(y) * stride;
        UniformRun best;
        for (int32_t x = 0; x < width;)
        {
            if (row[x * 3 + 2] != 255)
            {
                x++;
                continue;
            }
            const uint16_t color = LoadU16(row + x * 3);
            const int32_t start = x;
            while (x < width && row[x * 3 + 2] == 255 && LoadU16(row + x * 3) == color)
            {
                x++;
            }
            if (x - start >= kMinRun && x - start > best.end - best.start)
            {
                best.start = static_cast<int16_t>(start);
                best.end = static_cast<int16_t>(x);
                best.color = color;
            }
        }
        runs[y] = best;
    }
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

// Draws `b` with draw(bounds), on two cores when a split is registered and
// the blit is large enough (RegisterParallelRows): the lower half of its rows
// on the other core. draw must only read what it shares with the other half.
template <typename Draw> static void DrawRows(const BlitBounds& b, Draw&& draw)
{
    const int64_t pixels = static_cast<int64_t>(b.endX - b.startX) * (b.endY - b.startY);
    if (t_Play || !s_ParallelRows || pixels < s_ParallelMinPixels || b.endY - b.startY < 2)
    {
        draw(b);
        return;
    }
    struct Context
    {
        Draw* draw;
        BlitBounds bounds;
    } context{ &draw, b };
    const RowsJob job = [](void* c, int32_t startY, int32_t endY)
    {
        Context* ctx = static_cast<Context*>(c);
        BlitBounds rows = ctx->bounds;
        rows.startY = startY;
        rows.endY = endY;
        (*ctx->draw)(rows);
    };
    if (!s_ParallelRows(job, &context, b.startY, b.startY + (b.endY - b.startY) / 2, b.endY))
    {
        draw(b);
    }
}

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

// A tint as lookup tables over the RGB565 channel values: an RGB565 source
// has only 32 reds, 64 greens and 32 blues, so the pipeline's tinted channel,
// Div255(channel8 * tint), is one of those, worked out once per blit instead
// of per pixel. The packed tables are the same values already shifted into
// place and in memory's byte order, so an opaque pixel is three lookups and a
// store. An untinted blit uses tint 255, which gives the channel back
// unchanged.
struct TintTables
{
    uint8_t r[32], g[64], b[32];      // the tinted 8-bit channel
    uint16_t pr[32], pg[64], pb[32];  // the same, packed into RGB565 as stored
};

static void MakeTintTables(TintTables& t, uint8_t tintR, uint8_t tintG, uint8_t tintB)
{
    for (uint32_t i = 0; i < 32; i++)
    {
        t.r[i] = Div255((i << 3) * tintR);
        t.b[i] = Div255((i << 3) * tintB);
        t.pr[i] = ToMemory565(static_cast<uint16_t>((t.r[i] >> 3) << 11));
        t.pb[i] = ToMemory565(static_cast<uint16_t>(t.b[i] >> 3));
    }
    for (uint32_t i = 0; i < 64; i++)
    {
        t.g[i] = Div255((i << 2) * tintG);
        t.pg[i] = ToMemory565(static_cast<uint16_t>((t.g[i] >> 2) << 5));
    }
}

// The tables for the last few tints asked for, kept between blits: the same
// tints are usually drawn many times a frame (a row of cards, each with its
// coloured badge and white label; a particle emitter's colour while only its
// alpha fades), and building the tables costs about as much as tinting a
// hundred pixels. QuadBlit runs on one thread (its clip stack is shared too),
// so static entries are enough.
// A playing thread's own: the shared tables below are filled in place.
struct PlayState
{
    static constexpr uint32_t kTables = 8;
    TintTables tables[kTables];
    uint32_t keys[kTables];
    uint32_t next = 0;
    struct PathTotal
    {
        uint64_t us = 0;
        uint64_t pixels = 0;
        uint32_t blits = 0;
    };
    PathTotal paths[static_cast<size_t>(BlitPath::None)];
    PlayState()
    {
        for (uint32_t& k : keys)
        {
            k = 0xFFFFFFFFu;
        }
    }
};
static PlayState* s_PlayStates[2] = { nullptr, nullptr };

static void NotePath(BlitPath path, uint64_t us, uint64_t pixels)
{
    if (t_Play)
    {
        PlayState::PathTotal& total = t_Play->paths[static_cast<size_t>(path)];
        total.us += us;
        total.pixels += pixels;
        ++total.blits;
        return;
    }
    DekiRendering::RenderProfiler::AddPath(static_cast<uint32_t>(path), kBlitPathNames[static_cast<uint32_t>(path)],
                                           us, pixels);
}

static const TintTables& TintTablesFor(uint8_t tintR, uint8_t tintG, uint8_t tintB)
{
    const uint32_t playKey = (static_cast<uint32_t>(tintR) << 16) | (static_cast<uint32_t>(tintG) << 8) | tintB;
    if (t_Play)
    {
        PlayState& s = *t_Play;
        for (uint32_t i = 0; i < PlayState::kTables; i++)
        {
            if (s.keys[i] == playKey)
            {
                return s.tables[i];
            }
        }
        const uint32_t i = s.next;
        s.next = (s.next + 1) % PlayState::kTables;
        MakeTintTables(s.tables[i], tintR, tintG, tintB);
        s.keys[i] = playKey;
        return s.tables[i];
    }
    constexpr uint32_t kEntries = 8;
    static TintTables s_Tables[kEntries];
    // No RGB tint packs to 0xFFFFFFFF.
    static uint32_t s_Keys[kEntries] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                         0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
    static uint32_t s_Next = 0;
    const uint32_t key = (static_cast<uint32_t>(tintR) << 16) | (static_cast<uint32_t>(tintG) << 8) | tintB;
    for (uint32_t i = 0; i < kEntries; i++)
    {
        if (s_Keys[i] == key)
        {
            return s_Tables[i];
        }
    }
    const uint32_t i = s_Next;
    s_Next = (s_Next + 1) % kEntries;
    MakeTintTables(s_Tables[i], tintR, tintG, tintB);
    s_Keys[i] = key;
    return s_Tables[i];
}

// The row loops below but BlitRows run from IRAM (DEKI_FAST_ATTR), and each
// one Blit or BlitScaled calls is [[gnu::noinline]]: those run from flash,
// and a loop inlined into one runs from flash with it, its IRAM copy dropped.
// Which ones the compiler inlined changed with any edit near a caller, and
// with it how fast a blit was and how much IRAM the build took.

// Fills n RGB565 pixels with one stored value, two at a time. Inlined where
// it may be: every caller is a row loop in IRAM.
static DEKI_FAST_ATTR void FillRow565(uint16_t* dst, int32_t n, uint16_t value)
{
    if (n <= 0)
    {
        return;
    }
    if (reinterpret_cast<uintptr_t>(dst) & 2)
    {
        *dst++ = value;
        --n;
    }
    const uint32_t pair = static_cast<uint32_t>(value) | (static_cast<uint32_t>(value) << 16);
    uint32_t* dst32 = reinterpret_cast<uint32_t*>(dst);
    for (int32_t i = 0; i < n / 2; ++i)
    {
        dst32[i] = pair;
    }
    if (n & 1)
    {
        dst[n - 1] = value;
    }
}

// A source colour of 8-bit channels (r, g, b) at alpha a (not 0 or 255) over
// the destination pixel bg16 as stored, as the pipeline composites it: each
// channel Div255(source * a + destination * (255 - a)). Red and blue are mixed
// together, in the two halves of one word: each mix is under 65536, so the
// halves never carry into each other, nor does Div255's rounding. Two
// multiplies fewer a pixel, the same result.
[[gnu::always_inline]] static inline uint16_t Blend565(uint32_t r, uint32_t g, uint32_t b, uint32_t a, uint16_t bg16)
{
    const uint32_t bg = FromMemory565(bg16);
    const uint32_t invA = 255u - a;
    uint32_t rb = ((r << 16) | b) * a + ((((bg >> 11) << 19) | ((bg & 0x1Fu) << 3)) * invA);
    rb = ((rb + 0x00010001u + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
    const uint32_t g8 = Div255(g * a + (((bg >> 5) & 0x3Fu) << 2) * invA);
    return ToMemory565(static_cast<uint16_t>(((rb >> 19) << 11) | ((g8 >> 2) << 5) | ((rb & 0xFFu) >> 3)));
}

// A tinted source pixel v of alpha a (not 0 or 255) over the destination
// pixel bg16 as stored, as the pipeline composites it.
[[gnu::always_inline]] static inline uint16_t TintBlend565(const TintTables& t, uint32_t v, uint32_t a, uint16_t bg16)
{
    return Blend565(t.r[v >> 11], t.g[(v >> 5) & 0x3F], t.b[v & 0x1F], a, bg16);
}

// One tinted source pixel of alpha `a` (already multiplied by any alpha tint,
// not 0) onto an RGB565 destination pixel, as the pipeline composites it.
// Always inlined: it runs per pixel, and a call (into flash, from the IRAM
// loops that use it) cost more than the work.
[[gnu::always_inline]] static inline void TintPixel565(const TintTables& t, uint32_t v, uint32_t a, uint16_t* dp)
{
    if (a == 255)
    {
        *dp = static_cast<uint16_t>(t.pr[v >> 11] | t.pg[(v >> 5) & 0x3F] | t.pb[v & 0x1F]);
        return;
    }
    *dp = TintBlend565(t, v, a, *dp);
}

// Destination columns [x0, x1) of one row of a 1:1 tinted blit, each pixel
// with its own alpha. Inlined: its loop is the blit's inner loop. The source's
// bytes per pixel and whether there is an alpha tint are constants, so the
// loop keeps what it needs in registers: an Xtensa core has few, and each one
// spilled was a load per pixel.
template <bool SourceAlpha, bool AlphaTint, int32_t Bpp>
[[gnu::always_inline]] static inline void TintRun(const TintTables& t, const uint8_t* srcRow, uint16_t* dstRow,
                                                  int32_t destX, int32_t x0, int32_t x1, uint32_t tintA)
{
    const uint8_t* sp = srcRow + (x0 - destX) * Bpp;
    uint16_t* dp = dstRow + x0;
    if constexpr (AlphaTint)
    {
        // Every pixel blends: an alpha tint is below 255, and so is any alpha
        // it scales. The last blend is remembered: a translucent shape over a
        // flat background (a card's tinted glow, a faded panel) blends the
        // same source pixel over the same destination pixel after pixel.
        uint32_t lastIn = 0xFFFFFFFFu;  // source alpha << 16 | pixel as stored; nothing packs to this
        uint16_t lastBg = 0;
        uint16_t lastOut = 0;
        for (int32_t px = x0; px < x1; px++, sp += Bpp, dp++)
        {
            uint32_t a8 = 255;
            if constexpr (SourceAlpha)
            {
                a8 = sp[2];
                if (a8 == 0)
                {
                    continue;
                }
            }
            const uint32_t in = (a8 << 16) | LoadU16(sp);
            const uint16_t bg = *dp;
            if (in != lastIn || bg != lastBg)
            {
                lastIn = in;
                lastBg = bg;
                const uint32_t a = Div255(a8 * tintA);
                lastOut = a == 0 ? bg : TintBlend565(t, FromMemory565(static_cast<uint16_t>(in)), a, bg);
            }
            *dp = lastOut;
        }
    }
    else
    {
        for (int32_t px = x0; px < x1; px++, sp += Bpp, dp++)
        {
            uint32_t a = 255;
            if constexpr (SourceAlpha)
            {
                a = sp[2];
                if (a == 0)
                {
                    continue;
                }
            }
            TintPixel565(t, Load565(sp), a, dp);
        }
    }
}

// RGB565 or RGB565A8 -> RGB565, 1:1, any tint and alpha tint; no key, dither or
// flips. The pipeline's arithmetic in one tight loop per row: CompositePixel
// per pixel cost several times as much, most of it for opaque pixels that
// need no blending at all. (Splitting rows at their opaque run, as
// BlendRows_RGB565A8_to_RGB565 does, was measured slower here on an
// ESP32-S3: 100 ns a pixel against 80.)
template <bool SourceAlpha, bool AlphaTint, int32_t Bpp>
[[gnu::noinline]] static DEKI_FAST_ATTR void TintRows_RGB565(const Source& source, uint16_t* target16,
                                                             int32_t targetWidth, int32_t destX, int32_t destY,
                                                             const BlitBounds& b, const BlitParams& p)
{
    const TintTables& t = TintTablesFor(p.tintR, p.tintG, p.tintB);
    const int32_t stride = SourceStride(source);
    const uint32_t tintA = p.tintA;

    const int16_t* clear = SourceAlpha ? source.clearRowSpans : nullptr;
    const UniformRun* runs = SourceAlpha ? source.uniformRuns : nullptr;

    for (int32_t py = b.startY; py < b.endY; py++)
    {
        const int32_t srcY = py - destY;
        const uint8_t* srcRow = source.pixels + srcY * stride;
        uint16_t* dstRow = target16 + py * targetWidth;

        // The row's transparent run (alpha 0 everywhere in it) is not read,
        // and its run of one opaque colour is drawn without reading the
        // source: only the columns either side of them, inside the clip, go
        // pixel by pixel. The two runs never overlap.
        int32_t cs = b.endX, ce = b.endX, us = b.endX, ue = b.endX;
        if (clear)
        {
            cs = std::clamp<int32_t>(destX + clear[srcY * 2], b.startX, b.endX);
            ce = std::clamp<int32_t>(destX + clear[srcY * 2 + 1], cs, b.endX);
        }
        if (runs)
        {
            us = std::clamp<int32_t>(destX + runs[srcY].start, b.startX, b.endX);
            ue = std::clamp<int32_t>(destX + runs[srcY].end, us, b.endX);
        }
        // An empty run goes to the row's end, out of the other's way.
        if (ce == cs)
        {
            cs = ce = b.endX;
        }
        if (ue == us)
        {
            us = ue = b.endX;
        }
        const bool clearFirst = cs < us;
        const int32_t parts[6] = {
            b.startX, clearFirst ? cs : us, clearFirst ? ce : ue, clearFirst ? us : cs, clearFirst ? ue : ce, b.endX
        };
#pragma GCC unroll 1
        for (int32_t i = 0; i < 6; i += 2)
        {
            TintRun<SourceAlpha, AlphaTint, Bpp>(t, srcRow, dstRow, destX, parts[i], parts[i + 1], tintA);
        }

        if (ue > us)
        {
            // Alpha 255 throughout: the tinted colour, or with an alpha tint
            // that colour at the tint's alpha (Div255(255 * a) is a) over
            // each destination pixel, worked out again only when it changes.
            const uint32_t v = FromMemory565(runs[srcY].color);
            uint16_t* dp = dstRow + us;
            if constexpr (AlphaTint)
            {
                uint16_t lastBg = static_cast<uint16_t>(~dp[0]);
                uint16_t lastOut = 0;
                for (int32_t i = 0; i < ue - us; i++)
                {
                    const uint16_t bg = dp[i];
                    if (bg != lastBg)
                    {
                        lastBg = bg;
                        lastOut = TintBlend565(t, v, tintA, bg);
                    }
                    dp[i] = lastOut;
                }
            }
            else
            {
                FillRow565(dp, ue - us, static_cast<uint16_t>(t.pr[v >> 11] | t.pg[(v >> 5) & 0x3F] | t.pb[v & 0x1F]));
            }
        }
    }
}

// An alpha-only source (ALPHA8: text in one colour) onto RGB565, 1:1, any tint
// and alpha tint; no key, dither or flips. The source is white, so the colour
// is the tint itself (Div255(255 * t) is t): one colour, each pixel at its own
// alpha. A byte a pixel read where an RGB565A8 source reads three. Not
// inlined: its one caller runs from flash, and the loop must stay in IRAM.
template <bool AlphaTint>
[[gnu::noinline]] static DEKI_FAST_ATTR void AlphaRows_RGB565(const Source& source, uint16_t* target16,
                                                              int32_t targetWidth, int32_t destX, int32_t destY,
                                                              const BlitBounds& b, const BlitParams& p)
{
    const uint32_t r = p.tintR, g = p.tintG, bl = p.tintB;
    const uint16_t opaque =
        ToMemory565(PackRGB565(static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(bl)));
    const uint32_t tintA = p.tintA;
    const int32_t stride = SourceStride(source);
    for (int32_t py = b.startY; py < b.endY; py++)
    {
        const uint8_t* sp = source.pixels + (py - destY) * stride + (b.startX - destX);
        uint16_t* dp = target16 + py * targetWidth + b.startX;
        for (int32_t px = b.startX; px < b.endX; px++, sp++, dp++)
        {
            uint32_t a = *sp;
            if (a == 0)
            {
                continue;
            }
            if constexpr (AlphaTint)
            {
                a = Div255(a * tintA);
                if (a == 0)
                {
                    continue;
                }
            }
            if (a == 255)
            {
                *dp = opaque;
                continue;
            }
            *dp = Blend565(r, g, bl, a, *dp);
        }
    }
}

// TintRows_RGB565 for a source of Bpp bytes a pixel, with or without an alpha tint.
template <bool SourceAlpha, int32_t Bpp>
static inline void TintRowsFor(const Source& source, uint16_t* target16, int32_t targetWidth, int32_t destX,
                               int32_t destY, const BlitBounds& b, const BlitParams& p)
{
    if (p.hasAlphaTint)
    {
        TintRows_RGB565<SourceAlpha, true, Bpp>(source, target16, targetWidth, destX, destY, b, p);
    }
    else
    {
        TintRows_RGB565<SourceAlpha, false, Bpp>(source, target16, targetWidth, destX, destY, b, p);
    }
}

// RGB565 or RGB565A8 -> RGB565A8, at any scale, any tint and alpha tint; no
// key, dither or flips. The scaled pipeline's stepping and arithmetic in one
// tight loop per row: a particle emitter draws each particle this way into
// its composite, dozens a frame.
template <bool SourceAlpha>
[[gnu::noinline]] static DEKI_FAST_ATTR void
TintScaledRows_RGB565A8(const Source& source, uint8_t* target, int32_t targetWidth, int32_t destX, int32_t destY,
                        int32_t destWidth, int32_t destHeight, const BlitBounds& b, const BlitParams& p)
{
    const TintTables& t = TintTablesFor(p.tintR, p.tintG, p.tintB);
    const int32_t stride = SourceStride(source);
    const int32_t bpp = source.bytesPerPixel;
    const bool alphaTint = p.hasAlphaTint;
    const uint32_t tintA = p.tintA;
    const uint32_t xStep = (static_cast<uint32_t>(source.width) << 16) / static_cast<uint32_t>(destWidth);
    const uint32_t yStep = (static_cast<uint32_t>(source.height) << 16) / static_cast<uint32_t>(destHeight);

    for (int32_t py = b.startY; py < b.endY; py++)
    {
        const int32_t srcY =
            static_cast<int32_t>((static_cast<uint32_t>(py - destY) * yStep + ((yStep - 1) >> 1)) >> 16);
        const uint8_t* srcRow = source.pixels + srcY * stride;
        uint8_t* dp = target + (static_cast<size_t>(py) * static_cast<size_t>(targetWidth) + b.startX) * 3;
        uint32_t acc = static_cast<uint32_t>(b.startX - destX) * xStep + ((xStep - 1) >> 1);
        for (int32_t px = b.startX; px < b.endX; px++, dp += 3)
        {
            const uint8_t* sp = srcRow + static_cast<int32_t>(acc >> 16) * bpp;
            acc += xStep;
            uint32_t a = 255;
            if constexpr (SourceAlpha)
            {
                a = sp[2];
                if (a == 0)
                {
                    continue;
                }
            }
            if (alphaTint)
            {
                a = Div255(a * tintA);
                if (a == 0)
                {
                    continue;
                }
            }
            const uint32_t v = Load565(sp);
            const uint32_t r5 = v >> 11;
            const uint32_t g6 = (v >> 5) & 0x3F;
            const uint32_t b5 = v & 0x1F;
            uint32_t out;  // as stored
            if (a == 255)
            {
                out = t.pr[r5] | t.pg[g6] | t.pb[b5];
                dp[2] = 255;
            }
            else
            {
                const uint32_t bg = Load565(dp);
                const uint32_t invA = 255u - a;
                out = ToMemory565(PackRGB565(Div255(t.r[r5] * a + ((bg >> 11) << 3) * invA),
                                             Div255(t.g[g6] * a + (((bg >> 5) & 0x3F) << 2) * invA),
                                             Div255(t.b[b5] * a + ((bg & 0x1F) << 3) * invA)));
                dp[2] = AlphaUnion(static_cast<uint8_t>(a), dp[2]);
            }
            StoreU16(dp, static_cast<uint16_t>(out));
        }
    }
}

// RGB565 or RGB565A8 -> RGB565, at any scale, any tint and alpha tint; no key,
// dither or flips. The scaled pipeline's stepping and TintRows' arithmetic in
// one tight loop per row: particles drawn straight into the frame take it,
// dozens a frame, where the per-pixel pipeline cost half again as much. The
// source's bytes per pixel is a constant, so finding a pixel is a shift and
// an add.
template <bool SourceAlpha, int32_t Bpp>
[[gnu::noinline]] static DEKI_FAST_ATTR void
TintScaledRows_RGB565(const Source& source, uint16_t* target16, int32_t targetWidth, int32_t destX, int32_t destY,
                      int32_t destWidth, int32_t destHeight, const BlitBounds& b, const BlitParams& p)
{
    const TintTables& t = TintTablesFor(p.tintR, p.tintG, p.tintB);
    const int32_t stride = SourceStride(source);
    const bool alphaTint = p.hasAlphaTint;
    const uint32_t tintA = p.tintA;
    const uint32_t xStep = (static_cast<uint32_t>(source.width) << 16) / static_cast<uint32_t>(destWidth);
    const uint32_t yStep = (static_cast<uint32_t>(source.height) << 16) / static_cast<uint32_t>(destHeight);

    for (int32_t py = b.startY; py < b.endY; py++)
    {
        const int32_t srcY =
            static_cast<int32_t>((static_cast<uint32_t>(py - destY) * yStep + ((yStep - 1) >> 1)) >> 16);
        const uint8_t* srcRow = source.pixels + srcY * stride;
        uint16_t* dp = target16 + py * targetWidth + b.startX;
        uint32_t acc = static_cast<uint32_t>(b.startX - destX) * xStep + ((xStep - 1) >> 1);
        for (int32_t px = b.startX; px < b.endX; px++, dp++)
        {
            const uint8_t* sp = srcRow + static_cast<int32_t>(acc >> 16) * Bpp;
            acc += xStep;
            uint32_t a = 255;
            if constexpr (SourceAlpha)
            {
                a = sp[2];
                if (a == 0)
                {
                    continue;
                }
            }
            if (alphaTint)
            {
                a = Div255(a * tintA);
                if (a == 0)
                {
                    continue;
                }
            }
            TintPixel565(t, Load565(sp), a, dp);
        }
    }
}

// Opaque RGB565 -> RGB565 at a whole-number scale on both axes, plain (no
// tint, key, dither or flip): the pixels as stored, each repeated across the
// first target row of the rows it covers, which the others then copy whole.
// A map's view copy held at the map's own pixels is scaled up this way.
[[gnu::noinline]] static DEKI_FAST_ATTR void RepeatRows_RGB565(const Source& source, uint16_t* target16,
                                                               int32_t targetWidth, int32_t destX, int32_t destY,
                                                               int32_t scaleX, int32_t scaleY, const BlitBounds& b)
{
    const int32_t stride = SourceStride(source);
    const size_t rowBytes = static_cast<size_t>(b.endX - b.startX) * 2;
    int32_t py = b.startY;
    while (py < b.endY)
    {
        const int32_t blockY = (py - destY) / scaleY;
        const int32_t rowsEnd = std::min(b.endY, destY + (blockY + 1) * scaleY);
        const uint8_t* srcRow = source.pixels + static_cast<size_t>(blockY) * stride;
        uint16_t* const first = target16 + static_cast<size_t>(py) * targetWidth + b.startX;
        uint16_t* o = first;
        int32_t x = b.startX;
        int32_t sx = (x - destX) / scaleX;
        auto at = [srcRow](int32_t i)
        {
            uint16_t v;
            std::memcpy(&v, srcRow + static_cast<size_t>(i) * 2, 2);
            return v;
        };
        // The first source pixel, part of its block perhaps.
        const int32_t lead = std::min(b.endX, destX + (sx + 1) * scaleX) - x;
        for (const uint16_t v = at(sx); o < first + lead;)
        {
            *o++ = v;
        }
        x += lead;
        ++sx;
        if (scaleX == 2)
        {
            for (; x + 2 <= b.endX; x += 2, ++sx, o += 2)
            {
                const uint16_t v = at(sx);
                o[0] = v;
                o[1] = v;
            }
        }
        else
        {
            for (; x + scaleX <= b.endX; x += scaleX, ++sx)
            {
                const uint16_t v = at(sx);
                for (int32_t k = 0; k < scaleX; ++k)
                {
                    *o++ = v;
                }
            }
        }
        if (x < b.endX)
        {
            const uint16_t v = at(sx);
            for (; x < b.endX; ++x)
            {
                *o++ = v;
            }
        }
        for (int32_t y = py + 1; y < rowsEnd; ++y)
        {
            std::memcpy(target16 + static_cast<size_t>(y) * targetWidth + b.startX, first, rowBytes);
        }
        py = rowsEnd;
    }
}

// RGB565 or RGB565A8 -> RGB565 at a whole-number scale on both axes (pixel art
// drawn at 2x, 3x...), any tint and alpha tint; no key, dither or flips.
// TintScaledRows_RGB565's pixels: at a whole scale its stepping lands on
// source column (x - destX) / scaleX and row (y - destY) / scaleY (checked for
// scales up to 8 and targets up to 8192 pixels), so each source pixel is
// decoded and tinted once for the block it covers instead of once per target
// pixel. An opaque pixel's block is filled; a translucent one is blended
// pixel by pixel with the same arithmetic. A source row, decoded, serves
// every target row it covers.
template <bool SourceAlpha, int32_t Bpp>
[[gnu::noinline]] static DEKI_FAST_ATTR void
WholeScaleRows_RGB565(const Source& source, uint16_t* target16, int32_t targetWidth, int32_t destX, int32_t destY,
                      int32_t scaleX, int32_t scaleY, const BlitBounds& b, const BlitParams& p)
{
    // Flipped across or down (not turned: flipD stays with the pipeline), as
    // ApplyFlips flips the pipeline's source pixel.
    const bool flipH = source.flipH;
    const bool flipV = source.flipV;
    const TintTables& t = TintTablesFor(p.tintR, p.tintG, p.tintB);
    const int32_t stride = SourceStride(source);
    const bool alphaTint = p.hasAlphaTint;
    const uint32_t tintA = p.tintA;

    // Source columns the clip reaches, decoded a run at a time: the tinted
    // pixel as stored for an opaque one, the raw pixel for a translucent one,
    // and its alpha (0: nothing drawn). The opaque ones are also laid out as
    // the target row shows them, widened, so a stretch of them goes into each
    // target row the source row covers as one copy.
    constexpr int32_t kRun = 128;
    uint16_t values[kRun];
    uint8_t alphas[kRun];
    uint16_t widened[kRun * 8];
    const int32_t firstCol = (b.startX - destX) / scaleX;
    const int32_t endCol = (b.endX - 1 - destX) / scaleX + 1;

    int32_t py = b.startY;
    while (py < b.endY)
    {
        const int32_t blockY = (py - destY) / scaleY;
        const int32_t rowsEnd = std::min(b.endY, destY + (blockY + 1) * scaleY);
        const int32_t srcY = flipV ? source.height - 1 - blockY : blockY;
        const uint8_t* srcRow = source.pixels + srcY * stride;
        for (int32_t c0 = firstCol; c0 < endCol; c0 += kRun)
        {
            const int32_t c1 = std::min(endCol, c0 + kRun);
            for (int32_t sx = c0; sx < c1; sx++)
            {
                const uint8_t* sp = srcRow + (flipH ? source.width - 1 - sx : sx) * Bpp;
                uint32_t a = 255;
                if constexpr (SourceAlpha)
                {
                    a = sp[2];
                }
                if (alphaTint && a != 0)
                {
                    a = Div255(a * tintA);
                }
                const uint32_t v = Load565(sp);
                alphas[sx - c0] = static_cast<uint8_t>(a);
                values[sx - c0] =
                    a == 255 ? static_cast<uint16_t>(t.pr[v >> 11] | t.pg[(v >> 5) & 0x3F] | t.pb[v & 0x1F])
                             : static_cast<uint16_t>(v);
            }
            const int32_t x0 = std::max(b.startX, destX + c0 * scaleX);
            const int32_t x1 = std::min(b.endX, destX + c1 * scaleX);
            // The run as the target row shows it, worked out once for every
            // target row this source row covers: stretches of opaque pixels,
            // each repeated across its block into `widened` and copied in one
            // go, and translucent blocks, blended pixel by pixel; clear ones
            // are left out.
            struct Segment
            {
                int32_t x0, x1;  // target columns
                int32_t i;       // the source column's place in values/alphas (translucent)
                bool opaque;
            };
            Segment segments[kRun];
            int32_t count = 0;
            {
                int32_t x = x0;
                for (int32_t i = 0; x < x1; i++)
                {
                    const int32_t blockEnd = std::min(x1, destX + (c0 + i + 1) * scaleX);
                    const uint32_t a = alphas[i];
                    if (a == 255)
                    {
                        for (int32_t px = x; px < blockEnd; px++)
                        {
                            widened[px - x0] = values[i];
                        }
                        if (count > 0 && segments[count - 1].opaque && segments[count - 1].x1 == x)
                        {
                            segments[count - 1].x1 = blockEnd;
                        }
                        else
                        {
                            segments[count++] = { x, blockEnd, i, true };
                        }
                    }
                    else if (a != 0)
                    {
                        segments[count++] = { x, blockEnd, i, false };
                    }
                    x = blockEnd;
                }
            }
            for (int32_t y = py; y < rowsEnd; y++)
            {
                uint16_t* row = target16 + y * targetWidth;
                for (int32_t s = 0; s < count; s++)
                {
                    const Segment& seg = segments[s];
                    if (seg.opaque)
                    {
                        memcpy(row + seg.x0, widened + (seg.x0 - x0), static_cast<size_t>(seg.x1 - seg.x0) * 2);
                        continue;
                    }
                    const uint32_t value = values[seg.i];
                    const uint32_t a = alphas[seg.i];
                    for (int32_t px = seg.x0; px < seg.x1; px++)
                    {
                        row[px] = TintBlend565(t, value, a, row[px]);
                    }
                }
            }
        }
        py = rowsEnd;
    }
}

// RGB565 -> RGB565, no tint or key: row copy (the SIMD kernel when aligned).
// A source whose rows are each one colour is filled instead, reading one
// pixel a row; one whose rows all equal the first copies that row each time.
[[gnu::noinline]] static DEKI_FAST_ATTR bool CopyRows_RGB565(const Source& source, uint16_t* target16,
                                                             int32_t targetWidth, int32_t destX, int32_t destY,
                                                             const BlitBounds& b)
{
    const int32_t stride = SourceStride(source);
    const int32_t rowPixels = b.endX - b.startX;
    if (source.uniformRows)
    {
        for (int32_t py = b.startY; py < b.endY; py++)
        {
            const uint16_t value =
                source.rowColors ? source.rowColors[py - destY] : LoadU16(source.pixels + (py - destY) * stride);
            FillRow565(target16 + py * targetWidth + b.startX, rowPixels, value);
        }
        return true;
    }
    RowKernelFn copyKernel = s_Kernels[static_cast<int>(KernelOp::RGB565CopyRow)];
    for (int32_t py = b.startY; py < b.endY; py++)
    {
        const int32_t srcY = source.uniformColumns ? 0 : py - destY;
        const uint16_t* srcPtr = reinterpret_cast<const uint16_t*>(source.pixels + srcY * stride) + (b.startX - destX);
        uint16_t* dstPtr = target16 + py * targetWidth + b.startX;
        if (copyKernel && Aligned16(srcPtr, dstPtr))
        {
            copyKernel(reinterpret_cast<const uint8_t*>(srcPtr), reinterpret_cast<uint8_t*>(dstPtr), rowPixels, 255,
                       255, 255, 255);
        }
        else
        {
            memcpy(dstPtr, srcPtr, rowPixels * sizeof(uint16_t));
        }
    }
    return true;
}

// RGB565 -> RGB565, no tint or key, as one rectangle on the platform's DMA
// (RegisterRectCopy), when there is one and the copy is large enough. False
// leaves it to CopyRows_RGB565. A source of uniform rows or columns is
// cheaper on the CPU, which reads one pixel a row or one row.
static bool CopyRect_RGB565(const Source& source, uint16_t* target16, int32_t targetWidth, int32_t destX,
                            int32_t destY, const BlitBounds& b)
{
    if (t_Play || !s_RectCopy || source.uniformRows || source.uniformColumns)
    {
        return false;
    }
    const int32_t width = b.endX - b.startX;
    const int32_t height = b.endY - b.startY;
    if (width <= 0 || height <= 0 || static_cast<int64_t>(width) * height < s_RectCopyMinPixels)
    {
        return false;
    }
    FinishCopies();  // one at a time
    const int32_t stride = SourceStride(source);
    const uint8_t* src = source.pixels + (b.startY - destY) * stride + (b.startX - destX) * 2;
    // It may go on while the CPU draws only over whole rows: then nothing
    // the CPU writes shares a cache line with a row still on its way.
    const bool mayRun = b.startX == 0 && width == targetWidth;
    const RectCopyResult result =
        s_RectCopy(src, stride, target16 + b.startY * targetWidth + b.startX, targetWidth, width, height, mayRun);
    if (result == RectCopyResult::Running)
    {
        s_CopyTarget = reinterpret_cast<const uint8_t*>(target16);
        s_CopyY0 = b.startY;
        s_CopyY1 = b.endY;
        s_CopyWaited = 0;
    }
    return result != RectCopyResult::Refused;
}

// RGB565 -> RGB565 from one column or one row stretched over its rectangle
// (Source::stretchWidth/stretchHeight), no tint: one column fills each row
// with its pixel for that row; one row is sampled into the first row drawn,
// as the scaled paths sample it, and that row copied down the rest.
[[gnu::noinline]] static DEKI_FAST_ATTR void StretchRows_RGB565(const Source& source, uint16_t* target16,
                                                                int32_t targetWidth, int32_t destX, int32_t destY,
                                                                int32_t destWidth, int32_t destHeight,
                                                                const BlitBounds& b)
{
    const int32_t stride = SourceStride(source);
    const int32_t rowPixels = b.endX - b.startX;
    if (source.width == 1)
    {
        const uint32_t yStep = (static_cast<uint32_t>(source.height) << 16) / static_cast<uint32_t>(destHeight);
        for (int32_t py = b.startY; py < b.endY; py++)
        {
            const int32_t srcY =
                static_cast<int32_t>((static_cast<uint32_t>(py - destY) * yStep + ((yStep - 1) >> 1)) >> 16);
            FillRow565(target16 + py * targetWidth + b.startX, rowPixels, LoadU16(source.pixels + srcY * stride));
        }
        return;
    }
    const uint32_t xStep = (static_cast<uint32_t>(source.width) << 16) / static_cast<uint32_t>(destWidth);
    uint16_t* first = target16 + b.startY * targetWidth + b.startX;
    uint32_t acc = static_cast<uint32_t>(b.startX - destX) * xStep + ((xStep - 1) >> 1);
    for (int32_t i = 0; i < rowPixels; i++, acc += xStep)
    {
        first[i] = LoadU16(source.pixels + static_cast<int32_t>(acc >> 16) * 2);
    }
    for (int32_t py = b.startY + 1; py < b.endY; py++)
    {
        memcpy(target16 + py * targetWidth + b.startX, first, rowPixels * sizeof(uint16_t));
    }
}

// RGB565 -> RGB565 with a chroma key and per-row non-key spans, no tint.
// Inside [start, end) no pixel is the key (straight copy); outside, every
// pixel is the key (skipped without a read). A row stored as (-1, -1) has a
// key pixel inside its run and is compared pixel by pixel.
[[gnu::noinline]] static DEKI_FAST_ATTR bool CopyRows_RGB565_ChromaSpans(const Source& source, uint16_t* target16,
                                                                         int32_t targetWidth, int32_t destX,
                                                                         int32_t destY, const BlitBounds& b)
{
    const int32_t stride = SourceStride(source);
    const int16_t* spans = source.chromaRowSpans;
    RowKernelFn copyKernel = s_Kernels[static_cast<int>(KernelOp::RGB565CopyRow)];
    // The key is quantised to 5/6/5, so comparing packed pixels is the same
    // test the per-pixel pipeline makes on the unpacked channels. It is
    // compared with pixels as stored, so it is in memory's byte order too.
    const uint16_t key565 = ToMemory565(PackRGB565(source.keyR, source.keyG, source.keyB));
    for (int32_t py = b.startY; py < b.endY; py++)
    {
        const int32_t srcY = py - destY;
        const int32_t srcStartX = b.startX - destX;
        const int32_t srcEndX = b.endX - destX;
        if (spans[srcY * 2] < 0)
        {
            const uint16_t* srcRow = reinterpret_cast<const uint16_t*>(source.pixels + srcY * stride);
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
        const uint16_t* srcPtr = reinterpret_cast<const uint16_t*>(source.pixels + srcY * stride) + clampedStart;
        uint16_t* dstPtr = target16 + py * targetWidth + (destX + clampedStart);
        const int32_t rowPixels = clampedEnd - clampedStart;
        if (copyKernel && Aligned16(srcPtr, dstPtr))
        {
            copyKernel(reinterpret_cast<const uint8_t*>(srcPtr), reinterpret_cast<uint8_t*>(dstPtr), rowPixels, 255,
                       255, 255, 255);
        }
        else
        {
            memcpy(dstPtr, srcPtr, rowPixels * sizeof(uint16_t));
        }
    }
    return true;
}

// One untinted RGB565A8 source pixel onto an RGB565 destination pixel, as
// the pipeline composites it.
[[gnu::always_inline]] static inline void BlendPixel565A8(const uint8_t* sp, uint16_t* dp)
{
    const uint32_t a = sp[2];
    if (a == 0)
    {
        return;
    }
    const uint16_t stored = LoadU16(sp);
    if (a == 255)
    {
        *dp = stored;
        return;
    }
    const uint32_t v = FromMemory565(stored);
    *dp = Blend565((v >> 11) << 3, ((v >> 5) & 0x3Fu) << 2, (v & 0x1Fu) << 3, a, *dp);
}

// Source columns [x0, x1) of an untinted RGB565A8 row onto RGB565 (dst is the
// destination pixel of source column 0), skipping the row's transparent run
// [c0, c1) without reading it.
[[gnu::always_inline]] static inline void BlendRun565A8(const uint8_t* srcRow, int32_t bpp, uint16_t* dst, int32_t x0,
                                                        int32_t x1, int32_t c0, int32_t c1)
{
    int32_t sx = x0;
    while (sx < x1)
    {
        if (sx >= c0 && sx < c1)
        {
            sx = c1;
            continue;
        }
        const int32_t end = sx < c0 ? std::min(x1, c0) : x1;
        const uint8_t* sp = srcRow + sx * bpp;
        for (; sx < end; sx++, sp += bpp)
        {
            BlendPixel565A8(sp, dst + sx);
        }
    }
}

// RGB565A8 with alpha -> RGB565, no tint or key: a per-row opaque-span split
// (left blend | opaque copy | right blend) when spans are available,
// otherwise a blend of the whole row. The blends skip the row's transparent
// run (clearRowSpans) without reading it.
[[gnu::noinline]] static DEKI_FAST_ATTR bool BlendRows_RGB565A8_to_RGB565(const Source& source, uint16_t* target16,
                                                                          int32_t targetWidth, int32_t destX,
                                                                          int32_t destY, const BlitBounds& b)
{
    const int32_t stride = SourceStride(source);
    const int32_t bpp = source.bytesPerPixel;
    const int16_t* rowSpans = source.alphaRowSpans;
    const int16_t* clear = source.clearRowSpans;
    const UniformRun* runs = source.uniformRuns;

    for (int32_t py = b.startY; py < b.endY; py++)
    {
        const int32_t srcY = py - destY;
        const uint8_t* rowBase = source.pixels + srcY * stride;
        uint16_t* dstRow = target16 + py * targetWidth;
        uint16_t* dst = dstRow + destX;  // source column 0
        const int32_t srcStartX = b.startX - destX;
        const int32_t srcEndX = b.endX - destX;
        const int32_t c0 = clear ? clear[srcY * 2] : 0;
        const int32_t c1 = clear ? clear[srcY * 2 + 1] : 0;

        if (rowSpans)
        {
            const int32_t clampedStart = std::max<int32_t>(rowSpans[srcY * 2], srcStartX);
            const int32_t clampedEnd = std::min<int32_t>(rowSpans[srcY * 2 + 1], srcEndX);

            // Left alpha region.
            BlendRun565A8(rowBase, bpp, dst, srcStartX, std::min(clampedStart, srcEndX), c0, c1);

            // Opaque middle: direct copy, no alpha checks. A run of one colour
            // (uniformRuns) is filled, without reading the source.
            int32_t fillStart = clampedEnd, fillEnd = clampedEnd;
            if (runs && clampedEnd > clampedStart)
            {
                fillStart = std::clamp<int32_t>(runs[srcY].start, clampedStart, clampedEnd);
                fillEnd = std::clamp<int32_t>(runs[srcY].end, fillStart, clampedEnd);
                FillRow565(dstRow + destX + fillStart, fillEnd - fillStart, runs[srcY].color);
            }
            const uint8_t* srcPtr = rowBase + clampedStart * bpp;
            for (int32_t sx = clampedStart; sx < fillStart; sx++, srcPtr += bpp)
            {
                dstRow[destX + sx] = LoadU16(srcPtr);
            }
            srcPtr = rowBase + fillEnd * bpp;
            for (int32_t sx = fillEnd; sx < clampedEnd; sx++, srcPtr += bpp)
            {
                dstRow[destX + sx] = LoadU16(srcPtr);
            }

            // Right alpha region. Starts at the clip start when the opaque span
            // ends before it or is empty; starting at opaqueEnd then would
            // write pixels the clip rect excludes.
            BlendRun565A8(rowBase, bpp, dst, std::max(clampedEnd, srcStartX), srcEndX, c0, c1);
            continue;
        }

        BlendRun565A8(rowBase, bpp, dst, srcStartX, srcEndX, c0, c1);
    }
    return true;
}

// RGB565 -> RGB565A8, no tint or key: opaque expand. Not inlined into its one
// caller, which runs from flash.
[[gnu::noinline]] static DEKI_FAST_ATTR bool ExpandRows_RGB565_to_RGB565A8(const Source& source, uint8_t* target,
                                                                           int32_t targetWidth, int32_t destX,
                                                                           int32_t destY, const BlitBounds& b)
{
    const int32_t stride = SourceStride(source);
    const int32_t rowPixels = b.endX - b.startX;
    for (int32_t py = b.startY; py < b.endY; py++)
    {
        const uint16_t* srcPtr =
            reinterpret_cast<const uint16_t*>(source.pixels + (py - destY) * stride) + (b.startX - destX);
        uint8_t* dstPtr = target + (py * targetWidth + b.startX) * 3;
        for (int32_t i = 0; i < rowPixels; i++)
        {
            const uint16_t v = srcPtr[i];
            StoreU16(dstPtr + i * 3, v);
            dstPtr[i * 3 + 2] = 0xFF;
        }
    }
    return true;
}

// RGB565A8 -> RGB565A8, no tint or key: opaque copy when the source declares
// no alpha, otherwise the plain pipeline per pixel.
[[gnu::noinline]] static DEKI_FAST_ATTR bool Rows_RGB565A8_to_RGB565A8(const Source& source, uint8_t* target,
                                                                       int32_t targetWidth, int32_t destX,
                                                                       int32_t destY, const BlitBounds& b)
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
            // Byte 2 of the source is ignored: the source declares itself opaque.
            for (int32_t i = 0; i < rowPixels; i++)
            {
                dstPtr[i * 3] = srcPtr[i * 3];
                dstPtr[i * 3 + 1] = srcPtr[i * 3 + 1];
                dstPtr[i * 3 + 2] = 0xFF;
            }
            continue;
        }
        const size_t rowIdx = static_cast<size_t>(py) * static_cast<size_t>(targetWidth);
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
// Each drawn pixel reads the source pixel just before its middle: (step - 1)
// / 2 to start, as Qt and Tiled do. Exactly between two source pixels it
// takes the first, so a picture shrunk to half takes every other pixel from
// the first, and one grown by a whole number repeats each pixel evenly.
// ============================================================================

// The per-pixel pipeline at any scale, for whatever the row loops above do
// not take. From flash, inlined into its callers: twenty instances in IRAM
// would take 12 KB of it, and Wyrdmoor on a T-Deck drew no faster.
template <SrcKind SK, Deki::ColorFormat F, bool Plain>
static void BlitRows(const Source& source, uint8_t* target, int32_t targetWidth, int32_t destX, int32_t destY,
                     int32_t destWidth, int32_t destHeight, const BlitBounds& b, const BlitParams& P)
{
    const int32_t bpp = source.bytesPerPixel;
    const int32_t stride = SourceStride(source);
    const uint32_t xStep = (static_cast<uint32_t>(source.width) << 16) / static_cast<uint32_t>(destWidth);
    const uint32_t yStep = (static_cast<uint32_t>(source.height) << 16) / static_cast<uint32_t>(destHeight);

    for (int32_t py = b.startY; py < b.endY; py++)
    {
        const int32_t srcY =
            static_cast<int32_t>((static_cast<uint32_t>(py - destY) * yStep + ((yStep - 1) >> 1)) >> 16);
        const uint8_t* srcRow = source.pixels + srcY * stride;
        const size_t rowIdx = static_cast<size_t>(py) * static_cast<size_t>(targetWidth);

        uint32_t acc = static_cast<uint32_t>(b.startX - destX) * xStep + ((xStep - 1) >> 1);
        for (int32_t px = b.startX; px < b.endX; px++)
        {
            const int32_t srcX = static_cast<int32_t>(acc >> 16);
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

// ============================================================================
// Recorded frames (BeginRecording, PlayRecording)
// ============================================================================

struct RecordedOp
{
    enum class Kind : uint8_t
    {
        Scaled,  // BlitScaled: the rect x, y, w, h
        Turned,  // Blit turned: the point x, y and the transform
        Runs,    // BlitRuns: the corner x, y, the size w, h
        Fill     // a fill of [x, w) x [y, h) with tint as the pixel
    };
    Kind kind = Kind::Scaled;
    bool dither = false;
    uint8_t tint[4] = { 255, 255, 255, 255 };
    ClipRect clip;
    Source source{};
    int32_t x = 0, y = 0, w = 0, h = 0;
    float scaleX = 0.0f, scaleY = 0.0f, rotation = 0.0f, pivotX = 0.0f, pivotY = 0.0f;
    const uint8_t* runs = nullptr;
};

struct RecordedFrame
{
    uint8_t* target = nullptr;
    int32_t width = 0;
    int32_t height = 0;
    Deki::ColorFormat format = Deki::ColorFormat::RGB565;
    // In external memory: written once, then read only where a strip meets
    // the blit, which `rows` says from internal memory.
    std::vector<RecordedOp, DekiRendering::ExternalAllocator<RecordedOp>> ops;
    std::vector<int32_t> rows;  // each op's rows [top, bottom), two numbers an op
    std::vector<void*> owned;   // copies of pixels the renderer frees after their blit
    const char* abandoned = nullptr;
};

static RecordedFrame s_Recorded;

static void FreeOwned()
{
    for (void* p : s_Recorded.owned)
    {
        Deki::Memory::Free(p);
    }
    s_Recorded.owned.clear();
}

// The source as the op keeps it: pixels the renderer frees after the blit
// (ownsPixels) copied first, for the playing back after.
static Source KeptSource(const Source& source)
{
    Source kept = source;
    if (!source.ownsPixels || !source.pixels)
    {
        return kept;
    }
    kept.ownsPixels = false;
    const size_t stride = static_cast<size_t>(SourceStride(source));
    const size_t bytes = stride * static_cast<size_t>(source.height - 1) +
                         static_cast<size_t>(source.width) * static_cast<size_t>(source.bytesPerPixel);
    void* copy = Deki::Memory::Allocate(bytes, Deki::Memory::IsExternalAvailable() ? Deki::Memory::External
                                                                                   : Deki::Memory::Internal);
    if (!copy)
    {
        AbandonRecording("no memory to keep pixels freed after their blit");
        return kept;
    }
    std::memcpy(copy, source.pixels, bytes);
    s_Recorded.owned.push_back(copy);
    kept.pixels = static_cast<const uint8_t*>(copy);
    return kept;
}

static void KeepOp(const RecordedOp& op, int32_t top, int32_t bottom)
{
    s_Recorded.ops.push_back(op);
    s_Recorded.rows.push_back(top);
    s_Recorded.rows.push_back(bottom);
}

static int32_t FormatBytes(Deki::ColorFormat format)
{
    switch (format)
    {
        case Deki::ColorFormat::RGB565: return 2;
        case Deki::ColorFormat::RGB888: return 3;
        case Deki::ColorFormat::RGB565A8: return 3;
        case Deki::ColorFormat::ARGB8888: return 4;
    }
    return 2;
}

bool BeginRecording(uint8_t* target, int32_t width, int32_t height, Deki::ColorFormat format)
{
    FreeOwned();
    s_Recorded.ops.clear();
    s_Recorded.rows.clear();
    s_Recorded.abandoned = nullptr;
    s_RecordTarget = nullptr;
    if (!target || width <= 0 || height <= 0 || t_Play)
    {
        return false;
    }
    s_Recorded.target = target;
    s_Recorded.width = width;
    s_Recorded.height = height;
    s_Recorded.format = format;
    s_RecordTarget = target;
    return true;
}

bool IsRecording(const uint8_t* buffer)
{
    return buffer && buffer == s_RecordTarget;
}

void RecordFill(int32_t left, int32_t top, int32_t right, int32_t bottom, const uint8_t* pixel)
{
    if (!s_RecordTarget || !pixel)
    {
        return;
    }
    left = std::max<int32_t>(0, left);
    top = std::max<int32_t>(0, top);
    right = std::min(s_Recorded.width, right);
    bottom = std::min(s_Recorded.height, bottom);
    if (left >= right || top >= bottom)
    {
        return;
    }
    RecordedOp op;
    op.kind = RecordedOp::Kind::Fill;
    op.x = left;
    op.y = top;
    op.w = right;
    op.h = bottom;
    std::memcpy(op.tint, pixel, static_cast<size_t>(FormatBytes(s_Recorded.format)));
    KeepOp(op, top, bottom);
}

void AbandonRecording(const char* why)
{
    if (s_RecordTarget && !s_Recorded.abandoned)
    {
        s_Recorded.abandoned = why ? why : "given up";
    }
}

const RecordedFrame* EndRecording(const char** why)
{
    s_RecordTarget = nullptr;
    if (why)
    {
        *why = s_Recorded.abandoned;
    }
    return s_Recorded.abandoned ? nullptr : &s_Recorded;
}

size_t RecordedCount(const RecordedFrame& frame)
{
    return frame.ops.size();
}

void PlayRecording(const RecordedFrame& frame, uint8_t* rows, int32_t top, int32_t bottom, int lane, int32_t left,
                   int32_t right)
{
    left = std::max<int32_t>(left, 0);
    right = std::min(right, frame.width);
    if (!rows || lane < 0 || lane > 1 || top >= bottom || left >= right)
    {
        return;
    }
    if (!s_PlayStates[lane])
    {
        s_PlayStates[lane] = new (std::nothrow) PlayState();
        if (!s_PlayStates[lane])
        {
            return;
        }
    }
    const int32_t bpp = FormatBytes(frame.format);
    const size_t pitch = static_cast<size_t>(frame.width) * static_cast<size_t>(bpp);
    // The frame these rows are part of, as the blits address it: they write
    // only rows [top, bottom), which the clip keeps them to.
    uint8_t* const at =
        reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(rows) - static_cast<uintptr_t>(top) * pitch);
    PlayState* const before = t_Play;
    t_Play = s_PlayStates[lane];
    const size_t n = frame.ops.size();
    for (size_t i = 0; i < n; i++)
    {
        const int32_t opTop = frame.rows[i * 2], opBottom = frame.rows[i * 2 + 1];
        if (opBottom <= top || opTop >= bottom)
        {
            continue;
        }
        const RecordedOp& op = frame.ops[i];
        ClipRect clip = op.clip;
        clip.top = std::max(clip.top, top);
        clip.bottom = std::min(clip.bottom, bottom);
        clip.left = std::max(clip.left, left);
        clip.right = std::min(clip.right, right);
        if (clip.left >= clip.right)
        {
            continue;
        }
        t_PlayClip = &clip;
        switch (op.kind)
        {
            case RecordedOp::Kind::Scaled:
                BlitScaled(op.source, at, frame.width, frame.height, frame.format, op.x, op.y, op.w, op.h, op.tint[0],
                           op.tint[1], op.tint[2], op.tint[3], op.dither);
                break;
            case RecordedOp::Kind::Turned:
                Blit(op.source, at, frame.width, frame.height, frame.format, op.x, op.y, op.scaleX, op.scaleY,
                     op.rotation, op.pivotX, op.pivotY, op.tint[0], op.tint[1], op.tint[2], op.tint[3], op.dither);
                break;
            case RecordedOp::Kind::Runs:
                BlitRuns(op.runs, op.w, op.h, at, frame.width, frame.height, op.x, op.y, op.tint[0], op.tint[1],
                         op.tint[2], op.tint[3]);
                break;
            case RecordedOp::Kind::Fill:
            {
                const int32_t y0 = std::max(op.y, top), y1 = std::min(op.h, bottom);
                const int32_t x0 = std::max(op.x, left), x1 = std::min(op.w, right);
                for (int32_t y = y0; y < y1 && x0 < x1; y++)
                {
                    uint8_t* row = at + static_cast<size_t>(y) * pitch + static_cast<size_t>(x0) * bpp;
                    if (bpp == 2)
                    {
                        uint16_t value;
                        std::memcpy(&value, op.tint, 2);
                        FillRow565(reinterpret_cast<uint16_t*>(row), x1 - x0, value);
                    }
                    else
                    {
                        for (int32_t x = x0; x < x1; x++, row += bpp)
                        {
                            std::memcpy(row, op.tint, static_cast<size_t>(bpp));
                        }
                    }
                }
                break;
            }
        }
    }
    t_PlayClip = nullptr;
    t_Play = before;
}

void FlushPlayStats()
{
    for (PlayState* s : s_PlayStates)
    {
        if (!s)
        {
            continue;
        }
        for (size_t i = 0; i < static_cast<size_t>(BlitPath::None); i++)
        {
            PlayState::PathTotal& total = s->paths[i];
            if (total.blits > 0)
            {
                DekiRendering::RenderProfiler::AddPath(static_cast<uint32_t>(i), kBlitPathNames[i], total.us,
                                                       total.pixels, total.blits);
            }
            total = PlayState::PathTotal{};
        }
    }
}

// ============================================================================
// Scaled-blit cache (Source::cacheScaled)
// ============================================================================

namespace
{
// One source drawn at one size: waiting for its second frame (no pixels yet),
// or its scaled copy.
struct ScaledCopy
{
    const uint8_t* src = nullptr;
    int32_t srcW = 0, srcH = 0, stride = 0, bpp = 0;
    int32_t w = 0, h = 0;
    uint32_t firstSeen = 0;
    uint32_t lastUsed = 0;
    uint8_t* pixels = nullptr;
    size_t bytes = 0;
    int16_t* spans = nullptr;    // opaque runs, as Source::alphaRowSpans
    int16_t* clear = nullptr;    // transparent runs, as Source::clearRowSpans
    UniformRun* runs = nullptr;  // runs of one opaque colour, as Source::uniformRuns
};

// Policy: what the copies may hold, how many sizes are tracked, and how many
// frames a copy lives undrawn. A copy bigger than a quarter of the budget is
// not made: a scaled background is cheap to sample anyway.
size_t s_ScaledBudget = 256 * 1024;
size_t s_ScaledMaxEntries = 64;
constexpr uint32_t kScaledIdleFrames = 120;

std::vector<ScaledCopy> s_Scaled;
uint32_t s_ScaledFrame = 1;
size_t s_ScaledBytes = 0;

void FreeCopy(ScaledCopy& e)
{
    if (e.pixels)
    {
        s_ScaledBytes -= e.bytes;
        Deki::Memory::Free(e.pixels);
        Deki::Memory::Free(e.spans);
        Deki::Memory::Free(e.clear);
        Deki::Memory::Free(e.runs);
    }
    e.pixels = nullptr;
    e.spans = nullptr;
    e.clear = nullptr;
    e.runs = nullptr;
    e.bytes = 0;
}

void EraseCopy(size_t i)
{
    FreeCopy(s_Scaled[i]);
    s_Scaled[i] = s_Scaled.back();
    s_Scaled.pop_back();
}

// Makes room for `need` bytes by dropping the copies drawn longest ago, never
// one drawn this frame. False when that is not enough.
bool MakeRoom(size_t need)
{
    while (s_ScaledBytes + need > s_ScaledBudget)
    {
        size_t oldest = s_Scaled.size();
        for (size_t i = 0; i < s_Scaled.size(); i++)
        {
            const ScaledCopy& e = s_Scaled[i];
            if (e.pixels && e.lastUsed != s_ScaledFrame &&
                (oldest == s_Scaled.size() || e.lastUsed < s_Scaled[oldest].lastUsed))
            {
                oldest = i;
            }
        }
        if (oldest == s_Scaled.size())
        {
            return false;
        }
        FreeCopy(s_Scaled[oldest]);
    }
    return true;
}

// Scales `s` into `e`'s copy with TintScaledRows' sampling, so drawing the
// copy 1:1 gives the pixels the scaled blit would, and finds the copy's
// opaque and transparent runs for the 1:1 paths.
bool MakeCopy(ScaledCopy& e, const Source& s)
{
    const size_t rowBytes = static_cast<size_t>(e.w) * static_cast<size_t>(e.bpp);
    const size_t pixelBytes = rowBytes * static_cast<size_t>(e.h);
    const bool alpha = s.hasAlpha && s.bytesPerPixel >= 3;
    const size_t spanBytes = alpha ? static_cast<size_t>(e.h) * 2 * sizeof(int16_t) : 0;
    const size_t runBytes = alpha ? static_cast<size_t>(e.h) * sizeof(UniformRun) : 0;
    const size_t need = pixelBytes + 2 * spanBytes + runBytes;
    if (need > s_ScaledBudget / 4 || !MakeRoom(need))
    {
        return false;
    }
    e.pixels = static_cast<uint8_t*>(Deki::Memory::Allocate(pixelBytes, Deki::Memory::External));
    if (alpha)
    {
        e.spans = static_cast<int16_t*>(Deki::Memory::Allocate(spanBytes, Deki::Memory::External));
        e.clear = static_cast<int16_t*>(Deki::Memory::Allocate(spanBytes, Deki::Memory::External));
        e.runs = static_cast<UniformRun*>(Deki::Memory::Allocate(runBytes, Deki::Memory::External));
    }
    if (!e.pixels || (alpha && (!e.spans || !e.clear || !e.runs)))
    {
        Deki::Memory::Free(e.pixels);
        Deki::Memory::Free(e.spans);
        Deki::Memory::Free(e.clear);
        Deki::Memory::Free(e.runs);
        e.pixels = nullptr;
        e.spans = nullptr;
        e.clear = nullptr;
        e.runs = nullptr;
        return false;
    }
    e.bytes = need;
    s_ScaledBytes += need;

    const uint32_t xStep = (static_cast<uint32_t>(e.srcW) << 16) / static_cast<uint32_t>(e.w);
    const uint32_t yStep = (static_cast<uint32_t>(e.srcH) << 16) / static_cast<uint32_t>(e.h);
    for (int32_t y = 0; y < e.h; y++)
    {
        const uint8_t* srcRow =
            s.pixels + static_cast<int32_t>((static_cast<uint32_t>(y) * yStep + ((yStep - 1) >> 1)) >> 16) * e.stride;
        uint8_t* dst = e.pixels + static_cast<size_t>(y) * rowBytes;
        uint32_t acc = (xStep - 1) >> 1;
        for (int32_t x = 0; x < e.w; x++, acc += xStep, dst += e.bpp)
        {
            std::memcpy(dst, srcRow + static_cast<int32_t>(acc >> 16) * e.bpp, static_cast<size_t>(e.bpp));
        }
    }
    if (alpha)
    {
        const int32_t a = s.alphaOffset;
        for (int32_t y = 0; y < e.h; y++)
        {
            const uint8_t* row = e.pixels + static_cast<size_t>(y) * rowBytes;
            // The first run of opaque pixels; [0, 0) for none.
            int32_t start = 0;
            while (start < e.w && row[start * e.bpp + a] != 255)
            {
                start++;
            }
            int32_t end = start;
            while (end < e.w && row[end * e.bpp + a] == 255)
            {
                end++;
            }
            if (start >= e.w)
            {
                start = end = 0;
            }
            e.spans[y * 2] = static_cast<int16_t>(start);
            e.spans[y * 2 + 1] = static_cast<int16_t>(end);
            // The longest run of transparent pixels; [0, 0) for none.
            int32_t bestStart = 0, bestEnd = 0;
            for (int32_t x = 0; x < e.w;)
            {
                if (row[x * e.bpp + a] != 0)
                {
                    x++;
                    continue;
                }
                const int32_t runStart = x;
                while (x < e.w && row[x * e.bpp + a] == 0)
                {
                    x++;
                }
                if (x - runStart > bestEnd - bestStart)
                {
                    bestStart = runStart;
                    bestEnd = x;
                }
            }
            e.clear[y * 2] = static_cast<int16_t>(bestStart);
            e.clear[y * 2 + 1] = static_cast<int16_t>(bestEnd);
        }
        FindUniformRuns(e.pixels, e.w, e.h, static_cast<int32_t>(rowBytes), e.runs);
    }
    return true;
}

// The copy of `s` at w x h when there is one, made now if `s` was drawn at that
// size in an earlier frame. False: draw `s` scaled as usual.
bool ScaledCopyFor(const Source& s, int32_t w, int32_t h, Source& out)
{
    const int32_t stride = SourceStride(s);
    ScaledCopy* found = nullptr;
    for (ScaledCopy& e : s_Scaled)
    {
        if (e.src == s.pixels && e.w == w && e.h == h && e.srcW == s.width && e.srcH == s.height &&
            e.stride == stride && e.bpp == s.bytesPerPixel)
        {
            found = &e;
            break;
        }
    }
    if (!found)
    {
        if (s_Scaled.size() >= s_ScaledMaxEntries)
        {
            // Drop the entry drawn longest ago to track this one.
            size_t oldest = 0;
            for (size_t i = 1; i < s_Scaled.size(); i++)
            {
                if (s_Scaled[i].lastUsed < s_Scaled[oldest].lastUsed)
                {
                    oldest = i;
                }
            }
            if (s_Scaled[oldest].lastUsed == s_ScaledFrame)
            {
                return false;
            }
            EraseCopy(oldest);
        }
        ScaledCopy e;
        e.src = s.pixels;
        e.srcW = s.width;
        e.srcH = s.height;
        e.stride = stride;
        e.bpp = s.bytesPerPixel;
        e.w = w;
        e.h = h;
        e.firstSeen = s_ScaledFrame;
        e.lastUsed = s_ScaledFrame;
        s_Scaled.push_back(e);
        return false;
    }
    found->lastUsed = s_ScaledFrame;
    // A size seen only this frame may be one step of an animation: wait.
    if (!found->pixels && (found->firstSeen == s_ScaledFrame || !MakeCopy(*found, s)))
    {
        return false;
    }
    out = s;
    out.pixels = found->pixels;
    out.width = w;
    out.height = h;
    out.stride = 0;
    out.alphaRowSpans = found->spans;
    out.clearRowSpans = found->clear;
    out.uniformRuns = found->runs;
    // Uniform rows or columns stay so, scaled; the source's row colours are
    // its own rows', not the copy's.
    out.rowColors = nullptr;
    out.chromaRowSpans = nullptr;
    out.ownsPixels = false;
    out.cacheScaled = false;
    return true;
}
}  // namespace

void NextFrame()
{
    ++s_ScaledFrame;
    for (size_t i = s_Scaled.size(); i-- > 0;)
    {
        if (s_ScaledFrame - s_Scaled[i].lastUsed > kScaledIdleFrames)
        {
            EraseCopy(i);
        }
    }
}

void ForgetScaled(const uint8_t* begin, size_t bytes)
{
    if (s_RecordTarget)
    {
        AbandonRecording("pixels a blit was kept for went away");
    }
    if (s_Sink)
    {
        s_Sink->Forget(begin, bytes);
    }
    const uint8_t* end = begin + bytes;
    for (size_t i = s_Scaled.size(); i-- > 0;)
    {
        if (s_Scaled[i].src >= begin && s_Scaled[i].src < end)
        {
            EraseCopy(i);
        }
    }
}

void ClearScaledCache()
{
    if (s_RecordTarget)
    {
        AbandonRecording("pixels a blit was kept for went away");
    }
    while (!s_Scaled.empty())
    {
        EraseCopy(s_Scaled.size() - 1);
    }
}

void SetScaledCacheLimits(size_t bytes, size_t entries)
{
    s_ScaledBudget = bytes;
    s_ScaledMaxEntries = entries > 0 ? entries : 1;
    while (!s_Scaled.empty() && (s_ScaledBytes > s_ScaledBudget || s_Scaled.size() > s_ScaledMaxEntries))
    {
        size_t oldest = 0;
        for (size_t i = 1; i < s_Scaled.size(); i++)
        {
            if (s_Scaled[i].lastUsed < s_Scaled[oldest].lastUsed)
            {
                oldest = i;
            }
        }
        EraseCopy(oldest);
    }
}

size_t ScaledCacheBytes()
{
    return s_ScaledBytes;
}

size_t ScaledCacheEntries()
{
    return s_Scaled.size();
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
    BeforeWritingRows(target, bounds.endY);

    // A frame drawn by the sink: the rect, and the 16.16 steps BlitRows
    // samples with, from the rect's corner.
    if (target == s_SinkTarget)
    {
        SinkBlit blit;
        blit.source = &source;
        blit.left = bounds.startX;
        blit.top = bounds.startY;
        blit.right = bounds.endX;
        blit.bottom = bounds.endY;
        blit.originX = destX;
        blit.originY = destY;
        blit.dSxDx =
            static_cast<int32_t>((static_cast<uint32_t>(source.width) << 16) / static_cast<uint32_t>(destWidth));
        blit.dSyDx = 0;
        blit.dSxDy = 0;
        blit.dSyDy =
            static_cast<int32_t>((static_cast<uint32_t>(source.height) << 16) / static_cast<uint32_t>(destHeight));
        // Just before the middle of the corner pixel, as the rows below sample.
        blit.originSx = (blit.dSxDx - 1) >> 1;
        blit.originSy = (blit.dSyDy - 1) >> 1;
        blit.sampleWidth = source.width;
        blit.sampleHeight = source.height;
        blit.repeatColumn = false;
        blit.repeatRow = false;
        blit.tintR = tintR;
        blit.tintG = tintG;
        blit.tintB = tintB;
        blit.tintA = tintA;
        blit.orderedDither = useOrderedDither && source.hasAlpha;
        blit.clip = GetCurrentClipRect();
        if (DrawOnSink(blit))
        {
            return;
        }
    }

    // What TintScaledRows_RGB565 below would sample every frame: drawn 1:1
    // from a copy scaled once instead (Source::cacheScaled).
    if (!t_Play && source.cacheScaled && source.stretchWidth == 0 && source.stretchHeight == 0 &&
        targetFormat == Deki::ColorFormat::RGB565 && (destWidth != source.width || destHeight != source.height) &&
        !source.hasChromaKey && !(useOrderedDither && source.hasAlpha) && !HasFlips(source) &&
        (KindOf(source) == SrcKind::RGB565 || KindOf(source) == SrcKind::RGB565A8))
    {
        Source copy;
        if (ScaledCopyFor(source, destWidth, destHeight, copy))
        {
            BlitScaled(copy, target, targetWidth, targetHeight, targetFormat, destX, destY, destWidth, destHeight,
                       tintR, tintG, tintB, tintA, useOrderedDither);
            return;
        }
    }
    if (!t_Play && target == s_RecordTarget)
    {
        RecordedOp op;
        op.kind = RecordedOp::Kind::Scaled;
        op.source = KeptSource(source);
        op.x = destX;
        op.y = destY;
        op.w = destWidth;
        op.h = destHeight;
        op.tint[0] = tintR;
        op.tint[1] = tintG;
        op.tint[2] = tintB;
        op.tint[3] = tintA;
        op.dither = useOrderedDither;
        op.clip = GetCurrentClipRect();
        KeepOp(op, bounds.startY, bounds.endY);
        return;
    }
    PathScope scope(static_cast<uint64_t>(bounds.endX - bounds.startX) *
                    static_cast<uint64_t>(bounds.endY - bounds.startY));

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

    // One column or one row stretched over the rectangle, opaque and plain:
    // filled and copied, not sampled pixel by pixel.
    if (!oneToOne && plain && kind == SrcKind::RGB565 && targetFormat == Deki::ColorFormat::RGB565 &&
        (source.width == 1 || source.height == 1))
    {
        scope.path = BlitPath::StretchRows;
        StretchRows_RGB565(source, reinterpret_cast<uint16_t*>(target), targetWidth, destX, destY, destWidth,
                           destHeight, bounds);
        return;
    }

    // 1:1 onto RGB565 from an RGB565 or RGB565A8 source, tinted or blended: one
    // tight loop per row (TintRows_RGB565). Left to the paths below: a plain RGB565 copy, and
    // a plain blend with opaque spans to copy. A plain blend without spans
    // stays here: its rows' transparent runs (clearRowSpans) are skipped
    // unread, where blending whole rows read every pixel of a glyph's box.
    if (oneToOne && !p.hasKey && !p.dither && !p.flips && targetFormat == Deki::ColorFormat::RGB565 &&
        (kind == SrcKind::RGB565 || kind == SrcKind::RGB565A8))
    {
        const bool plainCopy = plain && kind == SrcKind::RGB565;
        const bool plainSpans = plain && kind == SrcKind::RGB565A8 && source.hasAlpha && source.alphaRowSpans;
        if (!plainCopy && !plainSpans)
        {
            uint16_t* target16 = reinterpret_cast<uint16_t*>(target);
            scope.path = BlitPath::TintRows;
            // Made before rows may be drawn on two cores (DrawRows).
            (void)TintTablesFor(p.tintR, p.tintG, p.tintB);
            DrawRows(bounds,
                     [&](const BlitBounds& rows)
                     {
                         if (kind == SrcKind::RGB565A8 && source.hasAlpha)
                         {
                             TintRowsFor<true, 3>(source, target16, targetWidth, destX, destY, rows, p);
                         }
                         else if (kind == SrcKind::RGB565A8)
                         {
                             TintRowsFor<false, 3>(source, target16, targetWidth, destX, destY, rows, p);
                         }
                         else
                         {
                             TintRowsFor<false, 2>(source, target16, targetWidth, destX, destY, rows, p);
                         }
                     });
            return;
        }
    }

    // 1:1 onto RGB565 from an alpha-only source (text in one colour).
    if (oneToOne && kind == SrcKind::ALPHA8 && !p.hasKey && !p.dither && !p.flips &&
        targetFormat == Deki::ColorFormat::RGB565)
    {
        scope.path = BlitPath::AlphaRows;
        uint16_t* target16 = reinterpret_cast<uint16_t*>(target);
        if (p.hasAlphaTint)
        {
            AlphaRows_RGB565<true>(source, target16, targetWidth, destX, destY, bounds, p);
        }
        else
        {
            AlphaRows_RGB565<false>(source, target16, targetWidth, destX, destY, bounds, p);
        }
        return;
    }

    // Onto RGB565A8 from an RGB565 or RGB565A8 source, scaled or tinted (a
    // particle into its emitter's composite): TintScaledRows_RGB565A8. A plain
    // 1:1 blit is left to the copy and expand paths below.
    if (!p.hasKey && !p.dither && !p.flips && targetFormat == Deki::ColorFormat::RGB565A8 &&
        (kind == SrcKind::RGB565 || kind == SrcKind::RGB565A8) && !(plain && oneToOne))
    {
        scope.path = BlitPath::TintScaledRows;
        if (kind == SrcKind::RGB565A8 && source.hasAlpha)
        {
            TintScaledRows_RGB565A8<true>(source, target, targetWidth, destX, destY, destWidth, destHeight, bounds, p);
        }
        else
        {
            TintScaledRows_RGB565A8<false>(source, target, targetWidth, destX, destY, destWidth, destHeight, bounds, p);
        }
        return;
    }

    // Onto RGB565 from an RGB565 or RGB565A8 source, scaled, tinted or not (a
    // particle drawn straight into the frame): TintScaledRows_RGB565. 1:1
    // blits took the row paths above.
    // A flip across or down is taken at a whole-number scale only.
    const int32_t wholeX = WholeScale(source.width, destWidth);
    const int32_t wholeY = WholeScale(source.height, destHeight);
    const bool whole = wholeX > 0 && wholeY > 0 && wholeX <= 8 && wholeY <= 8;
    if (!oneToOne && !p.hasKey && !p.dither && (!p.flips || (whole && !source.flipD)) &&
        targetFormat == Deki::ColorFormat::RGB565 && (kind == SrcKind::RGB565 || kind == SrcKind::RGB565A8))
    {
        uint16_t* target16 = reinterpret_cast<uint16_t*>(target);
        // The tint's tables made now, before rows may be drawn on two cores:
        // both halves then only read them.
        (void)TintTablesFor(p.tintR, p.tintG, p.tintB);
        // At a whole-number scale, each source pixel once for its block.
        const int32_t scaleX = wholeX;
        const int32_t scaleY = wholeY;
        if (whole && plain && kind == SrcKind::RGB565 && !source.hasAlpha)
        {
            scope.path = BlitPath::RepeatRows;
            DrawRows(bounds,
                     [&](const BlitBounds& rows)
                     { RepeatRows_RGB565(source, target16, targetWidth, destX, destY, scaleX, scaleY, rows); });
            return;
        }
        if (whole)
        {
            scope.path = BlitPath::WholeScaleRows;
            DrawRows(bounds,
                     [&](const BlitBounds& rows)
                     {
                         if (kind == SrcKind::RGB565A8 && source.hasAlpha)
                         {
                             WholeScaleRows_RGB565<true, 3>(source, target16, targetWidth, destX, destY, scaleX,
                                                            scaleY, rows, p);
                         }
                         else if (kind == SrcKind::RGB565A8)
                         {
                             WholeScaleRows_RGB565<false, 3>(source, target16, targetWidth, destX, destY, scaleX,
                                                             scaleY, rows, p);
                         }
                         else
                         {
                             WholeScaleRows_RGB565<false, 2>(source, target16, targetWidth, destX, destY, scaleX,
                                                             scaleY, rows, p);
                         }
                     });
            return;
        }
        scope.path = BlitPath::TintScaledRows;
        DrawRows(bounds,
                 [&](const BlitBounds& rows)
                 {
                     if (kind == SrcKind::RGB565A8 && source.hasAlpha)
                     {
                         TintScaledRows_RGB565<true, 3>(source, target16, targetWidth, destX, destY, destWidth,
                                                        destHeight, rows, p);
                     }
                     else if (kind == SrcKind::RGB565A8)
                     {
                         TintScaledRows_RGB565<false, 3>(source, target16, targetWidth, destX, destY, destWidth,
                                                         destHeight, rows, p);
                     }
                     else
                     {
                         TintScaledRows_RGB565<false, 2>(source, target16, targetWidth, destX, destY, destWidth,
                                                         destHeight, rows, p);
                     }
                 });
        return;
    }

    // 1:1 row fast paths: the same pixels as the pipeline, in fewer instructions.
    if (oneToOne && !p.hasTint && !p.hasAlphaTint && !p.dither && !p.flips)
    {
        if (targetFormat == Deki::ColorFormat::RGB565)
        {
            uint16_t* target16 = reinterpret_cast<uint16_t*>(target);
            if (kind == SrcKind::RGB565 && p.hasKey && source.chromaRowSpans)
            {
                scope.path = BlitPath::ChromaSpans;
                CopyRows_RGB565_ChromaSpans(source, target16, targetWidth, destX, destY, bounds);
                return;
            }
            if (plain && kind == SrcKind::RGB565)
            {
                if (CopyRect_RGB565(source, target16, targetWidth, destX, destY, bounds))
                {
                    scope.path = BlitPath::CopyDma;
                    return;
                }
                scope.path = BlitPath::CopyRows;
                DrawRows(bounds, [&](const BlitBounds& rows)
                         { CopyRows_RGB565(source, target16, targetWidth, destX, destY, rows); });
                return;
            }
            if (plain && kind == SrcKind::RGB565A8 && source.hasAlpha)
            {
                scope.path = BlitPath::BlendSpans;
                DrawRows(bounds, [&](const BlitBounds& rows)
                         { BlendRows_RGB565A8_to_RGB565(source, target16, targetWidth, destX, destY, rows); });
                return;
            }
        }
        else if (targetFormat == Deki::ColorFormat::RGB565A8 && plain)
        {
            if (kind == SrcKind::RGB565)
            {
                scope.path = BlitPath::ExpandRows;
                ExpandRows_RGB565_to_RGB565A8(source, target, targetWidth, destX, destY, bounds);
                return;
            }
            scope.path = BlitPath::Rows565A8;
            if (kind == SrcKind::RGB565A8 &&
                Rows_RGB565A8_to_RGB565A8(source, target, targetWidth, destX, destY, bounds))
            {
                return;
            }
        }
    }

    // Plain onto ARGB8888 at a whole-number scale, 1:1 included: each source
    // pixel read once for the block it covers (WholeScaleRows.h).
    if (plain && targetFormat == Deki::ColorFormat::ARGB8888)
    {
        scope.path = BlitPath::WholeScaleRows;
        if (WholeScaleRows_ARGB8888(source, reinterpret_cast<uint32_t*>(target), targetWidth, destX, destY, destWidth,
                                    destHeight, bounds.startX, bounds.startY, bounds.endX, bounds.endY))
        {
            return;
        }
    }

    if (plain)
    {
        scope.path = BlitPath::GenericScaledPlain;
        BlitRowsDispatch<true>(kind, targetFormat, source, target, targetWidth, destX, destY, destWidth, destHeight,
                               bounds, p);
    }
    else
    {
        scope.path = BlitPath::GenericScaled;
        BlitRowsDispatch<false>(kind, targetFormat, source, target, targetWidth, destX, destY, destWidth, destHeight,
                                bounds, p);
    }
}

// ============================================================================
// Pictures kept as runs
// ============================================================================

size_t PackRuns(const uint8_t* pixels, int32_t width, int32_t height, int32_t stride, bool flipH, bool flipV,
                uint8_t* out, size_t capacity)
{
    if (!pixels || !out || width <= 0 || height <= 0)
    {
        return 0;
    }
    size_t used = static_cast<size_t>(height) * 2;  // the rows' offsets
    if (used > capacity)
    {
        return 0;
    }
    for (int32_t y = 0; y < height; ++y)
    {
        if (used > UINT16_MAX)
        {
            return 0;
        }
        StoreU16(out + static_cast<size_t>(y) * 2, static_cast<uint16_t>(used));
        // The row as drawn: the picture's, or its mirror.
        const uint8_t* row = pixels + static_cast<size_t>(flipV ? height - 1 - y : y) * static_cast<size_t>(stride);
        auto pixel = [row, width, flipH](int32_t x) { return row + (flipH ? width - 1 - x : x) * 3; };
        // How many pixels from x on have an alpha for which `same` holds, up
        // to what one count holds.
        auto run = [&pixel, width](int32_t x, bool (*same)(uint8_t))
        {
            int32_t count = 0;
            while (x + count < width && count < 255 && same(pixel(x + count)[2]))
            {
                ++count;
            }
            return count;
        };
        for (int32_t x = 0; x < width;)
        {
            const int32_t pass = run(x, [](uint8_t a) { return a == 0; });
            const int32_t copy = run(x + pass, [](uint8_t a) { return a == 255; });
            const int32_t blend = run(x + pass + copy, [](uint8_t a) { return a != 0 && a != 255; });
            if (used + 3 + static_cast<size_t>(copy) * 2 + static_cast<size_t>(blend) * 3 > capacity)
            {
                return 0;
            }
            out[used++] = static_cast<uint8_t>(pass);
            out[used++] = static_cast<uint8_t>(copy);
            out[used++] = static_cast<uint8_t>(blend);
            x += pass;
            for (int32_t i = 0; i < copy; ++i, ++x)
            {
                out[used++] = pixel(x)[0];
                out[used++] = pixel(x)[1];
            }
            for (int32_t i = 0; i < blend; ++i, ++x)
            {
                std::memcpy(out + used, pixel(x), 3);
                used += 3;
            }
        }
    }
    return used;
}
// The rows of BlitRuns. Not inlined into its caller, which runs from flash:
// the loop must stay in IRAM.
[[gnu::noinline]] static DEKI_FAST_ATTR void RunRows_RGB565(const uint8_t* runs, uint16_t* target16,
                                                            int32_t targetWidth, int32_t destX, int32_t destY,
                                                            int32_t width, const BlitBounds& b)
{
    const TintTables& t = TintTablesFor(255, 255, 255);
    const int32_t rowEnd = std::min(destX + width, b.endX);
    for (int32_t py = b.startY; py < b.endY; py++)
    {
        const uint8_t* p = runs + LoadU16(runs + static_cast<size_t>(py - destY) * 2);
        uint16_t* dstRow = target16 + py * targetWidth;
        for (int32_t x = destX; x < rowEnd;)
        {
            x += p[0];
            const int32_t copy = p[1];
            const int32_t blend = p[2];
            p += 3;

            const int32_t left = std::max(x, b.startX);
            const int32_t right = std::min(x + copy, b.endX);
            if (left < right)
            {
                std::memcpy(dstRow + left, p + static_cast<size_t>(left - x) * 2,
                            static_cast<size_t>(right - left) * 2);
            }
            p += static_cast<size_t>(copy) * 2;
            x += copy;

            for (int32_t i = 0; i < blend; ++i, p += 3)
            {
                const int32_t px = x + i;
                if (px >= b.startX && px < b.endX)
                {
                    dstRow[px] = TintBlend565(t, Load565(p), p[2], dstRow[px]);
                }
            }
            x += blend;
        }
    }
}

// The rows of BlitRuns with a tint: each pixel copied goes through the
// tint's tables, or with an alpha tint is blended at the tint's alpha, as
// TintRows_RGB565 draws them. Not inlined, as RunRows_RGB565.
template <bool AlphaTint>
[[gnu::noinline]] static DEKI_FAST_ATTR void
TintRunRows_RGB565(const uint8_t* runs, uint16_t* target16, int32_t targetWidth, int32_t destX, int32_t destY,
                   int32_t width, const BlitBounds& b, const BlitParams& tint)
{
    const TintTables& t = TintTablesFor(tint.tintR, tint.tintG, tint.tintB);
    const uint32_t tintA = tint.tintA;
    const int32_t rowEnd = std::min(destX + width, b.endX);
    for (int32_t py = b.startY; py < b.endY; py++)
    {
        const uint8_t* p = runs + LoadU16(runs + static_cast<size_t>(py - destY) * 2);
        uint16_t* dstRow = target16 + py * targetWidth;
        for (int32_t x = destX; x < rowEnd;)
        {
            x += p[0];
            const int32_t copy = p[1];
            const int32_t blend = p[2];
            p += 3;

            const int32_t left = std::max(x, b.startX);
            const int32_t right = std::min(x + copy, b.endX);
            for (int32_t px = left; px < right; ++px)
            {
                const uint32_t v = Load565(p + static_cast<size_t>(px - x) * 2);
                if constexpr (AlphaTint)
                {
                    dstRow[px] = TintBlend565(t, v, tintA, dstRow[px]);  // opaque, at the tint's alpha
                }
                else
                {
                    dstRow[px] = static_cast<uint16_t>(t.pr[v >> 11] | t.pg[(v >> 5) & 0x3F] | t.pb[v & 0x1F]);
                }
            }
            p += static_cast<size_t>(copy) * 2;
            x += copy;

            for (int32_t i = 0; i < blend; ++i, p += 3)
            {
                const int32_t px = x + i;
                if (px < b.startX || px >= b.endX)
                {
                    continue;
                }
                uint32_t a = p[2];
                if constexpr (AlphaTint)
                {
                    a = Div255(a * tintA);
                    if (a == 0)
                    {
                        continue;
                    }
                }
                dstRow[px] = TintBlend565(t, Load565(p), a, dstRow[px]);
            }
            x += blend;
        }
    }
}

void BlitRuns(const uint8_t* runs, int32_t width, int32_t height, uint8_t* target, int32_t targetWidth,
              int32_t targetHeight, int32_t destX, int32_t destY, uint8_t tintR, uint8_t tintG, uint8_t tintB,
              uint8_t tintA)
{
    if (!runs || !target || width <= 0 || height <= 0 || tintA == 0)
    {
        return;
    }
    BlitBounds bounds;
    if (!ComputeClipBounds(destX, destY, width, height, targetWidth, targetHeight, bounds))
    {
        return;
    }
    BeforeWritingRows(target, bounds.endY);
    if (!t_Play && target == s_RecordTarget)
    {
        RecordedOp op;
        op.kind = RecordedOp::Kind::Runs;
        op.runs = runs;
        op.x = destX;
        op.y = destY;
        op.w = width;
        op.h = height;
        op.tint[0] = tintR;
        op.tint[1] = tintG;
        op.tint[2] = tintB;
        op.tint[3] = tintA;
        op.clip = GetCurrentClipRect();
        KeepOp(op, bounds.startY, bounds.endY);
        return;
    }
    PathScope scope(static_cast<uint64_t>(bounds.endX - bounds.startX) *
                    static_cast<uint64_t>(bounds.endY - bounds.startY));
    scope.path = BlitPath::RunRows;
    uint16_t* target16 = reinterpret_cast<uint16_t*>(target);
    if (tintR == 255 && tintG == 255 && tintB == 255 && tintA == 255)
    {
        RunRows_RGB565(runs, target16, targetWidth, destX, destY, width, bounds);
        return;
    }
    BlitParams tint;
    tint.tintR = tintR;
    tint.tintG = tintG;
    tint.tintB = tintB;
    tint.tintA = tintA;
    if (tintA != 255)
    {
        TintRunRows_RGB565<true>(runs, target16, targetWidth, destX, destY, width, bounds, tint);
    }
    else
    {
        TintRunRows_RGB565<false>(runs, target16, targetWidth, destX, destY, width, bounds, tint);
    }
}

// ============================================================================
// Rotated blit
// ============================================================================
// Inverse-maps the destination pixels of the rotated quad back into the source
// with a 16.16 fixed-point DDA: two adds per pixel, then the same pipeline as
// the scaled path. Each row of the quad's bounding box visits only its run of
// pixels that land inside the source, found once per row: the corners outside
// a turned sprite can be half the box.

struct RotatedBlitArgs
{
    int32_t startX, startY, endX, endY;  // destination rows/columns to visit
    int32_t rowSx, rowSy;                // 16.16 source coords of (startX, startY)
    int32_t dSxDx, dSyDx;                // per-column source step
    int32_t dSxDy, dSyDy;                // per-row source step
};

// a / b rounded toward minus infinity, and toward plus infinity, for b > 0
// and a of either sign.
static inline int64_t FloorDiv(int64_t a, int64_t b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

static inline int64_t CeilDiv(int64_t a, int64_t b)
{
    return a >= 0 ? (a + b - 1) / b : -(-a / b);
}

// Narrows the steps [k0, k1) to those at which the walk s + k * d (16.16)
// lands in [0, size) as a whole pixel: s >> 16 is in [0, size) exactly when s
// is in [0, size << 16). A straight walk enters and leaves that range once,
// so what is left is one run, empty when the walk misses it.
static inline void NarrowToInside(int32_t s, int32_t d, uint32_t size, int32_t& k0, int32_t& k1)
{
    const int64_t hi = static_cast<int64_t>(size) << 16;
    int64_t first = k0, end = k1;
    if (d > 0)
    {
        first = CeilDiv(-static_cast<int64_t>(s), d);
        end = CeilDiv(hi - s, d);
    }
    else if (d < 0)
    {
        first = FloorDiv(static_cast<int64_t>(s) - hi, -static_cast<int64_t>(d)) + 1;
        end = FloorDiv(s, -static_cast<int64_t>(d)) + 1;
    }
    else if (s < 0 || s >= hi)
    {
        end = first;
    }
    k0 = static_cast<int32_t>(std::max<int64_t>(k0, first));
    k1 = static_cast<int32_t>(std::max<int64_t>(k0, std::min<int64_t>(k1, end)));
}

// The run of a row's walk, starting at source coords (sx, sy), that lands
// inside a source of srcW x srcH pixels: the steps [k0, k1) from a.startX.
// False when there is none. Out of line, once, in IRAM: it runs once a row,
// and its 64-bit divisions would make every loop it was inlined into larger.
[[gnu::noinline]] static DEKI_FAST_ATTR bool InsideRun(int32_t sx, int32_t sy, const RotatedBlitArgs& a, uint32_t srcW,
                                                       uint32_t srcH, int32_t& k0, int32_t& k1)
{
    k0 = 0;
    k1 = a.endX - a.startX;
    NarrowToInside(sx, a.dSxDx, srcW, k0, k1);
    NarrowToInside(sy, a.dSyDx, srcH, k0, k1);
    return k0 < k1;
}

// Where a walk from s is after k steps of d: what k additions give.
static inline int32_t StepsAlong(int32_t s, int32_t d, int32_t k)
{
    return static_cast<int32_t>(static_cast<uint32_t>(s) + static_cast<uint32_t>(k) * static_cast<uint32_t>(d));
}

template <SrcKind SK, Deki::ColorFormat F>
static void RotatedBlitT(const Source& source, uint8_t* target, int32_t targetWidth, const RotatedBlitArgs& a,
                         const BlitParams& P)
{
    const int32_t stride = SourceStride(source);
    const int32_t bpp = source.bytesPerPixel;
    const uint32_t srcW = static_cast<uint32_t>(DrawnWidth(source)), srcH = static_cast<uint32_t>(DrawnHeight(source));
    // A stretched strip's every column (or row) is its one.
    const int32_t keepX = source.stretchWidth > 0 ? 0 : -1;
    const int32_t keepY = source.stretchHeight > 0 ? 0 : -1;

    int32_t rowSx = a.rowSx, rowSy = a.rowSy;
    for (int32_t py = a.startY; py < a.endY; py++, rowSx += a.dSxDy, rowSy += a.dSyDy)
    {
        int32_t k0, k1;
        if (!InsideRun(rowSx, rowSy, a, srcW, srcH, k0, k1))
        {
            continue;
        }
        int32_t sx = StepsAlong(rowSx, a.dSxDx, k0), sy = StepsAlong(rowSy, a.dSyDx, k0);
        const size_t rowIdx = static_cast<size_t>(py) * static_cast<size_t>(targetWidth);
        for (int32_t px = a.startX + k0; px < a.startX + k1; px++, sx += a.dSxDx, sy += a.dSyDx)
        {
            int32_t ix = (sx >> 16) & keepX;
            int32_t iy = (sy >> 16) & keepY;
            if (P.flips)
            {
                ApplyFlips(source, ix, iy);
            }
            CompositePixel<SK, F, false>(source, source.pixels + iy * stride + ix * bpp, target, rowIdx + px, px, py,
                                         P);
        }
    }
}

// RGB565 or RGB565A8 -> RGB565, rotated, any tint and alpha tint; no key,
// dither, flips or stretch: a turned sprite onto the frame. TintRows'
// arithmetic over each row's run inside the source, where CompositePixel per
// pixel cost several times as much. Out of line in IRAM: inlined into the
// dispatch it would run from flash. Every other rotated blit runs
// RotatedBlitT from flash: an instance in IRAM for each source and target
// format took internal memory a board has little of, for blits it rarely
// draws.
template <bool SourceAlpha, int32_t Bpp>
[[gnu::noinline]] static DEKI_FAST_ATTR void TintRotatedRows_RGB565(const Source& source, uint16_t* target16,
                                                                    int32_t targetWidth, const RotatedBlitArgs& a,
                                                                    const BlitParams& p)
{
    const TintTables& t = TintTablesFor(p.tintR, p.tintG, p.tintB);
    const int32_t stride = SourceStride(source);
    const uint32_t srcW = static_cast<uint32_t>(source.width), srcH = static_cast<uint32_t>(source.height);
    const bool alphaTint = p.hasAlphaTint;
    const uint32_t tintA = p.tintA;

    int32_t rowSx = a.rowSx, rowSy = a.rowSy;
    for (int32_t py = a.startY; py < a.endY; py++, rowSx += a.dSxDy, rowSy += a.dSyDy)
    {
        int32_t k0, k1;
        if (!InsideRun(rowSx, rowSy, a, srcW, srcH, k0, k1))
        {
            continue;
        }
        int32_t sx = StepsAlong(rowSx, a.dSxDx, k0), sy = StepsAlong(rowSy, a.dSyDx, k0);
        uint16_t* dp = target16 + static_cast<size_t>(py) * static_cast<size_t>(targetWidth) + a.startX + k0;
        for (int32_t k = k0; k < k1; k++, dp++, sx += a.dSxDx, sy += a.dSyDx)
        {
            const uint8_t* sp = source.pixels + (sy >> 16) * stride + (sx >> 16) * Bpp;
            uint32_t alpha = 255;
            if constexpr (SourceAlpha)
            {
                alpha = sp[2];
                if (alpha == 0)
                {
                    continue;
                }
            }
            if (alphaTint)
            {
                alpha = Div255(alpha * tintA);
                if (alpha == 0)
                {
                    continue;
                }
            }
            TintPixel565(t, Load565(sp), alpha, dp);
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

bool CoversRect(const Source& source, int32_t screenX, int32_t screenY, float scaleX, float scaleY, float rotation,
                float pivotX, float pivotY, uint8_t tintA, int32_t left, int32_t top, int32_t right, int32_t bottom)
{
    // Every pixel of an opaque source writes its colour (a tint changes the
    // colour, not that). Alpha-only sources and colour keys leave holes.
    if (!source.pixels || source.width <= 0 || source.height <= 0 || source.bytesPerPixel < 2 || source.hasAlpha ||
        source.hasChromaKey || tintA != 255 || rotation != 0.0f)
    {
        return false;
    }

    // Placed as Blit places an unrotated blit.
    const float destWidth = DrawnWidth(source) * scaleX;
    const float destHeight = DrawnHeight(source) * scaleY;
    if (destWidth <= 0 || destHeight <= 0)
    {
        return false;
    }
    const int32_t destX = screenX - static_cast<int32_t>(std::floor(destWidth * pivotX));
    const int32_t destY = screenY - static_cast<int32_t>(std::floor(destHeight * pivotY));
    if (destX > left || destY > top || destX + static_cast<int32_t>(destWidth) < right ||
        destY + static_cast<int32_t>(destHeight) < bottom)
    {
        return false;
    }

    // An unset clip limits nothing; the target's own edges are not the rect's.
    const ClipRect clip = GetCurrentClipRect();
    return !clip.IsSet() || (clip.left <= left && clip.top <= top && clip.right >= right && clip.bottom >= bottom);
}

bool VisibleBounds(const Source& source, int32_t& left, int32_t& top, int32_t& right, int32_t& bottom)
{
    const int32_t w = source.width, h = source.height;
    // An alpha-only source (a shape's mask, drawn in its tint) holds just the
    // alpha, at byte 0.
    if (!source.pixels || w <= 0 || h <= 0 || source.bytesPerPixel < 1 || source.hasChromaKey || source.flipD ||
        source.stretchWidth > 0 || source.stretchHeight > 0)
    {
        return false;
    }
    const size_t alphaAt = source.bytesPerPixel == 1 ? 0 : source.alphaOffset;
    left = top = 0;
    right = w;
    bottom = h;
    if (!source.hasAlpha && source.bytesPerPixel > 1)
    {
        return true;
    }
    const int32_t stride = SourceStride(source);
    const int32_t bpp = source.bytesPerPixel;
    int32_t minX = w, maxX = -1, minY = h, maxY = -1;
    for (int32_t y = 0; y < h; ++y)
    {
        const uint8_t* row = source.pixels + static_cast<size_t>(y) * stride + alphaAt;
        int32_t first = -1, last = -1;
        for (int32_t x = 0; x < w; ++x)
        {
            if (row[static_cast<size_t>(x) * bpp] != 0)
            {
                first = x;
                break;
            }
        }
        if (first < 0)
        {
            continue;
        }
        for (int32_t x = w - 1; x >= first; --x)
        {
            if (row[static_cast<size_t>(x) * bpp] != 0)
            {
                last = x;
                break;
            }
        }
        minX = std::min(minX, first);
        maxX = std::max(maxX, last);
        minY = std::min(minY, y);
        maxY = y;
    }
    if (maxY < 0)
    {
        left = right = top = bottom = 0;
        return true;
    }
    left = minX;
    right = maxX + 1;
    top = minY;
    bottom = maxY + 1;
    if (source.flipH)
    {
        const int32_t l = w - right;
        right = w - left;
        left = l;
    }
    if (source.flipV)
    {
        const int32_t t = h - bottom;
        bottom = h - top;
        top = t;
    }
    return true;
}

bool OpaqueInterior(const Source& source, int32_t& left, int32_t& top, int32_t& right, int32_t& bottom)
{
    const int32_t w = source.width, h = source.height;
    // An alpha-only source (a shape's mask, drawn in its tint) holds just the
    // alpha, at byte 0.
    if (!source.pixels || w <= 0 || h <= 0 || source.bytesPerPixel < 1 || source.hasChromaKey || source.flipD ||
        source.stretchWidth > 0 || source.stretchHeight > 0)
    {
        return false;
    }
    const size_t alphaAt = source.bytesPerPixel == 1 ? 0 : source.alphaOffset;
    if (!source.hasAlpha && source.bytesPerPixel > 1)
    {
        left = top = 0;
        right = w;
        bottom = h;
        return true;
    }

    // Each row's longest run of opaque pixels, [start, end).
    struct Runs
    {
        int16_t* data = nullptr;
        ~Runs() { Deki::Memory::Free(data); }
    } runs;
    const int16_t* spans = source.alphaRowSpans;
    if (!spans)
    {
        runs.data = static_cast<int16_t*>(
            Deki::Memory::Allocate(static_cast<size_t>(h) * 2 * sizeof(int16_t), Deki::Memory::External));
        if (!runs.data)
        {
            return false;
        }
        const int32_t stride = SourceStride(source);
        const int32_t bpp = source.bytesPerPixel;
        for (int32_t y = 0; y < h; ++y)
        {
            const uint8_t* row = source.pixels + static_cast<size_t>(y) * stride + alphaAt;
            int32_t best = 0, bestStart = 0, start = -1;
            for (int32_t x = 0; x <= w; ++x)
            {
                const bool opaque = x < w && row[static_cast<size_t>(x) * bpp] == 255;
                if (opaque && start < 0)
                {
                    start = x;
                }
                else if (!opaque && start >= 0)
                {
                    if (x - start > best)
                    {
                        best = x - start;
                        bestStart = start;
                    }
                    start = -1;
                }
            }
            runs.data[static_cast<size_t>(y) * 2] = static_cast<int16_t>(bestStart);
            runs.data[static_cast<size_t>(y) * 2 + 1] = static_cast<int16_t>(bestStart + best);
        }
        spans = runs.data;
    }

    // The rows taken together keep the columns every one of them covers: the
    // biggest such block of consecutive rows.
    int64_t bestArea = 0;
    for (int32_t y0 = 0; y0 < h; ++y0)
    {
        int32_t lo = spans[y0 * 2], hi = spans[y0 * 2 + 1];
        for (int32_t y1 = y0; y1 < h && hi > lo; ++y1)
        {
            lo = std::max<int32_t>(lo, spans[y1 * 2]);
            hi = std::min<int32_t>(hi, spans[y1 * 2 + 1]);
            const int64_t area = static_cast<int64_t>(hi - lo) * (y1 - y0 + 1);
            if (hi > lo && area > bestArea)
            {
                bestArea = area;
                left = lo;
                right = hi;
                top = y0;
                bottom = y1 + 1;
            }
        }
    }
    if (bestArea == 0)
    {
        return false;
    }
    if (source.flipH)
    {
        const int32_t l = w - right;
        right = w - left;
        left = l;
    }
    if (source.flipV)
    {
        const int32_t t = h - bottom;
        bottom = h - top;
        top = t;
    }
    return true;
}

bool BlitArea(const Source& source, int32_t screenX, int32_t screenY, float scaleX, float scaleY, float rotation,
              float pivotX, float pivotY, int32_t& left, int32_t& top, int32_t& right, int32_t& bottom)
{
    // Placed as Blit places it, unrotated or rotated.
    const float destWidth = DrawnWidth(source) * scaleX;
    const float destHeight = DrawnHeight(source) * scaleY;
    if (!source.pixels || source.width <= 0 || source.height <= 0 || destWidth <= 0 || destHeight <= 0)
    {
        return false;
    }
    if (rotation == 0.0f)
    {
        left = screenX - static_cast<int32_t>(std::floor(destWidth * pivotX));
        top = screenY - static_cast<int32_t>(std::floor(destHeight * pivotY));
        right = left + static_cast<int32_t>(destWidth);
        bottom = top + static_cast<int32_t>(destHeight);
        return right > left && bottom > top;
    }
    const float cosR = std::cos(rotation);
    const float sinR = -std::sin(rotation);  // the screen's Y runs down
    const float pivotSX = destWidth * pivotX;
    const float pivotSY = destHeight * pivotY;
    const float xs[2] = { -pivotSX, destWidth - pivotSX };
    const float ys[2] = { -pivotSY, destHeight - pivotSY };
    float minX = 0, maxX = 0, minY = 0, maxY = 0;
    for (int i = 0; i < 4; i++)
    {
        const float rx = xs[i & 1] * cosR - ys[i >> 1] * sinR;
        const float ry = xs[i & 1] * sinR + ys[i >> 1] * cosR;
        minX = i == 0 ? rx : std::min(minX, rx);
        maxX = i == 0 ? rx : std::max(maxX, rx);
        minY = i == 0 ? ry : std::min(minY, ry);
        maxY = i == 0 ? ry : std::max(maxY, ry);
    }
    left = screenX + static_cast<int32_t>(std::floor(minX));
    right = screenX + static_cast<int32_t>(std::floor(maxX + 1));
    top = screenY + static_cast<int32_t>(std::floor(minY));
    bottom = screenY + static_cast<int32_t>(std::floor(maxY + 1));
    return right > left && bottom > top;
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

    float destWidth = DrawnWidth(source) * scaleX;
    float destHeight = DrawnHeight(source) * scaleY;

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

    // Rotated. `rotation` is in radians, counter-clockwise on screen as
    // everywhere in the engine; the screen's Y runs down.
    float cosR = std::cos(rotation);
    float sinR = -std::sin(rotation);

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
    BeforeWritingRows(target, endY);
    if (!t_Play && target == s_RecordTarget)
    {
        RecordedOp op;
        op.kind = RecordedOp::Kind::Turned;
        op.source = KeptSource(source);
        op.x = screenX;
        op.y = screenY;
        op.scaleX = scaleX;
        op.scaleY = scaleY;
        op.rotation = rotation;
        op.pivotX = pivotX;
        op.pivotY = pivotY;
        op.tint[0] = tintR;
        op.tint[1] = tintG;
        op.tint[2] = tintB;
        op.tint[3] = tintA;
        op.dither = useOrderedDither;
        op.clip = clip;
        KeepOp(op, startY, endY);
        return;
    }
    PathScope scope(static_cast<uint64_t>(endX - startX) * static_cast<uint64_t>(endY - startY));

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

    // Fixed-point inverse mapping. For destination pixel (px, py), at its
    // centre (dx, dy = px, py - screen position + 0.5), so a turn reads the
    // same pixels as the picture it turns:
    //   localX =  dx*cosR + dy*sinR + pivotSX
    //   localY = -dx*sinR + dy*cosR + pivotSY
    //   srcX   = localX * srcW / destWidth,  srcY = localY * srcH / destHeight
    // which is affine in (px, py), so it is evaluated once at the box corner and
    // stepped per column and per row in 16.16.
    // In the drawn size: a stretched strip's one column or row is read
    // wherever the whole picture would be (RotatedBlitT).
    const float sxScale = DrawnWidth(source) / destWidth;
    const float syScale = DrawnHeight(source) / destHeight;
    const float dx0 = static_cast<float>(startX - screenX) + 0.5f;
    const float dy0 = static_cast<float>(startY - screenY) + 0.5f;
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

    // A frame drawn by the sink: the walk RotatedBlitT makes, from the box's
    // corner, over the drawn size.
    if (target == s_SinkTarget)
    {
        SinkBlit blit;
        blit.source = &source;
        blit.left = startX;
        blit.top = startY;
        blit.right = endX;
        blit.bottom = endY;
        blit.originX = startX;
        blit.originY = startY;
        blit.originSx = args.rowSx;
        blit.originSy = args.rowSy;
        blit.dSxDx = args.dSxDx;
        blit.dSyDx = args.dSyDx;
        blit.dSxDy = args.dSxDy;
        blit.dSyDy = args.dSyDy;
        blit.sampleWidth = DrawnWidth(source);
        blit.sampleHeight = DrawnHeight(source);
        blit.repeatColumn = source.stretchWidth > 0;
        blit.repeatRow = source.stretchHeight > 0;
        blit.tintR = tintR;
        blit.tintG = tintG;
        blit.tintB = tintB;
        blit.tintA = tintA;
        blit.orderedDither = p.dither;
        blit.clip = clip;
        if (DrawOnSink(blit))
        {
            return;
        }
    }

    const SrcKind kind = KindOf(source);
    if (targetFormat == Deki::ColorFormat::RGB565 && (kind == SrcKind::RGB565 || kind == SrcKind::RGB565A8) &&
        !p.hasKey && !p.dither && !p.flips && source.stretchWidth == 0 && source.stretchHeight == 0)
    {
        scope.path = BlitPath::TintRotatedRows;
        uint16_t* target16 = reinterpret_cast<uint16_t*>(target);
        if (kind == SrcKind::RGB565A8 && source.hasAlpha)
        {
            TintRotatedRows_RGB565<true, 3>(source, target16, targetWidth, args, p);
        }
        else if (kind == SrcKind::RGB565A8)
        {
            TintRotatedRows_RGB565<false, 3>(source, target16, targetWidth, args, p);
        }
        else
        {
            TintRotatedRows_RGB565<false, 2>(source, target16, targetWidth, args, p);
        }
        return;
    }
    scope.path = BlitPath::GenericRotated;
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
