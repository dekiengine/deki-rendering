#pragma once
#include <cstdint>

namespace Deki
{
class Object;
}

namespace DekiRendering
{
struct RenderContext;

// The hooks a RenderPass can implement, as bits for RenderPass::HookMask().
namespace RenderPassHooks
{
enum : uint32_t
{
    BeginFrame = 1u << 0,
    PreExecute = 1u << 1,
    Execute = 1u << 2,
    PostExecute = 1u << 3,
    EndFrame = 1u << 4,
    All = BeginFrame | PreExecute | Execute | PostExecute | EndFrame,
};
}

/// Base class for custom render passes. Add one to Standard2DRenderer to give
/// objects custom behaviour without changing the renderer.
///
/// Execute() runs per object before its children are rendered; PostExecute()
/// runs per object after them, in reverse order.
///
///   class MyEffectPass : public RenderPass {
///       void Execute(Deki::Object* obj, RenderContext& ctx) override {
///           auto* effect = obj->GetComponent<MyEffectComponent>();
///           if (!effect) return;
///           // Apply effect...
///       }
///   };
///
///   standard2DRenderer.AddPass(&myEffectPass);
///
/// Contract:
/// - Override HookMask() to name the hooks you implement; the renderer then
///   skips this pass for the others (each would be a virtual call per object).
/// - ctx.cam is the frame's world-to-screen snapshot (see FrameCamera), taken
///   once after every BeginFrame. Map through it in Execute/PreExecute rather
///   than calling ctx.camera per object or per tile.
/// - Dirty-rect tracking (ctx.trackDirty): QuadBlit blits into ctx.buffer are
///   recorded automatically. A pass that writes ctx.buffer any other way must
///   call QuadBlit::MarkDirty (or MarkAllDirty) for the pixels it touched. A
///   pass that swaps ctx.buffer in BeginFrame needs nothing: the renderer then
///   treats the whole frame as changed.
class RenderPass
{
public:
    virtual ~RenderPass() = default;

    /// Which hooks this pass implements, as RenderPassHooks bits. Read at the
    /// start of every frame. The default is every hook; declaring only those
    /// you override saves the empty virtual calls for the rest.
    virtual uint32_t HookMask() const { return RenderPassHooks::All; }

    /// Called once per frame, before any object renders. The pass may change
    /// ctx (e.g. swap ctx.buffer for a scratch buffer) to set the render target
    /// for the whole frame; every later per-object hook and the built-in
    /// render then see the changed ctx.
    virtual void BeginFrame(RenderContext& ctx) {}

    /// Called per object before the built-in render (sprite blit, clip push).
    /// To redirect one object's blit, change ctx.buffer (and width, height and
    /// format if needed) here and restore it in PostExecute.
    virtual void PreExecute(Deki::Object* obj, RenderContext& ctx) {}

    /// Called per object after the built-in render, before its children.
    virtual void Execute(Deki::Object* obj, RenderContext& ctx) {}

    /// Called per object after its children are rendered.
    virtual void PostExecute(Deki::Object* obj, RenderContext& ctx) {}

    /// Called once per frame, after all objects have rendered. A pass can
    /// composite here: read the scratch buffers it filled during the frame and
    /// write into the original framebuffer it saved in BeginFrame.
    virtual void EndFrame(RenderContext& ctx) {}
};

/// Custom sorting: returns true if the object is a sortable render item, and
/// sets outOrder. Add with Standard2DRenderer::AddSortingCallback().
using SortingCallback = bool (*)(Deki::Object* obj, int32_t& outOrder);

}  // namespace DekiRendering