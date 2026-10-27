#include "Standard2DRenderer.h"
#include "Fingerprint.h"
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
#include <cstring>

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

bool Standard2DRenderer::CoversView(const RenderContext& ctx) const
{
    // A later strip replays what the first recorded, cover included. The
    // first strip is not known until it is drawn, so it is cleared.
    return m_PlanCovers && ctx.contentFrame != 0 && ctx.contentFrame == m_PlanFrame;
}

void Standard2DRenderer::RunClear()
{
    if (m_Clear)
    {
        const DeferredClear* clear = m_Clear;
        m_Clear = nullptr;
        clear->run(clear->owner);
    }
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
    // frame, and only then moved to the strip by whole pixels, so a strip
    // truncates and rounds every edge the way the whole frame does. (ctx.cam
    // is the strip's: the whole view's moved up by viewTop and left by
    // viewLeft, which is exact.)
    FrameCamera cam = ctx.cam;
    cam.halfH += static_cast<float>(ctx.viewTop);
    cam.halfW += static_cast<float>(ctx.viewLeft);
    const int32_t viewTop = ctx.viewTop;
    const int32_t viewLeft = ctx.viewLeft;

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
        const int32_t left = g.clipLeft - viewLeft;
        const int32_t top = g.clipTop - viewTop;
        QuadBlit::PushClipRect(left, top, left + g.clipWidth, top + g.clipHeight);
    }

    // Renderer (a sprite, say): blit its content. FindChanges' walk
    // (measuring) works out what the blit would be and notes it instead.
    RendererComponent* renderer = item.renderer;
    if (renderer)
    {
        const bool measuring = m_PlanMode == PlanMode::Measuring;
        const uint32_t drawIndex = measuring ? m_DrawIndex++ : 0;
        const bool useOrderedDither = (renderer->alphaMode == AlphaMode::OrderedDither);

        float fScreenX = g.screenX, fScreenY = g.screenY;

        // A strip, or a part redrawn, outside the pixels an earlier strip (or
        // FindChanges) found the blit writes: nothing to draw here.
        if (g.areaKnown)
        {
            int32_t left = viewLeft, top = viewTop, right = viewLeft + ctx.width, bottom = viewTop + ctx.height;
            if (!ctx.redraw.Empty())
            {
                left = viewLeft + ctx.redraw.left;
                top = viewTop + ctx.redraw.top;
                right = viewLeft + ctx.redraw.right;
                bottom = viewTop + ctx.redraw.bottom;
            }
            if (g.areaBottom <= top || g.areaTop >= bottom || g.areaRight <= left || g.areaLeft >= right)
            {
                return;
            }
        }

        // Cull before RenderContent, which may rasterise text, bake a gradient
        // or copy a frame.
        if (g.hasExtents)
        {
            const float cx = fScreenX - static_cast<float>(viewLeft), cy = fScreenY - static_cast<float>(viewTop);
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
                if (measuring)
                {
                    g.areaKnown = true;  // nothing, in every part drawn
                    NoteDraw(renderer, 0, Deki::Rect{});
                }
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
        const CachedContent* cached = geo && ctx.contentFrame != 0 && g.contentKnown ? &g.content : nullptr;
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
            if (geo && ctx.contentFrame != 0 && !(hasContent && source.ownsPixels))
            {
                g.content = CachedContent{ hasContent, source, pivotX, pivotY, tintR, tintG, tintB, tintA };
                g.contentKnown = true;
            }
        }
        if (measuring && !hasContent)
        {
            g.areaKnown = true;  // nothing
            NoteDraw(renderer, 0, Deki::Rect{});
            return;
        }
        if (measuring && source.ownsPixels)
        {
            // Made again when the frame is drawn, and maybe not the same:
            // where it lands then is unknown, so the whole view is redrawn.
            if (source.pixels)
            {
                Deki::Memory::Free(const_cast<uint8_t*>(source.pixels));
            }
            m_Changes->SetFull();
            NoteDraw(renderer, 0, Deki::Rect{});
            return;
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
            const int32_t intScreenX =
                ((renderer->pixelSnap || cam.snapStep > 0) ? static_cast<int32_t>(std::lround(fScreenX))
                                                           : static_cast<int32_t>(fScreenX)) -
                viewLeft;
            const int32_t intScreenY =
                ((renderer->pixelSnap || cam.snapStep > 0) ? static_cast<int32_t>(std::lround(fScreenY))
                                                           : static_cast<int32_t>(fScreenY)) -
                viewTop;

            // Recording a frame's first strip: does this draw cover the whole
            // view? (Content handed over with ownsPixels is produced again in
            // each strip, so it may not be the same there.)
            if (BuildingPlan() && !m_PlanCovers && ctx.buffer == m_FrameBuffer && !source.ownsPixels)
            {
                const int32_t viewWidth = ctx.viewWidth > 0 ? ctx.viewWidth : ctx.width;
                const int32_t viewHeight = ctx.viewHeight > 0 ? ctx.viewHeight : ctx.height;
                m_PlanCovers = QuadBlit::CoversRect(source, intScreenX, intScreenY, drawScaleX, drawScaleY, wt.rotation,
                                                    pivotX, pivotY, tintA, -viewLeft, -viewTop, viewWidth - viewLeft,
                                                    viewHeight - viewTop);
            }

            // What it writes, for the frame's other strips (their content is
            // this one: content made again for each strip may not be).
            if (geo && !g.areaKnown && ctx.contentFrame != 0 && !source.ownsPixels)
            {
                int32_t left = 0, top = 0, right = 0, bottom = 0;
                if (!QuadBlit::BlitArea(source, intScreenX, intScreenY, drawScaleX, drawScaleY, wt.rotation, pivotX,
                                        pivotY, left, top, right, bottom))
                {
                    left = top = right = bottom = 0;
                }
                g.areaLeft = left + viewLeft;
                g.areaTop = top + viewTop;
                g.areaRight = right + viewLeft;
                g.areaBottom = bottom + viewTop;
                g.areaKnown = true;
            }

            if (measuring)
            {
                // The pixels it writes: within its clip, if it keeps to it,
                // and within the view.
                Deki::Rect area{ g.areaLeft, g.areaTop, g.areaRight, g.areaBottom };
                if (QuadBlit::IsClipEnabled())
                {
                    const QuadBlit::ClipRect clip = QuadBlit::GetCurrentClipRect();
                    area.left = std::max(area.left, clip.left);
                    area.top = std::max(area.top, clip.top);
                    area.right = std::min(area.right, clip.right);
                    area.bottom = std::min(area.bottom, clip.bottom);
                }
                area.left = std::max<int32_t>(area.left, 0);
                area.top = std::max<int32_t>(area.top, 0);
                area.right = std::min(area.right, ctx.width);
                area.bottom = std::min(area.bottom, ctx.height);
                if (area.Empty())
                {
                    area = Deki::Rect{};
                }

                // Everything that decides the pixels, field by field (no
                // padding bytes). A component that cannot say when its pixels
                // change in place gets a signature no other frame repeats.
                uint32_t version = 0;
                if (!renderer->GetContentVersion(version))
                {
                    version = ++m_AlwaysChanged;
                }
                Fingerprint sig;
                sig.Add(source.pixels);
                sig.Add(source.width);
                sig.Add(source.height);
                sig.Add(source.stride);
                sig.Add(source.bytesPerPixel);
                sig.Add(static_cast<uint32_t>(source.hasAlpha) | (static_cast<uint32_t>(source.isRGB565) << 1) |
                        (static_cast<uint32_t>(source.hasChromaKey) << 2) | (static_cast<uint32_t>(source.flipH) << 3) |
                        (static_cast<uint32_t>(source.flipV) << 4) | (static_cast<uint32_t>(source.flipD) << 5) |
                        (static_cast<uint32_t>(useOrderedDither) << 6));
                sig.Add(static_cast<uint32_t>(source.keyR) | (static_cast<uint32_t>(source.keyG) << 8) |
                        (static_cast<uint32_t>(source.keyB) << 16));
                sig.Add(source.stretchWidth);
                sig.Add(source.stretchHeight);
                sig.Add(pivotX);
                sig.Add(pivotY);
                sig.Add(static_cast<uint32_t>(tintR) | (static_cast<uint32_t>(tintG) << 8) |
                        (static_cast<uint32_t>(tintB) << 16) | (static_cast<uint32_t>(tintA) << 24));
                sig.Add(intScreenX);
                sig.Add(intScreenY);
                sig.Add(drawScaleX);
                sig.Add(drawScaleY);
                sig.Add(wt.rotation);
                sig.Add(version);
                sig.Add(drawIndex);
                NoteDraw(renderer, sig.Value(), area);
                if (renderer->ignoreClip)
                {
                    QuadBlit::SetClipEnabled(wasClipEnabled);
                }
                return;
            }

            // The frame's first draw: when it covers what is drawn opaquely,
            // the clear would only be drawn over.
            if (m_Clear)
            {
                const Deki::Rect cover = ctx.redraw.Empty() ? Deki::Rect{ 0, 0, ctx.width, ctx.height } : ctx.redraw;
                if (ctx.buffer == m_FrameBuffer &&
                    QuadBlit::CoversRect(source, intScreenX, intScreenY, drawScaleX, drawScaleY, wt.rotation, pivotX,
                                         pivotY, tintA, cover.left, cover.top, cover.right, cover.bottom))
                {
                    m_Clear = nullptr;
                }
                else
                {
                    RunClear();
                }
            }

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
    m_Clear = ctx.clear && ctx.clear->run ? ctx.clear : nullptr;
    if (!scene || !ctx.camera || !ctx.buffer)
    {
        RunClear();
        return;
    }

    QuadBlit::ClearClipStack();
    m_FrameBuffer = ctx.buffer;
    // Drawing only part of the buffer (FindChanges): nothing writes outside it.
    if (!ctx.redraw.Empty())
    {
        QuadBlit::SetTargetLimit(ctx.redraw.left, ctx.redraw.top, ctx.redraw.right, ctx.redraw.bottom);
    }

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
    //
    // The clear waits for the frame's first draw, which may cover it. Passes
    // may write anywhere, so it is made before any of them runs; an Execute
    // pass only for an object it wants.
    RenderContext frameCtx = ctx;
    frameCtx.clear = nullptr;
    if (!m_BeginPasses.empty())
    {
        RunClear();
    }
    for (RenderPass* pass : m_BeginPasses)
    {
        pass->BeginFrame(frameCtx);
    }

    // Capture the camera once for the frame, against the target the passes
    // settled on. Everything below maps world to screen through this. A strip
    // captures it for the whole view, then moves it to the strip's pixels.
    const int32_t viewWidth = frameCtx.viewWidth > 0 ? frameCtx.viewWidth : frameCtx.width;
    const int32_t viewHeight = frameCtx.viewHeight > 0 ? frameCtx.viewHeight : frameCtx.height;
    frameCtx.cam = frameCtx.camera->CaptureFrameCamera(viewWidth, viewHeight);
    frameCtx.cam.halfW -= static_cast<float>(frameCtx.viewLeft);
    frameCtx.cam.halfH -= static_cast<float>(frameCtx.viewTop);
    SetCurrentDrawView({ frameCtx.cam.ppm, viewWidth, viewHeight });

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
        m_PlanCovers = false;
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
            RenderObject(sortableItems[i], frameCtx, m_PlanMode == PlanMode::Recording ? static_cast<int32_t>(i) : -1);
        }

        if (m_PlanMode == PlanMode::Recording)
        {
            m_PlanFrame = ctx.contentFrame;
        }
    }
    m_PlanMode = PlanMode::Off;

    // Nothing was drawn: the clear is all there is.
    RunClear();

    // Post-frame composites (e.g. screen-space overlays).
    for (auto it = m_EndPasses.rbegin(); it != m_EndPasses.rend(); ++it)
    {
        (*it)->EndFrame(frameCtx);
    }
    SetCurrentDrawView({});

    QuadBlit::ClearTargetLimit();
}

