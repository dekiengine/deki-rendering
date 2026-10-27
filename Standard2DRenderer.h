#pragma once

#include "DekiRenderer.h"
#include "DirtyRegion.h"
#include "QuadBlit.h"
#include "RenderPass.h"
#include <deki/ComponentInterfaceAdapters.h>
#include <deki/Object.h>

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

namespace Deki
{
class Object;
}
namespace Deki
{
class Component;
}

namespace Deki
{
class IClipProvider;
}
namespace Deki
{
class ISortableProvider;
}

namespace DekiRendering
{
class RendererComponent;

/// The default renderer for 2D scenes, with built-in sprites, clipping and
/// sorting groups:
/// - RendererComponent: content blitted with QuadBlit
/// - Deki2D::ClipComponent: clip rects pushed and popped around children
/// - Deki2D::SortingGroupComponent: children grouped for sorting
///
/// Extend it with:
/// - AddPass(): custom RenderPass objects for new component types
/// - AddSortingCallback(): custom sorting for new component types
///
/// It can also be used inside other renderers (e.g. a 3D renderer that draws
/// UI overlays with it).
///
/// Per frame it captures the camera once into RenderContext::cam, finds each
/// object's renderer, clip and sortable components in one walk of its
/// component list (component types are classified once and cached), and calls
/// each pass only for the hooks its HookMask() declares. Nothing has a fixed
/// capacity: passes, callbacks, objects per level and clip depth all grow as
/// needed.
class Standard2DRenderer : public DekiRenderer
{
public:
    static constexpr uint32_t kRendererTypeID = 0x53324452;  // "S2DR"
    uint32_t GetRendererType() const override { return kRendererTypeID; }

    void Render(Deki::Scene* scene, const RenderContext& ctx) override;
    /// Yes, unless one of its passes says no (RenderPass::DrawsInStrips).
    bool DrawsInStrips(Deki::Scene* scene) const override;
    /// For the later strips of a frame whose first strip drew an opaque
    /// image over the whole view (a full-screen background).
    bool CoversView(const RenderContext& ctx) const override;
    bool ClearsLazily() const override { return true; }
    /// Compares each renderer's draw with the last frame's: where it lands,
    /// and a signature of everything that decides its pixels (content, its
    /// version, size, pivot, tint, scale, rotation, clip, place in the draw
    /// order). Passes say for themselves (RenderPass::ReportsChanges). No when
    /// a pass has hooks other than Execute.
    bool FindChanges(Deki::Scene* scene, const RenderContext& ctx, DirtyRegion& changed) override;

    /// Adds a custom render pass. Not owned: the caller manages its lifetime.
    void AddPass(RenderPass* pass);

    void RemovePass(RenderPass* pass);

    /// Adds custom sorting for new component types.
    void AddSortingCallback(SortingCallback cb);

    void RemoveSortingCallback(SortingCallback cb);

private:
    std::vector<RenderPass*> m_Passes;  // in attach order
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

    // What a component type contributes to rendering, resolved once per type:
    // one hash lookup in this DLL per component per frame instead of two
    // engine-registry probes per interface per component. Cleared whenever
    // Deki::ComponentInterfaceAdapters::Version() changes, i.e. a package
    // registered an adapter after the cache was filled.
    struct TypeTraits
    {
        bool isRenderer;
        Deki::InterfaceAdapter clipAdapter;      // null when the type is not an IClipProvider
        Deki::InterfaceAdapter sortableAdapter;  // null when the type is not an ISortableProvider
    };
    std::unordered_map<Deki::ComponentType, TypeTraits> m_TypeTraits;
    uint32_t m_TraitsVersion = 0;
    const TypeTraits& TraitsFor(const Deki::Component* comp);

    struct Renderables
    {
        RendererComponent* renderer;
        Deki::IClipProvider* clip;
        Deki::ISortableProvider* sortable;
    };
    Renderables ResolveRenderables(Deki::Object* obj);

    // One renderable claimed by a component, with its components already
    // resolved and its insertion order, so the sort is stable without
    // std::stable_sort, whose temporary buffer would allocate per parent per
    // frame.
    struct SortItem
    {
        Deki::Object* obj;
        RendererComponent* renderer;  // may be null (sort group, clip-only, callback-claimed)
        Deki::IClipProvider* clip;    // may be null
        int32_t order;
        uint32_t seq;
    };
    // One scratch list per recursion depth. Must be a deque, not a vector of
    // vectors: Render() and RenderObject() hold a reference to their depth's
    // list while deeper levels are created, and growing a vector<vector> moves
    // the inner vectors and leaves that reference dangling.
    std::deque<std::vector<SortItem>> m_SortScratch;
    int m_SortDepth = 0;
    std::vector<SortItem>& SortListForDepth(int depth);
    static void SortItems(std::vector<SortItem>& items);

