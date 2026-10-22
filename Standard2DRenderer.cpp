#include "Standard2DRenderer.h"
#include "RenderProfiler.h"
#include "DekiRendererRegistry.h"
#include <deki/IClipProvider.h>
#include <deki/ISortableProvider.h>
#include <deki/Engine.h>
#include <deki/SceneSystem.h>
#include "CameraComponent.h"
#include "RendererComponent.h"
#include "QuadBlit.h"
#include <deki/Object.h>
#include <deki/Scene.h>
#include <deki/LogSystem.h>
#include <deki/providers/Memory.h>

#include <algorithm>
#include <cmath>

namespace DekiRendering
{

// Registers this renderer with the renderer registry.
static struct Standard2DRegistrar
{
    Standard2DRegistrar()
    {
        DekiRendererRegistry::Register("standard2d", []() -> DekiRenderer* { return new Standard2DRenderer(); });
    }
} s_Standard2dRegistrar;

// --- Passes and callbacks ---

void Standard2DRenderer::AddPass(RenderPass* pass)
{
    if (pass && std::find(m_Passes.begin(), m_Passes.end(), pass) == m_Passes.end())
    {
        m_Passes.push_back(pass);
    }
}

void Standard2DRenderer::RemovePass(RenderPass* pass)
{
    auto it = std::find(m_Passes.begin(), m_Passes.end(), pass);
    if (it != m_Passes.end())
    {
        m_Passes.erase(it);
    }
}

bool Standard2DRenderer::DrawsInStrips(Deki::Scene* scene) const
{
    for (const RenderPass* pass : m_Passes)
    {
        if (!pass->DrawsInStrips(scene))
        {
            return false;
        }
    }
    return true;
}

void Standard2DRenderer::AddSortingCallback(SortingCallback cb)
{
    if (cb && std::find(m_SortingCallbacks.begin(), m_SortingCallbacks.end(), cb) == m_SortingCallbacks.end())
    {
        m_SortingCallbacks.push_back(cb);
    }
}

void Standard2DRenderer::RemoveSortingCallback(SortingCallback cb)
{
    auto it = std::find(m_SortingCallbacks.begin(), m_SortingCallbacks.end(), cb);
    if (it != m_SortingCallbacks.end())
    {
        m_SortingCallbacks.erase(it);
    }
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
        if (mask & RenderPassHooks::BeginFrame)
        {
            m_BeginPasses.push_back(pass);
        }
        if (mask & RenderPassHooks::PreExecute)
        {
            m_PrePasses.push_back(pass);
        }
        if (mask & RenderPassHooks::Execute)
        {
            m_ExecPasses.push_back(pass);
        }
        if (mask & RenderPassHooks::PostExecute)
        {
            m_PostPasses.push_back(pass);
        }
        if (mask & RenderPassHooks::EndFrame)
        {
            m_EndPasses.push_back(pass);
        }
    }
}

// --- Component classification ---

const Standard2DRenderer::TypeTraits& Standard2DRenderer::TraitsFor(const Deki::Component* comp)
{
    const Deki::ComponentType type = comp->GetType();
    auto it = m_TypeTraits.find(type);
    if (it != m_TypeTraits.end())
    {
        return it->second;
    }

    // Same predicates as Deki::Object::GetComponent<RendererComponent>() and
    // FindInterface<T>() (exact type, then one base level), evaluated once.
    const Deki::ComponentType base = comp->GetBaseType();
    TypeTraits traits;
    traits.isRenderer = (type == ::Deki::TypeId<RendererComponent>() || base == ::Deki::TypeId<RendererComponent>());
    traits.clipAdapter = Deki::ComponentInterfaceAdapters::Find(Deki::IClipProvider::kInterfaceID, type, base);
    traits.sortableAdapter = Deki::ComponentInterfaceAdapters::Find(Deki::ISortableProvider::kInterfaceID, type, base);
    return m_TypeTraits.emplace(type, traits).first->second;
}

