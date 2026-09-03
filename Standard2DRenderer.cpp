#include "Standard2DRenderer.h"
#include "DekiRendererRegistry.h"
#include "IClipProvider.h"
#include "ISortableProvider.h"
#include "DekiEngine.h"
#include "SceneSystem.h"
#include "CameraComponent.h"
#include "RendererComponent.h"
#include "QuadBlit.h"
#include "DekiObject.h"
#include "Scene.h"
#include "DekiLogSystem.h"

#include <algorithm>
#include <cmath>

// Self-register with the renderer registry
static struct Standard2DRegistrar {
    Standard2DRegistrar() {
        DekiRendererRegistry::Register("standard2d",
            []() -> DekiRenderer* { return new Standard2DRenderer(); });
    }
} s_standard2dRegistrar;

// --- Pass / callback management ---

void Standard2DRenderer::AddPass(RenderPass* pass)
{
    if (pass && std::find(m_Passes.begin(), m_Passes.end(), pass) == m_Passes.end())
        m_Passes.push_back(pass);
}

void Standard2DRenderer::RemovePass(RenderPass* pass)
{
    auto it = std::find(m_Passes.begin(), m_Passes.end(), pass);
    if (it != m_Passes.end())
        m_Passes.erase(it);
}

void Standard2DRenderer::AddSortingCallback(SortingCallback cb)
{
    if (cb && std::find(m_SortingCallbacks.begin(), m_SortingCallbacks.end(), cb) == m_SortingCallbacks.end())
        m_SortingCallbacks.push_back(cb);
}

void Standard2DRenderer::RemoveSortingCallback(SortingCallback cb)
{
    auto it = std::find(m_SortingCallbacks.begin(), m_SortingCallbacks.end(), cb);
    if (it != m_SortingCallbacks.end())
        m_SortingCallbacks.erase(it);
}

void Standard2DRenderer::RebuildHookLists()
{
    m_BeginPasses.clear();
    m_PrePasses.clear();
    m_ExecPasses.clear();
    m_PostPasses.clear();
    m_EndPasses.clear();
    for (RenderPass* pass : m_Passes)
    {
        const uint32_t mask = pass->HookMask();
        if (mask & RenderPassHooks::BeginFrame)  m_BeginPasses.push_back(pass);
        if (mask & RenderPassHooks::PreExecute)  m_PrePasses.push_back(pass);
        if (mask & RenderPassHooks::Execute)     m_ExecPasses.push_back(pass);
        if (mask & RenderPassHooks::PostExecute) m_PostPasses.push_back(pass);
        if (mask & RenderPassHooks::EndFrame)    m_EndPasses.push_back(pass);
    }
}

// --- Component classification ---

const Standard2DRenderer::TypeTraits& Standard2DRenderer::TraitsFor(const DekiComponent* comp)
{
    const ComponentType type = comp->GetType();
    auto it = m_TypeTraits.find(type);
    if (it != m_TypeTraits.end())
        return it->second;

    // Same predicates as DekiObject::GetComponent<RendererComponent>() and
    // FindInterface<T>() (exact type, then one base level), evaluated once.
    const ComponentType base = comp->GetBaseType();
    TypeTraits traits;
    traits.isRenderer = (type == RendererComponent::StaticType || base == RendererComponent::StaticType);
    traits.clipAdapter = ComponentInterfaceAdapters::Find(IClipProvider::InterfaceID, type, base);
    traits.sortableAdapter = ComponentInterfaceAdapters::Find(ISortableProvider::InterfaceID, type, base);
    return m_TypeTraits.emplace(type, traits).first->second;
}

