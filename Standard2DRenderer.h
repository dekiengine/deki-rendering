#pragma once

#include "DekiRenderer.h"
#include "RenderPass.h"

#include <cstdint>
#include <vector>

// Forward declarations
class DekiObject;

/**
 * @brief Standard 2D renderer with built-in support for sprites, clipping, and sorting groups
 *
 * This is the default renderer for 2D scenes. It handles:
 * - RendererComponent: blits content via QuadBlit
 * - ClipComponent: pushes/pops clip rects around children
 * - SortingGroupComponent: groups children for sorting
 *
 * Extensible via:
 * - AddPass(): register custom RenderPass objects for new component types
 * - AddSortingCallback(): register custom sorting for new component types
 *
 * Can also be composed inside other renderers (e.g., a 3D renderer
 * that uses Standard2DRenderer for UI overlays).
 */
class Standard2DRenderer : public DekiRenderer
{
public:
    static constexpr uint32_t RendererTypeID = 0x53324452; // "S2DR"
    uint32_t GetRendererType() const override { return RendererTypeID; }

    void Render(Scene* scene, const RenderContext& ctx) override;

    /**
     * @brief Add a custom render pass
     * @param pass Non-owning pointer to a RenderPass (caller manages lifetime)
     */
    void AddPass(RenderPass* pass);

    /**
     * @brief Remove a previously added render pass
     * @param pass The pass to remove
     */
    void RemovePass(RenderPass* pass);

    /**
     * @brief Add a custom sorting callback for new component types
     * @param cb Function that returns true if an object is sortable, setting outOrder
     */
    void AddSortingCallback(SortingCallback cb);

    /**
     * @brief Remove a previously added sorting callback
     * @param cb The callback to remove
     */
    void RemoveSortingCallback(SortingCallback cb);

private:
    static constexpr int MAX_PASSES = 16;
    static constexpr int MAX_SORTING_CALLBACKS = 8;
    RenderPass* m_Passes[MAX_PASSES] = {};
    int m_PassCount = 0;
    SortingCallback m_SortingCallbacks[MAX_SORTING_CALLBACKS] = {};
    int m_SortingCallbackCount = 0;

    // One renderable claimed by a component, with its insertion order so the
    // sort is stable without std::stable_sort (whose temporary buffer was a
    // heap allocation per parent per frame).
    struct SortItem
    {
        DekiObject* obj;
        int32_t order;
        uint32_t seq;
    };
    // One reusable list per recursion depth. They used to be 64-entry stack
    // arrays: 512 bytes of stack per hierarchy level, and a silent cap.
    std::vector<std::vector<SortItem>> m_SortScratch;
    int m_SortDepth = 0;
    std::vector<SortItem>& SortListForDepth(int depth);
    static void SortItems(std::vector<SortItem>& items);

    void CollectSortableItems(DekiObject* obj, std::vector<SortItem>& items);
    void RenderObject(DekiObject* obj, const RenderContext& ctx);

    // Built-in component handling
    bool GetBuiltinSortingOrder(DekiObject* obj, int32_t& outOrder);
    void ExecuteBuiltins(DekiObject* obj, RenderContext& ctx);
    void PostExecuteBuiltins(DekiObject* obj, RenderContext& ctx);
};