Standard2DRenderer::Renderables Standard2DRenderer::ResolveRenderables(Deki::Object* obj)
{
    // One walk of the component list; the first match wins for each role, the
    // same result as separate GetComponent / FindInterface lookups.
    Renderables r{ nullptr, nullptr, nullptr };
    for (Deki::Component* comp : obj->GetComponents())
    {
        const TypeTraits& t = TraitsFor(comp);
        if (t.isRenderer && !r.renderer)
        {
            r.renderer = static_cast<RendererComponent*>(comp);
        }
        if (t.clipAdapter && !r.clip)
        {
            r.clip = static_cast<Deki::IClipProvider*>(t.clipAdapter(comp));
        }
        if (t.sortableAdapter && !r.sortable)
        {
            r.sortable = static_cast<Deki::ISortableProvider*>(t.sortableAdapter(comp));
        }
    }
    return r;
}

// --- Built-in components ---

void Standard2DRenderer::ExecuteBuiltins(const SortItem& item, RenderContext& ctx, BuiltinGeometry* geo)
{
    Deki::Object* obj = item.obj;

    // Positions are worked out in the whole view, exactly as for a whole
    // frame, and only then moved up to the strip by whole pixels, so a strip
    // truncates and rounds every edge the way the whole frame does. (ctx.cam
    // is the strip's: the whole view's moved up by viewTop, which is exact.)
    FrameCamera cam = ctx.cam;
    cam.halfH += static_cast<float>(ctx.viewTop);
    const int32_t viewTop = ctx.viewTop;

    // The whole-view geometry: the same in every strip of a frame, so a later
    // strip takes it from the first.
    BuiltinGeometry local;
    BuiltinGeometry& g = geo ? *geo : local;
    if (!g.valid)
    {
        g.wt = obj->GetWorldTransform();  // one dirty check for all five values
        cam.WorldToScreen(g.wt.x, g.wt.y, g.screenX, g.screenY);
        if (item.clip)
        {
            const int32_t screenX = static_cast<int32_t>(std::floor(g.screenX));
            const int32_t screenY = static_cast<int32_t>(std::floor(g.screenY));
            const float scaledW = item.clip->GetClipWidth() * cam.ppm * g.wt.scaleX;
            const float scaledH = item.clip->GetClipHeight() * cam.ppm * g.wt.scaleY;
            g.clipLeft = screenX - static_cast<int32_t>(std::floor(scaledW * 0.5f));
            g.clipTop = screenY - static_cast<int32_t>(std::floor(scaledH * 0.5f));
            g.clipWidth = static_cast<int32_t>(scaledW);
            g.clipHeight = static_cast<int32_t>(scaledH);
        }
        // The cull box is conservative, from the component's world extents:
        // it allows any pivot (the content lies within one full size of the
        // origin) and any rotation (within width + height). Objects of
        // unknown size are drawn.
        float extentW = 0.0f, extentH = 0.0f;
        g.hasExtents = item.renderer && item.renderer->GetContentExtents(extentW, extentH);
        if (g.hasExtents)
        {
            g.reach = (std::fabs(extentW * g.wt.scaleX) + std::fabs(extentH * g.wt.scaleY)) * cam.ppm + 2.0f;
        }
        g.valid = true;
    }
    const Deki::WorldTransform& wt = g.wt;

    // Clip: push a clip rect if the object has an IClipProvider.
    if (item.clip)
    {
        const int32_t top = g.clipTop - viewTop;
        QuadBlit::PushClipRect(g.clipLeft, top, g.clipLeft + g.clipWidth, top + g.clipHeight);
    }

    // Renderer (a sprite, say): blit its content.
    RendererComponent* renderer = item.renderer;
    if (renderer)
    {
        const bool useOrderedDither = (renderer->alphaMode == AlphaMode::OrderedDither);

        float fScreenX = g.screenX, fScreenY = g.screenY;

        // Cull before RenderContent, which may rasterise text, bake a gradient
        // or copy a frame.
        if (g.hasExtents)
        {
            const float cx = fScreenX, cy = fScreenY - static_cast<float>(viewTop);
            const float reach = g.reach;
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
            {
                return;
            }
        }

        QuadBlit::Source source;
        float pivotX, pivotY;
        uint8_t tintR, tintG, tintB, tintA;
        const bool profiling = RenderProfiler::IsEnabled();
        bool hasContent = false;

        // Drawn strip by strip, an object across several strips produces its
        // content once (a particle composite, a text bake) and every strip
        // blits that. Content handed over with ownsPixels is freed after its
        // blit, so it is produced again for each strip.
        CachedContent* cached = nullptr;
        if (ctx.contentFrame != 0)
        {
            auto it = m_ContentCache.find(renderer);
            if (it != m_ContentCache.end())
            {
                cached = &it->second;
            }
        }
        if (cached)
        {
            hasContent = cached->hasContent;
            source = cached->source;
            pivotX = cached->pivotX;
            pivotY = cached->pivotY;
            tintR = cached->tintR;
            tintG = cached->tintG;
            tintB = cached->tintB;
            tintA = cached->tintA;
        }
        else
        {
            const uint64_t contentStart = profiling ? RenderProfiler::Now() : 0;
            hasContent = renderer->RenderContent(obj, source, pivotX, pivotY, tintR, tintG, tintB, tintA);
            if (profiling)
            {
                const Deki::ComponentTypeInfo* info = renderer->GetTypeInfo();
                RenderProfiler::AddContent(renderer->GetType(), RenderProfiler::Now() - contentStart,
                                           info ? info->shortName : nullptr);
            }
            if (ctx.contentFrame != 0 && !(hasContent && source.ownsPixels))
            {
                m_ContentCache[renderer] = CachedContent{ hasContent, source, pivotX, pivotY, tintR, tintG, tintB, tintA };
            }
        }
        if (hasContent)
        {
            // ignoreClip: switch clipping off for this blit.
            bool wasClipEnabled = QuadBlit::IsClipEnabled();
            if (renderer->ignoreClip)
            {
                QuadBlit::SetClipEnabled(false);
            }

            // Units: source pixels -> world meters -> screen pixels.
            //   screen_px = (source_px / source.pixelsPerMeter) * world_scale * camera.pixelsPerMeter
            // QuadBlit applies (source_px * scale), so:
            //   scale = world_scale * camera.pixelsPerMeter / source.pixelsPerMeter
            //
            // World coordinates are always meters. Source art renders 1:1 when
            // the camera's, the sprite's and the project's pixels-per-meter match.
            const float worldToScreen = cam.ppm;
            const float spritePPM = (source.pixelsPerMeter > 0.0f) ? source.pixelsPerMeter : 1.0f;
            const float invSourcePPM = 1.0f / spritePPM;
            const float drawScaleX = wt.scaleX * worldToScreen * invSourcePPM;
            const float drawScaleY = wt.scaleY * worldToScreen * invSourcePPM;

            // Pixel Perfect: snap to the art-pixel grid, whatever the
            // renderer's own setting. Otherwise:
            // pixelSnap true: round to the nearest pixel (sharp sprite art).
            // pixelSnap false: truncate, so sub-pixel motion accumulates and
            // continuous movement looks smoother (there is no bilinear yet).
            if (cam.snapStep > 0)
            {
                fScreenX = cam.SnapX(fScreenX);
                fScreenY = cam.SnapY(fScreenY);
            }
            const int32_t intScreenX = (renderer->pixelSnap || cam.snapStep > 0)
                                           ? static_cast<int32_t>(std::lround(fScreenX))
                                           : static_cast<int32_t>(fScreenX);
            const int32_t intScreenY = ((renderer->pixelSnap || cam.snapStep > 0)
                                            ? static_cast<int32_t>(std::lround(fScreenY))
                                            : static_cast<int32_t>(fScreenY)) -
                                       viewTop;

            const uint64_t blitStart = profiling ? RenderProfiler::Now() : 0;
            QuadBlit::Blit(source, ctx.buffer, ctx.width, ctx.height, ctx.format, intScreenX, intScreenY, drawScaleX,
                           drawScaleY, wt.rotation, pivotX, pivotY, tintR, tintG, tintB, tintA, useOrderedDither);
            if (profiling)
            {
                RenderProfiler::AddBlit(renderer->GetType(), RenderProfiler::Now() - blitStart,
                                        static_cast<uint64_t>(source.width) * static_cast<uint64_t>(source.height));
            }

            if (renderer->ignoreClip)
            {
                QuadBlit::SetClipEnabled(wasClipEnabled);
            }

            // A component that composed its pixels this frame can hand the
            // buffer over instead of keeping one; freeing it is then our job.
            //
            // Through Deki::Memory, not delete[]: the buffer came from the
            // engine's allocator (Deki::Buffer<T>::Release(), or Allocate),
            // and delete[] would misread its header. Every component in the
            // tree keeps its own buffer, so only third-party ones reach this.
            if (source.ownsPixels && source.pixels)
            {
                Deki::Memory::Free(const_cast<uint8_t*>(source.pixels));
            }
        }
    }
}

