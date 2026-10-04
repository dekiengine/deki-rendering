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
};

}  // namespace DekiRendering
