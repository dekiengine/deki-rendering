#pragma once
#include <cstdint>

// Forward declarations
class DekiObject;
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
 *     void Execute(DekiObject* obj, RenderContext& ctx) override {
 *         auto* effect = obj->GetComponent<MyEffectComponent>();
 *         if (!effect) return;
 *         // Apply effect...
 *     }
 * };
 *
 * standard2DRenderer.AddPass(&myEffectPass);
 * @endcode
 */
class RenderPass
{
public:
    virtual ~RenderPass() = default;

    /**
     * @brief Called per-object before children are rendered
     * @param obj The current object being rendered
     * @param ctx Render context with camera, buffer, and format info
     */
    virtual void Execute(DekiObject* obj, RenderContext& ctx) {}

    /**
     * @brief Called per-object after children are rendered
     * @param obj The current object being rendered
     * @param ctx Render context with camera, buffer, and format info
     */
    virtual void PostExecute(DekiObject* obj, RenderContext& ctx) {}
};

/**
 * @brief Callback for custom sorting
 *
 * Returns true if the object is a sortable render item, setting outOrder.
 * Register on Standard2DRenderer via AddSortingCallback().
 */
using SortingCallback = bool(*)(DekiObject* obj, int32_t& outOrder);

// 