void Standard2DRenderer::PostExecuteBuiltins(const SortItem& item)
{
    // Pop the clip rect pushed in ExecuteBuiltins.
    if (item.clip)
    {
        QuadBlit::PopClipRect();
    }
}

// --- Sortable item collection ---

std::vector<Standard2DRenderer::SortItem>& Standard2DRenderer::SortListForDepth(int depth)
{
    while (static_cast<size_t>(depth) >= m_SortScratch.size())
    {
        m_SortScratch.emplace_back();  // deque: existing lists keep their addresses
    }
    std::vector<SortItem>& list = m_SortScratch[depth];
    list.clear();  // keeps capacity, so no allocation once warmed up
    return list;
}

void Standard2DRenderer::SortItems(std::vector<SortItem>& items)
{
    // Lower order draws first; equal orders keep collection order.
    std::sort(items.begin(), items.end(), [](const SortItem& a, const SortItem& b)
              { return a.order != b.order ? a.order < b.order : a.seq < b.seq; });
}

void Standard2DRenderer::CollectSortableItems(Deki::Object* obj, std::vector<SortItem>& items)
{
    // Every object that reaches the sort is active and was reached through
    // active parents (the walk starts at the scene roots), so RenderObject
    // need not check again.
    if (!obj || !obj->IsActive())
    {
        return;
    }

    const Renderables r = ResolveRenderables(obj);

    // Built-in components first: a renderer, then any other sortable
    // (Deki2D::ClipComponent, Deki2D::SortingGroupComponent, ...).
    if (r.renderer)
    {
        items.push_back({ obj, r.renderer, r.clip, r.renderer->sortingOrder, static_cast<uint32_t>(items.size()) });
        return;
    }
    if (r.sortable)
    {
        items.push_back({ obj, nullptr, r.clip, r.sortable->GetSortingOrder(), static_cast<uint32_t>(items.size()) });
        return;
    }

    // Then the custom sorting callbacks.
    int32_t order;
    for (SortingCallback cb : m_SortingCallbacks)
    {
        if (cb(obj, order))
        {
            items.push_back({ obj, nullptr, r.clip, order, static_cast<uint32_t>(items.size()) });
            return;
        }
    }

    // Nothing claimed it: a transparent container, so its children are
    // sorted at this level.
    for (auto* child : obj->GetChildren())
    {
        CollectSortableItems(child, items);
    }
}