bool Standard2DRenderer::FindChanges(Deki::Scene* scene, const RenderContext& ctx, DirtyRegion& changed)
{
    // The frames it is asked about are drawn by replaying its walk, which
    // needs a frame id; and a pass with hooks other than Execute may draw
    // anywhere, into anything.
    if (!scene || !ctx.camera || ctx.contentFrame == 0)
    {
        ForgetRecords();
        return false;
    }
    const uint32_t adapterVersion = Deki::ComponentInterfaceAdapters::Version();
    if (adapterVersion != m_TraitsVersion)
    {
        m_TypeTraits.clear();
        m_TraitsVersion = adapterVersion;
    }
    RebuildHookLists();
    if (!m_BeginPasses.empty() || !m_PrePasses.empty() || !m_PostPasses.empty() || !m_EndPasses.empty())
    {
        ForgetRecords();
        return false;
    }

    // The whole view, whatever part of it ctx describes.
    const int32_t viewWidth = ctx.viewWidth > 0 ? ctx.viewWidth : ctx.width;
    const int32_t viewHeight = ctx.viewHeight > 0 ? ctx.viewHeight : ctx.height;
    RenderContext frameCtx = ctx;
    frameCtx.width = viewWidth;
    frameCtx.height = viewHeight;
    frameCtx.viewTop = 0;
    frameCtx.viewLeft = 0;
    frameCtx.viewWidth = viewWidth;
    frameCtx.viewHeight = viewHeight;
    frameCtx.clear = nullptr;
    frameCtx.redraw = Deki::Rect{};
    frameCtx.cam = frameCtx.camera->CaptureFrameCamera(viewWidth, viewHeight);
    SetCurrentDrawView({ frameCtx.cam.ppm, viewWidth, viewHeight });

    // Another camera, view or scene moves everything: nothing compares.
    changed.Reset(viewWidth, viewHeight);
    const uint32_t sceneSerial = Deki::Engine::GetInstance().GetSceneSystem().GetRootSceneSerial();
    const FrameCamera& cam = frameCtx.cam;
    const FrameCamera& was = m_RecordCam;
    if (!m_HaveRecords || viewWidth != m_RecordViewWidth || viewHeight != m_RecordViewHeight ||
        sceneSerial != m_RecordScene || cam.camX != was.camX || cam.camY != was.camY || cam.ppm != was.ppm ||
        cam.halfW != was.halfW || cam.halfH != was.halfH || cam.snapStep != was.snapStep)
    {
        m_Records.clear();
        changed.SetFull();
    }

    QuadBlit::ClearClipStack();
    m_Clear = nullptr;
    m_FrameBuffer = ctx.buffer;
    m_Changes = &changed;
    m_DrawIndex = 0;
    if (++m_RecordCall == 0)
    {
        m_RecordCall = 1;  // 0 marks a record never seen
    }

    // The walk Render() makes, recorded as for a frame's first strip, but
    // measuring instead of drawing. Render() calls with this frame id replay it.
    m_SortDepth = 0;
    std::vector<SortItem>& roots = SortListForDepth(0);
    for (Deki::Object* obj : scene->GetObjects())
    {
        CollectSortableItems(obj, roots);
    }
    for (Deki::Object* obj : Deki::Engine::GetInstance().GetSceneSystem().GetPersistentObjects())
    {
        CollectSortableItems(obj, roots);
    }
    SortItems(roots);
    m_PlanMode = PlanMode::Measuring;
    m_Plan.clear();
    m_PlanCovers = false;
    for (const SortItem& item : roots)
    {
        PlanEntry entry;
        entry.item = item;
        m_Plan.push_back(entry);
    }
    m_PlanRoots = static_cast<uint32_t>(roots.size());
    for (size_t i = 0; i < roots.size(); i++)
    {
        RenderObject(roots[i], frameCtx, static_cast<int32_t>(i));
    }
    m_PlanMode = PlanMode::Off;
    m_PlanFrame = ctx.contentFrame;

    for (RenderPass* pass : m_ExecPasses)
    {
        if (pass->ReportsChanges())
        {
            pass->FinishChanges(frameCtx, changed);
        }
    }

    // Renderers not met this time (destroyed, inactive) leave their pixels.
    for (auto it = m_Records.begin(); it != m_Records.end();)
    {
        if (it->second.seen != m_RecordCall)
        {
            changed.Add(it->second.area);
            it = m_Records.erase(it);
        }
        else
        {
            ++it;
        }
    }

    m_Changes = nullptr;
    m_HaveRecords = true;
    m_RecordCam = cam;
    m_RecordViewWidth = viewWidth;
    m_RecordViewHeight = viewHeight;
    m_RecordScene = sceneSerial;
    QuadBlit::ClearClipStack();
    SetCurrentDrawView({});
    return true;
}

