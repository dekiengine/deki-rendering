#pragma once
#include <cstdint>

namespace Deki
{
class Object;
class Scene;
}  // namespace Deki

namespace DekiRendering
{
class DirtyRegion;
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
/// - Redrawing only what changed: a pass draws a frame whole unless it says
///   where it draws (ReportsChanges, below).
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

    /// Whether Execute has anything to do for `obj` this frame. Drawn in
    /// strips, the renderer asks once, in the frame's first strip, and calls
    /// Execute in the others only where the answer was yes: a pass that draws
    /// a few kinds of object (particles, a tilemap) then does not look at
    /// every object in every strip. The default is yes.
    virtual bool WantsObject(Deki::Object* obj) const
    {
        (void)obj;
        return true;
    }

    /// Called per object after its children are rendered.
    virtual void PostExecute(Deki::Object* obj, RenderContext& ctx) {}

    /// Called once per frame, after all objects have rendered. A pass can
    /// composite here: read the scratch buffers it filled during the frame and
    /// write into the original framebuffer it saved in BeginFrame.
    virtual void EndFrame(RenderContext& ctx) {}

    /// Whether this pass draws `scene` correctly when the frame is drawn strip
    /// by strip (RenderingProjectSettings::bandRows), of rows or of columns.
    /// Each strip is a Render() of its own: BeginFrame and EndFrame run once
    /// per strip, ctx.buffer holds rows [ctx.viewTop, ctx.viewTop +
    /// ctx.height) and columns [ctx.viewLeft, ctx.viewLeft + ctx.width) of a
    /// view ctx.viewWidth by ctx.viewHeight, and ctx.cam is moved to the
    /// strip. The
    /// default is yes for a pass with only per-object hooks, which maps
    /// through ctx.cam as the contract above asks, and no for one with
    /// BeginFrame or EndFrame, which usually works on the whole frame.
    /// Override it to say otherwise. While any pass says no, whole frames are
    /// drawn instead. Asked every frame before drawing in strips.
    virtual bool DrawsInStrips(Deki::Scene* scene) const
    {
        (void)scene;
        return (HookMask() & (RenderPassHooks::BeginFrame | RenderPassHooks::EndFrame)) == 0;
    }

    /// Redrawing only what changed (RenderingProjectSettings::redrawChanges):
    /// whether this pass says where its drawing changed, with AddChanges and
    /// FinishChanges. The default is no: while a pass that cannot say wants
    /// any object, every frame is drawn whole, which is always right, only
    /// slower. Say yes only for a pass with Execute as its one hook whose
    /// Execute draws through QuadBlit (or keeps to its clip): a frame drawn in
    /// parts sets the clip to each part, and the pass's Execute must not write
    /// outside it.
    virtual bool ReportsChanges() const { return false; }

    /// Adds to `changed` the pixels where this pass draws `obj` differently
    /// from the last frame: where it drew then and where it draws now, in
    /// pixels of the whole view (ctx is the whole view; ctx.buffer may be
    /// null). Called once a frame, before anything is drawn, for each object
    /// WantsObject says yes to, when ReportsChanges() is true.
    virtual void AddChanges(Deki::Object* obj, const RenderContext& ctx, DirtyRegion& changed)
    {
        (void)obj;
        (void)ctx;
        (void)changed;
    }

    /// After every AddChanges of the frame: adds where the pass drew last
    /// frame for objects it was not asked about this time (destroyed, made
    /// inactive, no longer wanted), which are left as they were otherwise.
    virtual void FinishChanges(const RenderContext& ctx, DirtyRegion& changed)
    {
        (void)ctx;
        (void)changed;
    }
};

/// Custom sorting: returns true if the object is a sortable render item, and
/// sets outOrder. Add with Standard2DRenderer::AddSortingCallback().
using SortingCallback = bool (*)(Deki::Object* obj, int32_t& outOrder);

}  // namespace DekiRendering