    void CollectSortableItems(Deki::Object* obj, std::vector<SortItem>& items);

    // A renderer's content (RenderContent's results).
    struct CachedContent
    {
        bool hasContent = false;
        QuadBlit::Source source{};
        float pivotX = 0.5f, pivotY = 0.5f;
        uint8_t tintR = 255, tintG = 255, tintB = 255, tintA = 255;
    };

    // What ExecuteBuiltins works out in the whole view for one object, which
    // is the same in every strip of a frame.
    struct BuiltinGeometry
    {
        bool valid = false;
        Deki::WorldTransform wt{};
        int32_t clipLeft = 0, clipTop = 0, clipWidth = 0, clipHeight = 0;  // IClipProvider, whole view
        float screenX = 0.0f, screenY = 0.0f;                              // renderer origin, before snapping
        bool hasExtents = false;
        float reach = 0.0f;  // cull radius around the origin, when hasExtents
        // The pixels of the whole view that the renderer's blit can write,
        // once a strip has worked them out (QuadBlit::BlitArea): the frame's
        // other strips outside them skip the object's blit.
        bool areaKnown = false;
        int32_t areaLeft = 0, areaTop = 0, areaRight = 0, areaBottom = 0;
        // The renderer's content, made once for every part of the frame
        // (RenderContext::contentFrame). Content handed over with ownsPixels
        // is freed after its blit, so it is made again for each.
        bool contentKnown = false;
        CachedContent content;
    };

    // A frame drawn strip by strip (RenderContext::contentFrame) is collected
    // and sorted once: its first strip records the sorted tree here, with each
    // object's geometry, and the other strips replay it. The hooks, clips and
    // draws are the same as walking the scene again; only the walk, the sorts
    // and the whole-view maths are not repeated.
    struct PlanEntry
    {
        SortItem item;
        uint32_t firstChild = 0;
        uint32_t childCount = 0;
        BuiltinGeometry geo;
        // Which Execute passes want the object (RenderPass::WantsObject), a
        // bit per pass in m_ExecPasses' order; past 32 passes, all do.
        uint32_t execPasses = 0xFFFFFFFFu;
    };
    // Measuring is FindChanges' walk: it records the plan as Recording does,
    // and each renderer's content, but draws nothing.
    enum class PlanMode
    {
        Off,
        Recording,
        Measuring,
        Replaying
    };
    bool BuildingPlan() const { return m_PlanMode == PlanMode::Recording || m_PlanMode == PlanMode::Measuring; }
    std::vector<PlanEntry> m_Plan;
    uint32_t m_PlanRoots = 0;
    uint32_t m_PlanFrame = 0;
    PlanMode m_PlanMode = PlanMode::Off;
    // Recording found a blit into the frame's buffer that writes the whole
    // view opaquely. Every strip draws it the same, and it overwrites whatever
    // came before it, so the later strips need no clear (CoversView).
    bool m_PlanCovers = false;
    const uint8_t* m_FrameBuffer = nullptr;  // the buffer Render() was given
    // The frame's clear while nothing has been written yet (ctx.clear).
    const DeferredClear* m_Clear = nullptr;
    // Runs the frame's clear, if still pending, before something is drawn.
    void RunClear();

    // planIndex: the object's entry in m_Plan while recording or replaying, -1 otherwise.
    void RenderObject(const SortItem& item, const RenderContext& ctx, int32_t planIndex);

    // Built-in components: clip and renderer. geo, when given, holds the
    // object's whole-view geometry from an earlier strip of the frame, or is
    // filled in for later ones.
    void ExecuteBuiltins(const SortItem& item, RenderContext& ctx, BuiltinGeometry* geo);
    void PostExecuteBuiltins(const SortItem& item);

    // FindChanges: what each renderer drew in the last frame it compared.
    struct DrawRecord
    {
        uint64_t signature = 0;
        Deki::Rect area = {};  // the pixels of the view it can write; empty for none
        uint32_t seen = 0;     // the FindChanges call that met it last
    };
    std::unordered_map<const RendererComponent*, DrawRecord> m_Records;
    uint32_t m_RecordCall = 0;
    bool m_HaveRecords = false;
    FrameCamera m_RecordCam;  // what the records were drawn through
    int32_t m_RecordViewWidth = 0, m_RecordViewHeight = 0;
    uint32_t m_RecordScene = 0;        // SceneSystem::GetRootSceneSerial then
    DirtyRegion* m_Changes = nullptr;  // FindChanges' region while it walks
    uint32_t m_DrawIndex = 0;          // renderers met so far in the walk
    uint32_t m_AlwaysChanged = 0;      // makes signatures no other frame repeats
    // Compares one renderer's draw with its record and keeps this one.
    void NoteDraw(const RendererComponent* renderer, uint64_t signature, const Deki::Rect& area);
    void ForgetRecords();
};

}  // namespace DekiRendering
