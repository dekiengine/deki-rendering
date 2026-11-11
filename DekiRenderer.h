#pragma once

#include <cstdint>
#include <vector>
#include <deki/Engine.h>
#include <deki/providers/Rect.h>
#include "FrameCamera.h"

namespace Deki
{
class Scene;
}

namespace DekiRendering
{
class CameraComponent;
class DirtyRegion;
class Lighting2D;

/// A clear of the frame's buffer that its caller leaves to the renderer
/// (DekiRenderer::ClearsLazily): run(owner) clears it. runLit, when given,
/// clears it in the clear colour lit by `light`, which must light the view the
/// same all over (Lighting2D::Uniform): what Lighting2D::Apply would make of
/// a frame holding only the clear, without the pass over it. pixel, when
/// given, writes the clear's one pixel in the frame's format to `out` (4
/// bytes) and says true, when the clear is of the whole frame: a render pass
/// that writes every pixel of the frame itself takes it instead
/// (QuadBlit::TakePendingClear) and draws it where it draws nothing.
/// colour, when given, writes the same pixel and says true whatever the clear
/// covers: the whole frame, or the part of it being drawn
/// (RenderContext::redraw). A pass that is about to write every pixel the
/// clear covers reads it (QuadBlit::PendingClearColour) and drops the clear.
struct DeferredClear
{
    void (*run)(void* owner) = nullptr;
    void* owner = nullptr;
    void (*runLit)(void* owner, Lighting2D& light, int32_t viewLeft, int32_t viewTop) = nullptr;
    bool (*pixel)(void* owner, uint8_t* out) = nullptr;
    bool (*colour)(void* owner, uint8_t* out) = nullptr;
};

/// Everything a renderer or render pass needs to produce a frame.
struct RenderContext
{
    CameraComponent* camera;
    uint8_t* buffer;
    int32_t width;
    int32_t height;
    Deki::ColorFormat format;
    // World-to-screen snapshot for this frame, captured by Standard2DRenderer
    // from `camera` and the target size after the passes' BeginFrame. Use it
    // instead of camera->WorldToScreen in per-object and per-tile code. Last
    // and defaulted so five-field aggregate initialisers still work. A pass
    // that redirects an object to a target of another size must refresh it
    // (camera->CaptureFrameCamera) for that object.
    FrameCamera cam = {};
    // Strip rendering: `buffer` holds rows [viewTop, viewTop + height) and
    // columns [viewLeft, viewLeft + width) of a view viewWidth by viewHeight.
    // A strip of rows is the view's full width, a strip of columns its full
    // height (DekiRenderSystem cuts strips along the panel's refresh).
    // Standard2DRenderer captures `cam` for the whole view and moves it up by
    // viewTop and left by viewLeft, so whatever maps through `cam` lands in
    // the strip, and QuadBlit clips to it. viewWidth and viewHeight 0 mean the
    // buffer is the whole view.
    int32_t viewTop = 0;
    int32_t viewHeight = 0;
    int32_t viewLeft = 0;
    int32_t viewWidth = 0;
    // Nonzero while one frame is drawn strip by strip: each renderer's
    // content from an earlier strip of the frame with this id is reused
    // rather than produced again.
    uint32_t contentFrame = 0;
    // The clear of `buffer` DekiRenderSystem left to a renderer that
    // ClearsLazily(), or null. The renderer runs it once, before its first
    // write into `buffer`, or skips it when that write covers every pixel
    // opaquely: the clear would only be drawn over. It leaves it with
    // QuadBlit (SetPendingClear), so a blit or a render pass that writes
    // there runs it first.
    const DeferredClear* clear = nullptr;
    // Redrawing only what changed: the part of `buffer` to draw, in its own
    // pixels. Nothing outside it is written, and the clear covers only it.
    // Empty (the default): all of `buffer`.
    Deki::Rect redraw = {};
    // FindChanges: whether the caller can shift its frame (it keeps the last
    // one whole), so a camera that moved by whole pixels need not redraw it.
    bool scrollable = false;
    // FindChanges, when it scrolls: how far the last frame's pixels move on
    // screen this frame (right and down positive). A pass that reports its own
    // changes sees it in AddChanges.
    int32_t scrollX = 0;
    int32_t scrollY = 0;
};

/// Base class for renderers. Subclass it for a custom rendering strategy;
/// Standard2DRenderer is the default. A renderer has full control: it can use
/// RenderPass objects, compose other renderers, or do something else entirely.
///
///   class MyRenderer : public DekiRenderer {
///       void Render(Scene* scene, const RenderContext& ctx) override {
///           // Custom rendering logic
///       }
///   };
///
///   renderSystem.SetRenderer(&myRenderer);
class DekiRenderer
{
public:
    virtual ~DekiRenderer() = default;