void Standard2DRenderer::NoteDraw(const RendererComponent* renderer, uint64_t signature, const Deki::Rect& area)
{
    DrawRecord& record = m_Records[renderer];
    const bool met = record.seen != 0;
    const Deki::Rect& old = record.area;
    if (!met || record.signature != signature || old.left != area.left || old.top != area.top ||
        old.right != area.right || old.bottom != area.bottom)
    {
        if (met)
        {
            m_Changes->Add(old);
        }
        m_Changes->Add(area);
    }
    record.signature = signature;
    record.area = area;
    record.seen = m_RecordCall;
}

void Standard2DRenderer::ForgetRecords()
{
    m_Records.clear();
    m_HaveRecords = false;
}

void Standard2DRenderer::RenderObject(const SortItem& item, const RenderContext& ctx, int32_t planIndex)
{
    Deki::Object* obj = item.obj;
    RenderContext objCtx = ctx;

    // Phase 1: PreExecute passes, which may redirect ctx.buffer for this object.
    if (!m_PrePasses.empty())
    {
        RunClear();
    }
    for (RenderPass* pass : m_PrePasses)
    {
        pass->PreExecute(obj, objCtx);
    }

    // Phase 2: built-ins (sprite blit), into any target PreExecute redirected
    // to. Recording, the entry's geometry is filled in; replaying, reused.
    // (Taken by index here: m_Plan only grows below, after this call.)
    ExecuteBuiltins(item, objCtx, planIndex >= 0 ? &m_Plan[planIndex].geo : nullptr);

    // Phase 3: Execute passes (tilemap draw, etc.). Recording, each says
    // whether it wants the object; replaying, only those that did run.
    for (size_t i = 0; i < m_ExecPasses.size(); i++)
    {
        RenderPass* pass = m_ExecPasses[i];
        if (i < 32)
        {
            const uint32_t bit = 1u << i;
            if (m_PlanMode == PlanMode::Replaying && !(m_Plan[planIndex].execPasses & bit))
            {
                continue;
            }
            if (BuildingPlan() && !pass->WantsObject(obj))
            {
                m_Plan[planIndex].execPasses &= ~bit;
                continue;
            }
        }
        if (m_PlanMode == PlanMode::Measuring)
        {
            // Where the pass's drawing changed, if it can say; otherwise
            // anywhere.
            if (i < 32 || pass->WantsObject(obj))
            {
                if (pass->ReportsChanges())
                {
                    pass->AddChanges(obj, objCtx, *m_Changes);
                }
                else
                {
                    m_Changes->SetFull();
                }
            }
            continue;
        }
        if (m_Clear && pass->WantsObject(obj))
        {
            RunClear();
        }
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
        if (BuildingPlan())
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
            RenderObject(childItems[i], objCtx, BuildingPlan() ? static_cast<int32_t>(first + i) : -1);
        }
        --m_SortDepth;
    }

    // Phase 5: PostExecute passes, in reverse order.
    if (!m_PostPasses.empty())
    {
        RunClear();
    }
    for (auto it = m_PostPasses.rbegin(); it != m_PostPasses.rend(); ++it)
    {
        (*it)->PostExecute(obj, objCtx);
    }

    // Phase 6: built-ins again (clip pop).
    PostExecuteBuiltins(item);
}

}  // namespace DekiRendering
