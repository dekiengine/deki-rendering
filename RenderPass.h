#pragma once
#include <cstdint>

// Forward declarations
namespace Deki { class Object; }
struct RenderContext;

/**
 * @brief Base class for custom render passes
 *
 * Register a RenderPass on Standard2DRenderer to add custom
 * per-object behavior without modifying the renderer itself.
 *
 * Execute() is called per-object before children are rendered.
 * PostExecute() is called per-object after children are rendered (reverse order).
 *
 * Usage:
 * @code
 * class MyEffectPass : public RenderPass {
 *     void Execute(Deki::Object* obj, RenderContext& ctx) override {
 *         auto* effect = obj->GetComponent<MyEffectComponent>();
 *         if (!effect) return;
 *         // Apply effect...
 *     }
 * };
 *
 * standard2DRenderer.AddPass(&myEffectPass);
 * @endcode
 *
 * Contract notes:
 * - Override HookMask() to name the hooks you implement; the renderer then
 *   skips this pass entirely for the others (a per-object virtual call each).
 * - ctx.cam is the frame's world-to-screen snapshot (see FrameCamera), taken
 *   once after every BeginFrame. Map through it in Execute/PreExecute rather
 *   than calling ctx.camera per object or per tile.
 * - Dirty-rect tracking (ctx.trackDirty): blits through QuadBlit into
 *   ctx.buffer are recorded automatically. A pass that writes ctx.buffer any
 *   other way must call QuadBlit::MarkDirty (or MarkAllDirty) for the pixels
 *   it touched. A pass that swaps ctx.buffer in BeginFrame needs nothing: the
 *   renderer then treats the whole frame as changed.
 */
namespace RenderPassHooks
{
enum : uint32_t
{
    BeginFrame  = 1u << 0,
    PreExecute  = 1u << 1,
    Execute     = 1u << 2,
    PostExecute = 1u << 3,
    EndFrame    = 1u << 4,
    All         = BeginFrame | PreExecute | Execute | PostExecute | EndFrame,
};
}

class RenderPass
{
public:
    virtual ~RenderPass() = default;

    /**
     * @brief Which hooks this pass implements (RenderPassHooks bits).
     *
     * Read at the start of every frame. The default keeps every hook, so
     * existing passes behave as before; declaring only what you override
     * removes the empty virtual calls for the rest.
     */
    virtual uint32_t HookMask() const { return RenderPassHooks::All; }

    /**
     * @brief Called once per frame, before any object renders.
     *
     * Pass may mutate ctx (e.g. swap ctx.buffer to a scratch buffer) to
     * install a default render target for the whole frame. The mutated
     * ctx is then propagated to every subsequent per-object hook and to
     * the built-in render via Standard2DRenderer's frame-scoped context.
     */
    virtual void BeginFrame(RenderContext& ctx) {}

    /**
     * @brief Called per-object BEFORE built-in render (sprite blit, clip push)
     *
     * Use this to redirect a single object's blit by mutating ctx.buffer
     * (and width/height/format if needed). Restore in PostExecute.
     */
    virtual void PreExecute(Deki::Object* obj, RenderContext& ctx) {}

    /**
     * @brief Called per-object before children are rendered (after built-in render)
     * @param obj The current object being rendered
     * @param ctx Render context with camera, buffer, and format info
     */
    virtual void Execute(Deki::Object* obj, RenderContext& ctx) {}

    /**
     * @brief Called per-object after children are rendered
     * @param obj The current object being rendered
     * @param ctx Render context with camera, buffer, and format info
     */
    virtual void PostExecute(Deki::Object* obj, RenderContext& ctx) {}

    /**
     * @brief Called once per frame, after all objects have rendered.
     *
     * Pass may run a screen-space composite by reading from scratch
     * buffers it filled during the frame and writing into the original
     * framebuffer (saved in BeginFrame).
     */
    virtual void EndFrame(RenderContext& ctx) {}
};

/**
 * @brief Callback for custom sorting
 *
 * Returns true if the object is a sortable render item, setting outOrder.
 * Register on Standard2DRenderer via AddSortingCallback().
 */
using SortingCallback = bool(*)(Deki::Object* obj, int32_t& outOrder);

// 