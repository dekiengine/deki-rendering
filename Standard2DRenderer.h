#pragma once

#include "DekiRenderer.h"
#include "ExternalAllocator.h"
#include "PackedPixels.h"
#include "DirtyRegion.h"
#include "Fingerprint.h"
#include "Lighting2D.h"
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
    ~Standard2DRenderer() override;
    bool ScrolledBy(int32_t& dx, int32_t& dy) const override;
    const std::vector<Deki::Rect>& ScrollPins() const override { return m_PinRects; }

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
        uint32_t piece = kNoPiece;  // which of the renderer's pieces (RendererComponent::GetPieceCount)
    };
    static constexpr uint32_t kNoPiece = 0xFFFFFFFFu;
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
        // is freed after its blit, so it is made again for each. Where it is
        // (ContentOf): above 0 the renderer's place in m_ReuseContent, from
        // 1; below 0 its place in m_PlanContent, from -1. Not here: an
        // entry is written for every object every frame, its content read
        // only for the few a part draws.
        bool contentKnown = false;
        int32_t contentAt = 0;
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
    // In external memory, as the lists below: written once a frame, and no
    // size they pass through as they grow is taken from internal RAM.
    std::vector<PlanEntry, ExternalAllocator<PlanEntry>> m_Plan;
    // The frame's content of renderers with no place in m_ReuseContent.
    std::vector<CachedContent, ExternalAllocator<CachedContent>> m_PlanContent;
    const CachedContent& ContentOf(const BuiltinGeometry& g) const
    {
        return g.contentAt > 0 ? m_ReuseContent[g.contentAt - 1] : m_PlanContent[static_cast<size_t>(-g.contentAt) - 1];
    }
    // What replaying needs of each entry to pass it over: the pixels of the
    // view it can draw in (PlanAreaOf), noted as the plan is built. A frame
    // redrawn in parts replays the plan once a part, and most objects lie
    // outside most parts. The plan's entries are large and, on a board, in
    // external memory, where reading two lines of each to pass it over cost
    // more than drawing the few that are inside; this list is a few lines
    // for all of them.
    std::vector<Deki::Rect, ExternalAllocator<Deki::Rect>> m_PlanAreas;
    Deki::Rect PlanAreaOf(const PlanEntry& entry) const;
    void FindPlanAreas();  // all of them, for a plan that has none
    // Whether `area` (view pixels) lies outside what ctx draws: its strip, or
    // the part of it redrawn.
    static bool OutsidePart(const Deki::Rect& area, const RenderContext& ctx);
    uint32_t m_PlanRoots = 0;
    uint32_t m_PlanFrame = 0;
    PlanMode m_PlanMode = PlanMode::Off;
    // Recording found a blit into the frame's buffer that writes the whole
    // view opaquely. Every strip draws it the same, and it overwrites whatever
    // came before it, so the later strips need no clear (CoversView).
    bool m_PlanCovers = false;
    const uint8_t* m_FrameBuffer = nullptr;  // the buffer Render() was given
    // The frame's blits go to a GPU (QuadBlit::BeginSinkFrame): after a hook
    // of a pass that writes into the buffer itself, the sink is told.
    bool m_SinkFrame = false;
    void AfterHook(const RenderPass* pass)
    {
        if (m_SinkFrame && pass->WritesBuffer())
        {
            QuadBlit::NoteWritten(m_FrameBuffer);
        }
    }
    // Runs the frame's clear, if still pending (QuadBlit::PendingClear),
    // before something is drawn.
    void RunClear();
    // Before the hooks of `passes`: the clear, if one of them may write into
    // the buffer other than through QuadBlit (RenderPass::WritesBuffer).
    void RunClearFor(const std::vector<RenderPass*>& passes);

    // The frame's 2D light (Lighting2D.h), worked out at the start of each
    // Render(), and whether it has been laid over the frame yet. It is laid
    // over what is drawn up to the camera's litUpToSortOrder, before the
    // first object above it, or before a renderer that lights itself (a lit
    // mesh), or at the end.
    Lighting2D m_Lighting;
    bool m_Lit = false;
    // The frame's clear, lit, waiting for the frame's first write as the
    // clear itself did (QuadBlit::PendingClear): it stands for m_LitFrom,
    // the clear DekiRenderSystem left, made in its colour lit.
    DeferredClear m_LitClear;
    const DeferredClear* m_LitFrom = nullptr;
    Deki::ColorFormat m_LitFormat = Deki::ColorFormat::RGB565;
    int32_t m_LitViewLeft = 0;
    int32_t m_LitViewTop = 0;
    static void RunLitClear(void* owner);
    static bool LitClearPixel(void* owner, uint8_t* out);
    static bool LitClearColour(void* owner, uint8_t* out);
    // Lights the frame now if `next` (null: the frame's end) is where it is due.
    void LightFrameIfDue(const SortItem* next, const RenderContext& ctx);

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
        Deki::Rect area = {};    // the pixels of the view it can write; empty for none
        Deki::Rect reach = {};   // the same before it is cut to the view (still within its clip)
        uint32_t seen = 0;       // the FindChanges call that met it last
        uint32_t reuseSlot = 0;  // its ReuseEntry in m_Reuse, from 1; 0 for none
        // The signature with the place on screen instead of in the world:
        // equal while it stays put on screen as the camera scrolls.
        uint64_t screenSignature = 0;
    };
    // By renderer and piece (kNoPiece for a renderer's one drawing).
    struct RecordKey
    {
        const RendererComponent* renderer;
        uint32_t piece;
        bool operator==(const RecordKey& o) const { return renderer == o.renderer && piece == o.piece; }
    };
    struct RecordKeyHash
    {
        size_t operator()(const RecordKey& k) const noexcept
        {
            const uint64_t h = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(k.renderer)) ^
                               (static_cast<uint64_t>(k.piece) * 0x9E3779B97F4A7C15ull);
            return static_cast<size_t>(h ^ (h >> 29));
        }
    };
    std::unordered_map<RecordKey, DrawRecord, RecordKeyHash> m_Records;
    // What FindChanges measured of a renderer's content, kept while its draw
    // key (RendererComponent::GetDrawKey) stays the same: then it is reused,
    // and only where the object is, is worked out. In external memory, a slot
    // for each renderer with a key (DrawRecord::reuseSlot). What RenderContent
    // gave is kept beside it (m_ReuseContent, the same slot): every renderer's
    // entry is read every frame, and where memory is slow to read a short
    // entry is most of what that costs.
    struct ReuseEntry
    {
        bool valid = false;
        bool hasExtents = false;
        // The content, at this scale and rotation: the signature up to where
        // it is drawn, and the blit's area from the whole pixel it is drawn
        // at (relEmpty: none).
        bool contentKnown = false;
        bool relEmpty = false;
        float extentW = 0.0f, extentH = 0.0f;
        uint64_t key = 0;
        float scaleX = 0.0f, scaleY = 0.0f, rotation = 0.0f;
        Fingerprint partial;
        int32_t relLeft = 0, relTop = 0, relRight = 0, relBottom = 0;
    };
    ReuseEntry* m_Reuse = nullptr;
    CachedContent* m_ReuseContent = nullptr;
    uint32_t m_ReuseCapacity = 0;
    uint32_t m_ReuseUsed = 0;
    std::vector<uint32_t> m_ReuseFree;
    // This FindChanges may reuse (the view and the assets are as they were
    // last time), and the asset epoch then.
    bool m_ReuseOn = false;
    uint64_t m_ReuseEpoch = 0;
    // The record's entry, made when `create` and there is none (null when
    // there is no memory for it).
    ReuseEntry* ReuseFor(DrawRecord& record, bool create);
    void ForgetReuse();
    uint32_t m_RecordCall = 0;
    bool m_HaveRecords = false;
    FrameCamera m_RecordCam;  // what the records were drawn through
    int32_t m_RecordViewWidth = 0, m_RecordViewHeight = 0;
    uint32_t m_RecordScene = 0;        // SceneSystem::GetRootSceneSerial then
    DirtyRegion* m_Changes = nullptr;  // FindChanges' region while it walks
    uint32_t m_DrawIndex = 0;          // renderers met so far in the walk
    uint32_t m_AlwaysChanged = 0;      // makes signatures no other frame repeats
    // Scrolling (RenderContext::scrollable): how far this frame moves the
    // last one's pixels, and how far they have moved in all since the records
    // were made. Signatures measure positions from there, so an object that
    // stays put in the world compares equal while the camera scrolls.
    int32_t m_ScrollX = 0, m_ScrollY = 0;
    int32_t m_ScrollOriginX = 0, m_ScrollOriginY = 0;
    // While scrolling: what stays put on screen, opaque and drawn the same,
    // keeps its pixels (ScrollPins), in the order it is drawn. Anything drawn
    // over a pin afterwards that is not the same on screen redraws it.
    struct Pin
    {
        Deki::Rect rect;
        uint32_t drawIndex = 0;
    };
    std::vector<Pin> m_Pins;
    std::vector<Deki::Rect> m_PinRects;
    // What renderers that stay put on screen changed, less every pin of the
    // frame: worked out once the walk has found them all.
    std::vector<Deki::Rect> m_StillChanges;
    std::vector<Deki::Rect> m_StillPieces, m_StillNext;  // scratch for taking the pins out of them
    // What a source's pixels say for themselves, unflipped, kept by their
    // address and revision in a small table in external memory (internal RAM
    // is the display's): its opaque interior (QuadBlit::OpaqueInterior) and
    // its non-transparent box (QuadBlit::VisibleBounds). Pixels with no
    // revision are not kept, and are not trimmed.
    struct SourceFacts
    {
        const uint8_t* pixels = nullptr;
        uint32_t revision = 0;
        int32_t width = 0, height = 0, stride = 0;
        uint8_t known = 0;  // bit 0: interior, bit 1: visible box
        bool hasInterior = false, hasVisible = false;
        Deki::Rect interior, visible;
    };
    PackedPixels m_Packed;
    // The asset epoch and the scene its runs were made in.
    uint64_t m_PackedEpoch = 0;
    uint32_t m_PackedScene = 0;
    static constexpr size_t kSourceFacts = 512;
    static constexpr uint32_t kSourceFactWays = 4;  // places looked in for a source
    uint32_t m_SourceFactTurn = 0;                  // which of them the next one with no place takes
    SourceFacts* m_SourceFacts = nullptr;
    SourceFacts* FactsOf(const QuadBlit::Source& source);
    bool OpaqueInteriorOf(const QuadBlit::Source& source, Deki::Rect& rect);
    bool VisibleBoundsOf(const QuadBlit::Source& source, Deki::Rect& rect);
    // The source cut down to its box of non-transparent pixels, and where
    // that box starts in the source as drawn (with its flips). False when
    // nothing is known of the source, or there is nothing to cut.
    bool VisiblePartOf(const QuadBlit::Source& source, QuadBlit::Source& part, int32_t& x, int32_t& y);
    // The picture's runs in m_Packed, or null when it cannot have any.
    const uint8_t* RunsOf(const QuadBlit::Source& picture);
    // Draws a source 1:1 and unturned with its corner at (left, top) the
    // cheapest way it can: from its runs, or only its visible box. False,
    // with nothing drawn, when the usual blit of the whole source is as good.
    bool BlitUnscaled(const QuadBlit::Source& source, const RenderContext& ctx, int32_t left, int32_t top,
                      uint8_t tintR, uint8_t tintG, uint8_t tintB, uint8_t tintA, bool useOrderedDither);
    // Compares one renderer's draw with its record and keeps this one.
    // screenSignature and pin: see DrawRecord and m_Pins (0 and empty when
    // not scrolling).
    void NoteDraw(const RendererComponent* renderer, uint64_t signature, const Deki::Rect& area,
                  const Deki::Rect& reach = Deki::Rect{}, uint64_t screenSignature = 0,
                  const Deki::Rect& pin = Deki::Rect{}, uint32_t drawIndex = 0, DrawRecord* record = nullptr);
    void ForgetRecords();
    Lighting2D m_LightingRecord;  // the light of the last frame FindChanges compared
};

}  // namespace DekiRendering
