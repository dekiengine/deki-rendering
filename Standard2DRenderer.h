#pragma once

#include "DekiRenderer.h"
#include "DirtyRegion.h"
#include "RenderPass.h"
#include "ComponentInterfaceAdapters.h"

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

// Forward declarations
class DekiObject;
class DekiComponent;
class RendererComponent;
class IClipProvider;
class ISortableProvider;

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
 *
 * Per frame it captures the camera once into RenderContext::cam, resolves
 * each object's renderer / clip / sortable components with a single walk of
 * its component list (component types are classified once and cached), and
 * calls each pass only for the hooks its HookMask() declares. There are no
 * fixed capacities anywhere: passes, callbacks, objects per level and clip
 * depth all grow as needed.
 */
class Standard2DRenderer : public DekiRenderer
{
public:
    static constexpr uint32_t RendererTypeID = 0x53324452; // "S2DR"
    uint32_t GetRendererType() const override { return RendererTypeID; }

    void Render(Scene* scene, const RenderContext& ctx) override;
    const DirtyRegion* GetLastFrameDirty() const override { return m_FrameDirtyValid ? &m_FrameDirty : nullptr; }

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
    std::vector<RenderPass*> m_Passes;  // in attach order; no cap
    // Per-hook subsets of m_Passes in the same relative order, rebuilt from
    // HookMask() at the start of every frame. A pass that only implements
    // Execute costs nothing in the other four loops.
    std::vector<RenderPass*> m_BeginPasses;
    std::vector<RenderPass*> m_PrePasses;
    std::vector<RenderPass*> m_ExecPasses;
    std::vector<RenderPass*> m_PostPasses;
    std::vector<RenderPass*> m_EndPasses;
    void RebuildHookLists();

    std::vector<SortingCallback> m_SortingCallbacks;

    // Pixels of ctx.buffer the last frame changed (ctx.trackDirty renders
    // only). QuadBlit adds every clipped blit rectangle; a pass that installs
    // its own frame target makes it full.
    DirtyRegion m_FrameDirty;
    bool m_FrameDirtyValid = false;

    // What a component type contributes to rendering, resolved once per type
    // (one hash lookup in this DLL per component per frame instead of two
    // engine-registry probes per interface per component). Dropped whenever
    // ComponentInterfaceAdapters::Version() changes, i.e. a package registered
    // an adapter after the cache was filled.
    struct TypeTraits
    {
        bool isRenderer;
        InterfaceAdapter clipAdapter;      // null when the type is not an IClipProvider
        InterfaceAdapter sortableAdapter;  // null when the type is not an ISortableProvider
    };
    std::unordered_map<ComponentType, TypeTraits> m_TypeTraits;
    uint32_t m_TraitsVersion = 0;
    const TypeTraits& TraitsFor(const DekiComponent* comp);

    struct Renderables
    {
        RendererComponent* renderer;
        IClipProvider* clip;
        ISortableProvider* sortable;
    };
    Renderables ResolveRenderables(DekiObject* obj);

    // One renderable claimed by a component, with the components already
    // resolved and its insertion order so the sort is stable without
    // std::stable_sort (whose temporary buffer was a heap allocation per
    // parent per frame).
    struct SortItem
    {
        DekiObject* obj;
        RendererComponent* renderer;  // may be null (sort group, clip-only, callback-claimed)
        IClipProvider* clip;          // may be null
        int32_t order;
        uint32_t seq;
    };
    // One scratch list per recursion depth. A deque, not a vector of vectors:
    // Render() and RenderObject() hold a reference to their depth's list while
    // deeper levels are created, and growing a vector<vector> moves the inner
    // vectors, leaving that reference dangling (the renderer golden test found
    // it as a use-after-free on the first frame with children).
    std::deque<std::vector<SortItem>> m_SortScratch;
    int m_SortDepth = 0;
    std::vector<SortItem>& SortListForDepth(int depth);
    static void SortItems(std::vector<SortItem>& items);

    void CollectSortableItems(DekiObject* obj, std::vector<SortItem>& items);
    void RenderObject(const SortItem& item, const RenderContext& ctx);

    // Built-in component handling
    void ExecuteBuiltins(const SortItem& item, RenderContext& ctx);
    void PostExecuteBuiltins(const SortItem& item);
};