// --- Main render loop ---

namespace
{
DrawView s_CurrentDrawView;
}

const DrawView& CurrentDrawView()
{
    return s_CurrentDrawView;
}

void SetCurrentDrawView(const DrawView& view)
{
    s_CurrentDrawView = view;
}

void Standard2DRenderer::Render(Deki::Scene* scene, const RenderContext& ctx)
{
    if (!scene || !ctx.camera || !ctx.buffer)
    {
        return;
    }

    QuadBlit::ClearClipStack();

    // A package that loaded (or reloaded) since the last frame may have
    // registered adapters for types already classified: start over.
    const uint32_t adapterVersion = Deki::ComponentInterfaceAdapters::Version();
    if (adapterVersion != m_TraitsVersion)
    {
        m_TypeTraits.clear();
        m_TraitsVersion = adapterVersion;
    }
    RebuildHookLists();

    // The frame's own context. Passes can swap frameCtx.buffer in BeginFrame
    // to set the render target for the whole frame; every RenderObject below
    // uses frameCtx, not the original ctx.
    RenderContext frameCtx = ctx;
    for (RenderPass* pass : m_BeginPasses)
    {
        pass->BeginFrame(frameCtx);
    }

    // Capture the camera once for the frame, against the target the passes
    // settled on. Everything below maps world to screen through this. A strip
    // captures it for the whole view, then moves it up to the strip's rows.
    const int32_t viewHeight = frameCtx.viewHeight > 0 ? frameCtx.viewHeight : frameCtx.height;
    frameCtx.cam = frameCtx.camera->CaptureFrameCamera(frameCtx.width, viewHeight);
    if (frameCtx.viewTop != 0)
    {
        frameCtx.cam.halfH -= static_cast<float>(frameCtx.viewTop);
    }
    SetCurrentDrawView({ frameCtx.cam.ppm, frameCtx.width, viewHeight });

    // Content kept from earlier strips belongs to another frame now.
    if (ctx.contentFrame == 0 || ctx.contentFrame != m_ContentCacheFrame)
    {
        m_ContentCache.clear();
        m_ContentCacheFrame = ctx.contentFrame;
    }

    // Dirty-rect tracking: QuadBlit records every blit into the caller's
    // buffer. A pass that installed its own frame target composites back into
    // the caller's buffer itself, without QuadBlit, so that frame is fully
    // dirty.
    m_FrameDirtyValid = ctx.trackDirty;
    if (ctx.trackDirty)
    {
        m_FrameDirty.Reset(ctx.width, ctx.height);
        if (frameCtx.buffer != ctx.buffer || frameCtx.width != ctx.width || frameCtx.height != ctx.height)
        {
            m_FrameDirty.SetFull();
        }
        QuadBlit::SetDirtyTracking(&m_FrameDirty, ctx.buffer);
    }

    m_SortDepth = 0;
    if (ctx.contentFrame != 0 && ctx.contentFrame == m_PlanFrame)
    {
        // A later strip of the frame: the order and geometry are recorded.
        m_PlanMode = PlanMode::Replaying;
        for (uint32_t i = 0; i < m_PlanRoots; i++)
        {
            RenderObject(m_Plan[i].item, frameCtx, static_cast<int32_t>(i));
        }
    }
    else
    {
        // Collect and sort the root objects.
        std::vector<SortItem>& sortableItems = SortListForDepth(0);

        for (Deki::Object* obj : scene->GetObjects())
        {
            CollectSortableItems(obj, sortableItems);
        }

        // And the persistent objects.
        const auto& persistentObjects = Deki::Engine::GetInstance().GetSceneSystem().GetPersistentObjects();
        for (Deki::Object* obj : persistentObjects)
        {
            CollectSortableItems(obj, sortableItems);
        }

        SortItems(sortableItems);

        // The first strip of a frame records it for the others: the roots
        // first, each object's children after it as they are reached.
        m_PlanMode = ctx.contentFrame != 0 ? PlanMode::Recording : PlanMode::Off;
        m_Plan.clear();
        m_PlanRoots = 0;
        m_PlanFrame = 0;
        if (m_PlanMode == PlanMode::Recording)
        {
            for (const SortItem& item : sortableItems)
            {
                PlanEntry entry;
                entry.item = item;
                m_Plan.push_back(entry);
            }
            m_PlanRoots = static_cast<uint32_t>(sortableItems.size());
        }

        // Render in sorted order. An index loop is safe: RenderObject recurses,
        // but deeper levels use their own scratch lists, so this one stays put.
        for (size_t i = 0; i < sortableItems.size(); i++)
        {
            RenderObject(sortableItems[i], frameCtx,
                         m_PlanMode == PlanMode::Recording ? static_cast<int32_t>(i) : -1);
        }

        if (m_PlanMode == PlanMode::Recording)
        {
            m_PlanFrame = ctx.contentFrame;
        }
    }
    m_PlanMode = PlanMode::Off;

    // Post-frame composites (e.g. screen-space overlays).
    for (auto it = m_EndPasses.rbegin(); it != m_EndPasses.rend(); ++it)
    {
        (*it)->EndFrame(frameCtx);
    }
    SetCurrentDrawView({});

    if (ctx.trackDirty)
    {
        QuadBlit::SetDirtyTracking(nullptr, nullptr);
    }
}