Standard2DRenderer::Renderables Standard2DRenderer::ResolveRenderables(DekiObject* obj)
{
    // One walk of the component list; first match wins per role, exactly as
    // the separate GetComponent / FindInterface lookups did.
    Renderables r{ nullptr, nullptr, nullptr };
    for (DekiComponent* comp : obj->GetComponents())
    {
        const TypeTraits& t = TraitsFor(comp);
        if (t.isRenderer && !r.renderer)
            r.renderer = static_cast<RendererComponent*>(comp);
        if (t.clipAdapter && !r.clip)
            r.clip = static_cast<IClipProvider*>(t.clipAdapter(comp));
        if (t.sortableAdapter && !r.sortable)
            r.sortable = static_cast<ISortableProvider*>(t.sortableAdapter(comp));
    }
    return r;
}

// --- Built-in component handling ---

void Standard2DRenderer::ExecuteBuiltins(const SortItem& item, RenderContext& ctx)
{
    DekiObject* obj = item.obj;
    const DekiWorldTransform wt = obj->GetWorldTransform();  // one dirty check for all five values

    // Clip: push clip rect if IClipProvider is present
    if (item.clip)
    {
        float fScreenX, fScreenY;
        ctx.cam.WorldToScreen(wt.x, wt.y, fScreenX, fScreenY);
        int32_t screenX = static_cast<int32_t>(std::floor(fScreenX));
        int32_t screenY = static_cast<int32_t>(std::floor(fScreenY));

        const float effective = ctx.cam.ppm;
        float scaledW = item.clip->GetClipWidth() * effective * wt.scaleX;
        float scaledH = item.clip->GetClipHeight() * effective * wt.scaleY;
        int32_t left = screenX - static_cast<int32_t>(std::floor(scaledW * 0.5f));
        int32_t top  = screenY - static_cast<int32_t>(std::floor(scaledH * 0.5f));

        QuadBlit::PushClipRect(left, top,
                               left + static_cast<int32_t>(scaledW),
                               top  + static_cast<int32_t>(scaledH));
    }

    // Sprite: blit content
    RendererComponent* renderer = item.renderer;
    if (renderer)
    {
        const bool useOrderedDither = (renderer->alphaMode == AlphaMode::OrderedDither);

        float fScreenX, fScreenY;
        ctx.cam.WorldToScreen(wt.x, wt.y, fScreenX, fScreenY);

        // Cull before RenderContent, which may rasterise text, bake a gradient
        // or copy a frame: a conservative screen box from the component's
        // world extents - any pivot (the content lies within one full size of
        // the origin) and any rotation (within width + height). Objects of
        // unknown size are drawn.
        float extentW = 0.0f, extentH = 0.0f;
        if (renderer->GetContentExtents(extentW, extentH))
        {
            const float cx = fScreenX, cy = fScreenY;
            const float reach = (std::fabs(extentW * wt.scaleX) +
                                 std::fabs(extentH * wt.scaleY)) *
                                    ctx.cam.ppm +
                                2.0f;
            float left = 0.0f, top = 0.0f;
            float right = static_cast<float>(ctx.width), bottom = static_cast<float>(ctx.height);
            if (!renderer->ignoreClip && QuadBlit::IsClipEnabled())
            {
                const QuadBlit::ClipRect clip = QuadBlit::GetCurrentClipRect();
                left = std::max(left, static_cast<float>(clip.left));
                top = std::max(top, static_cast<float>(clip.top));
                right = std::min(right, static_cast<float>(clip.right));
                bottom = std::min(bottom, static_cast<float>(clip.bottom));
            }
            if (cx + reach < left || cx - reach > right || cy + reach < top || cy - reach > bottom)
                return;
        }

        QuadBlit::Source source;
        float pivotX, pivotY;
        uint8_t tintR, tintG, tintB, tintA;
        if (renderer->RenderContent(obj, source, pivotX, pivotY,
                                     tintR, tintG, tintB, tintA))
        {
            // Temporarily disable clipping if renderer has ignoreClip set
            bool wasClipEnabled = QuadBlit::IsClipEnabled();
            if (renderer->ignoreClip)
                QuadBlit::SetClipEnabled(false);

            // Apply unit conversion: source pixels -> world meters -> screen pixels.
            //   screen_px = (source_px / source.pixelsPerMeter) * world_scale * camera.pixelsPerMeter
            // QuadBlit applies (source_px * scale), so:
            //   scale = world_scale * camera.pixelsPerMeter / source.pixelsPerMeter
            //
            // World coords are always meters; sprite.pixelsPerMeter is always
            // honored. Match camera and sprite PPM (and project PPM) for 1:1
            // pixel rendering of source art.
            const float worldToScreen = ctx.cam.ppm;
            const float spritePPM = (source.pixelsPerMeter > 0.0f) ? source.pixelsPerMeter : 1.0f;
            const float invSourcePPM = 1.0f / spritePPM;
            const float drawScaleX = wt.scaleX * worldToScreen * invSourcePPM;
            const float drawScaleY = wt.scaleY * worldToScreen * invSourcePPM;

            // pixelSnap = true → round to nearest pixel (sharp, sprite-art).
            // pixelSnap = false → truncate (sub-pixel motion accumulates;
            // visually smoother under continuous movement, no bilinear yet).
            const int32_t intScreenX = renderer->pixelSnap
                ? static_cast<int32_t>(std::lround(fScreenX))
                : static_cast<int32_t>(fScreenX);
            const int32_t intScreenY = renderer->pixelSnap
                ? static_cast<int32_t>(std::lround(fScreenY))
                : static_cast<int32_t>(fScreenY);

            QuadBlit::Blit(
                source,
                ctx.buffer,
                ctx.width,
                ctx.height,
                ctx.format,
                intScreenX,
                intScreenY,
                drawScaleX,
                drawScaleY,
                wt.rotation,
                pivotX,
                pivotY,
                tintR,
                tintG,
                tintB,
                tintA,
                useOrderedDither
            );

            // Restore clip state
            if (renderer->ignoreClip)
                QuadBlit::SetClipEnabled(wasClipEnabled);

            // Free intermediate buffer if we own it
            if (source.ownsPixels && source.pixels)
            {
                delete[] source.pixels;
            }
        }
    }
}