    /// The renderer's type id, for downcasting without RTTI. Each subclass
    /// defines a unique `static constexpr uint32_t kRendererTypeID`.
    virtual uint32_t GetRendererType() const = 0;

    /// Renders `scene` into the buffer `ctx` describes.
    virtual void Render(Deki::Scene* scene, const RenderContext& ctx) = 0;

    /// Whether this renderer draws `scene` correctly strip by strip
    /// (RenderContext::viewTop, viewHeight, viewLeft and viewWidth), in strips
    /// of rows or of columns: each strip's Render() must write into its pixels
    /// exactly what a whole frame writes there. Asked
    /// every frame before drawing in strips. While it says no,
    /// DekiRenderSystem draws whole frames instead, in a framebuffer it
    /// allocates for them. The default is no, so a renderer that knows nothing
    /// of strips is never drawn wrong.
    virtual bool DrawsInStrips(Deki::Scene* scene) const
    {
        (void)scene;
        return false;
    }

    /// Whether Render(ctx) is known, before it runs, to write every pixel of
    /// ctx.buffer with opaque content, so whatever the buffer held first never
    /// shows. DekiRenderSystem then skips clearing it. Asked for each strip
    /// of a frame drawn in strips. The default is no.
    virtual bool CoversView(const RenderContext& ctx) const
    {
        (void)ctx;
        return false;
    }

    /// Whether Render() takes over the clear of a whole frame
    /// (RenderContext::clear) instead of DekiRenderSystem clearing first. The
    /// default is no.
    virtual bool ClearsLazily() const { return false; }

    /// Redrawing only what changed (RenderingProjectSettings::redrawChanges).
    /// Works out, before anything is drawn, where `scene` draws differently
    /// from the last frame this was asked about, and adds those pixels of the
    /// whole view to `changed` (SetFull for all of them). The Render() calls
    /// that follow with the same ctx.contentFrame draw that frame, each only
    /// within its ctx.redraw. ctx is the whole view; its buffer may be null.
    /// Returns false when it cannot tell (the default): the frame is then
    /// drawn and sent whole, and the next call compares with nothing.
    virtual bool FindChanges(Deki::Scene* scene, const RenderContext& ctx, DirtyRegion& changed)
    {
        (void)scene;
        (void)ctx;
        (void)changed;
        return false;
    }

    /// After FindChanges with ctx.scrollable: true when the camera moved by
    /// whole pixels and nothing else needs it redrawn, so the last frame's
    /// pixels move by (dx, dy) on screen (right and down positive). The caller
    /// shifts its frame by that much before drawing what `changed` holds: the
    /// edge the move uncovers, and what changed besides. The default is no.
    /// With ScrolledBy, the rects of the view whose pixels stay where they
    /// are as the rest moves: what stays on screen (a HUD), opaque and drawn
    /// the same, is already right there. Empty for none.
    virtual const std::vector<Deki::Rect>& ScrollPins() const
    {
        static const std::vector<Deki::Rect> kNone;
        return kNone;
    }

    virtual bool ScrolledBy(int32_t& dx, int32_t& dy) const
    {
        (void)dx;
        (void)dy;
        return false;
    }
};

}  // namespace DekiRendering