void Standard2DRenderer::RenderObject(const SortItem& item, const RenderContext& ctx, int32_t planIndex)
{
    Deki::Object* obj = item.obj;
    RenderContext objCtx = ctx;

    // Phase 1: PreExecute passes, which may redirect ctx.buffer for this object.
    for (RenderPass* pass : m_PrePasses)
    {
        pass->PreExecute(obj, objCtx);
    }

    // Phase 2: built-ins (sprite blit), into any target PreExecute redirected
    // to. Recording, the entry's geometry is filled in; replaying, reused.
    // (Taken by index here: m_Plan only grows below, after this call.)
    ExecuteBuiltins(item, objCtx, planIndex >= 0 ? &m_Plan[planIndex].geo : nullptr);

    // Phase 3: Execute passes (tilemap draw, etc.).
    for (RenderPass* pass : m_ExecPasses)
    {
        pass->Execute(obj, objCtx);
    }

    // Phase 4: the sorted children, with objCtx, so they inherit any buffer
    // redirect this object's PreExecute or Execute hooks applied.
    if (m_PlanMode == PlanMode::Replaying)
    {
        // m_Plan does not change while replaying, so entries stay put.
        const uint32_t first = m_Plan[planIndex].firstChild;
        const uint32_t count = m_Plan[planIndex].childCount;
        for (uint32_t i = 0; i < count; i++)
        {
            RenderObject(m_Plan[first + i].item, objCtx, static_cast<int32_t>(first + i));
        }
    }
    else
    {
        ++m_SortDepth;
        std::vector<SortItem>& childItems = SortListForDepth(m_SortDepth);
        for (auto* child : obj->GetChildren())
        {
            CollectSortableItems(child, childItems);
        }

        SortItems(childItems);

        // Recording: the children's entries go in one block, and each child
        // records its own children after it. Indices, not references: the
        // plan grows as the recursion goes.
        uint32_t first = 0;
        if (m_PlanMode == PlanMode::Recording)
        {
            first = static_cast<uint32_t>(m_Plan.size());
            for (const SortItem& child : childItems)
            {
                PlanEntry entry;
                entry.item = child;
                m_Plan.push_back(entry);
            }
            m_Plan[planIndex].firstChild = first;
            m_Plan[planIndex].childCount = static_cast<uint32_t>(childItems.size());
        }

        for (size_t i = 0; i < childItems.size(); i++)
        {
            RenderObject(childItems[i], objCtx,
                         m_PlanMode == PlanMode::Recording ? static_cast<int32_t>(first + i) : -1);
        }
        --m_SortDepth;
    }

    // Phase 5: PostExecute passes, in reverse order.
    for (auto it = m_PostPasses.rbegin(); it != m_PostPasses.rend(); ++it)
    {
        (*it)->PostExecute(obj, objCtx);
    }

    // Phase 6: built-ins again (clip pop).
    PostExecuteBuiltins(item);
}

}  // namespace DekiRendering