void Standard2DRenderer::PostExecuteBuiltins(const SortItem& item)
{
    // Pop clip rect if IClipProvider is present
    if (item.clip)
        QuadBlit::PopClipRect();
}

// --- Sortable item collection ---

std::vector<Standard2DRenderer::SortItem>& Standard2DRenderer::SortListForDepth(int depth)
{
    while (static_cast<size_t>(depth) >= m_SortScratch.size())
        m_SortScratch.emplace_back();  // deque: existing lists keep their addresses
    std::vector<SortItem>& list = m_SortScratch[depth];
    list.clear();  // keeps capacity: no allocation once warm
    return list;
}

void Standard2DRenderer::SortItems(std::vector<SortItem>& items)
{
    // Lower order draws first; equal orders keep collection order.
    std::sort(items.begin(), items.end(), [](const SortItem& a, const SortItem& b)
              { return a.order != b.order ? a.order < b.order : a.seq < b.seq; });
}

void Standard2DRenderer::CollectSortableItems(DekiObject* obj, std::vector<SortItem>& items)
{
    // Every object reaching the sort is active and was reached through active
    // parents (the walk starts at the scene roots), so RenderObject needs no
    // further active check.
    if (!obj || !obj->IsActive()) return;

    const Renderables r = ResolveRenderables(obj);

    // Check built-in components first: a renderer, then any other sortable
    // (ClipComponent, SortingGroupComponent, ...).
    if (r.renderer)
    {
        items.push_back({obj, r.renderer, r.clip, r.renderer->sortingOrder, static_cast<uint32_t>(items.size())});
        return;
    }
    if (r.sortable)
    {
        items.push_back({obj, nullptr, r.clip, r.sortable->GetSortingOrder(), static_cast<uint32_t>(items.size())});
        return;
    }

    // Then check custom sorting callbacks
    int32_t order;
    for (SortingCallback cb : m_SortingCallbacks)
    {
        if (cb(obj, order))
        {
            items.push_back({obj, nullptr, r.clip, order, static_cast<uint32_t>(items.size())});
            return;
        }
    }

    // No one claimed it — transparent container, children float up
    for (auto* child : obj->GetChildren())
        CollectSortableItems(child, items);
}

