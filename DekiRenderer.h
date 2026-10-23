#pragma once

#include <cstdint>
#include <deki/Engine.h>
#include "FrameCamera.h"

namespace Deki
{
class Scene;
}

namespace DekiRendering
{
class CameraComponent;
class DirtyRegion;

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
    // Ask the renderer to record which pixels of `buffer` this frame changes
    // (see DekiRenderer::GetLastFrameDirty). Set by DekiRenderSystem when the
    // project's dirty-rect tracking is on; off for edit-mode renders.
    bool trackDirty = false;
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

    /// The pixels the last Render() changed in ctx.buffer, when that render had
    /// ctx.trackDirty set. nullptr when it did not or the renderer cannot say;
    /// the caller then treats the whole frame as changed. Valid until the next
    /// Render().
    virtual const DirtyRegion* GetLastFrameDirty() const { return nullptr; }

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
};

}  // namespace DekiRendering