// --- Main render loop ---

void Standard2DRenderer::Render(Scene* scene, const RenderContext& ctx)
{
    if (!scene || !ctx.camera || !ctx.buffer)
        return;

    QuadBlit::ClearClipStack();

    // A package that loaded (or reloaded) since the last frame may have
    // registered adapters for types already classified: start over.
    const uint32_t adapterVersion = ComponentInterfaceAdapters::Version();
    if (adapterVersion != m_TraitsVersion)
    {
        m_TypeTraits.clear();
        m_TraitsVersion = adapterVersion;
    }
    RebuildHookLists();

    // Frame-scoped mutable context. Passes can swap frameCtx.buffer in
    // BeginFrame to install a default render target for the whole frame;
    // every RenderObject below uses frameCtx, not the original ctx.
    RenderContext frameCtx = ctx;
    for (RenderPass* pass : m_BeginPasses)
        pass->BeginFrame(frameCtx);

    // Capture the camera once for the frame, against the target the passes
    // settled on. Everything below maps world to screen through this.
    frameCtx.cam = frameCtx.camera->CaptureFrameCamera(frameCtx.width, frameCtx.height);

    // Collect and sort root objects
    m_SortDepth = 0;
    std::vector<SortItem>& sortableItems = SortListForDepth(0);

    for (DekiObject* obj : scene->GetObjects())
        CollectSortableItems(obj, sortableItems);

    // Also collect persistent objects
    const auto& persistentObjects = DekiEngine::GetInstance().GetSceneSystem().GetPersistentObjects();
    for (DekiObject* obj : persistentObjects)
        CollectSortableItems(obj, sortableItems);

    // Sort by sortingOrder (lower = behind)
    SortItems(sortableItems);

    // Render in sorted order. Index loop: RenderObject recurses and the deeper
    // levels use their own scratch lists, so this one is stable meanwhile.
    for (size_t i = 0; i < sortableItems.size(); i++)
        RenderObject(sortableItems[i], frameCtx);

    // Post-frame composites (e.g. screen-space overlays).
    for (auto it = m_EndPasses.rbegin(); it != m_EndPasses.rend(); ++it)
        (*it)->EndFrame(frameCtx);
}

void Standard2DRenderer::RenderObject(const SortItem& item, const RenderContext& ctx)
{
    DekiObject* obj = item.obj;
    RenderContext objCtx = ctx;

    // Phase 1: Pre-execute custom passes (may redirect ctx.buffer for this object)
    for (RenderPass* pass : m_PrePasses)
        pass->PreExecute(obj, objCtx);

    // Phase 2: Execute built-in handling (sprite blit) — uses any ctx redirect from PreExecute
    ExecuteBuiltins(item, objCtx);

    // Phase 3: Execute custom passes (tilemap draw, etc.)
    for (RenderPass* pass : m_ExecPasses)
        pass->Execute(obj, objCtx);

    // Phase 4: Recurse into sorted children — uses objCtx so children inherit any
    // buffer redirect applied by this object's PreExecute / Execute hooks.
    ++m_SortDepth;
    std::vector<SortItem>& childItems = SortListForDepth(m_SortDepth);
    for (auto* child : obj->GetChildren())
        CollectSortableItems(child, childItems);

    SortItems(childItems);

    for (size_t i = 0; i < childItems.size(); i++)
        RenderObject(childItems[i], objCtx);
    --m_SortDepth;

    // Phase 5: Post-execute custom passes (reverse order)
    for (auto it = m_PostPasses.rbegin(); it != m_PostPasses.rend(); ++it)
        (*it)->PostExecute(obj, objCtx);

    // Phase 6: Post-execute built-ins (clip pop)
    PostExecuteBuiltins(item);
}
