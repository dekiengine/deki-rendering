#include "Standard2DRenderer.h"

#include <deki/assets/AssetManager.h>
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
#include <new>

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
    QuadBlit::RunPendingClear();
}

void Standard2DRenderer::RunClearFor(const std::vector<RenderPass*>& passes)
{
    // A pass that says no writes through QuadBlit, which clears first, or
    // calls QuadBlit::BeforeWriting.
    for (const RenderPass* pass : passes)
    {
        if (pass->WritesBuffer())
        {
            RunClear();
            return;
        }
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

    // Measuring a renderer that says what its drawing depends on: what the
    // last measure found of it, when that has not changed (ReuseEntry).
    RendererComponent* const measured = m_PlanMode == PlanMode::Measuring ? item.renderer : nullptr;
    const bool isPiece = item.piece != kNoPiece;
    DrawRecord* record = measured ? &m_Records[RecordKey{ measured, item.piece }] : nullptr;
    uint64_t drawKey = 0;
    bool keyed = false;
    if (measured)
    {
        uint64_t own = 0;
        keyed = isPiece ? measured->GetPieceDrawKey(item.piece, own) : measured->GetDrawKey(own);
        if (keyed)
        {
            // With what this renderer decides from the component's own base.
            Fingerprint k;
            k.Add(static_cast<uint32_t>(own));
            k.Add(static_cast<uint32_t>(own >> 32));
            k.Add(static_cast<uint32_t>(measured->alphaMode) | (static_cast<uint32_t>(measured->pixelSnap) << 8) |
                  (static_cast<uint32_t>(measured->ignoreClip) << 9));
            drawKey = k.Value();
        }
    }
    ReuseEntry* kept = nullptr;
    if (keyed && m_ReuseOn && record->reuseSlot != 0)
    {
        ReuseEntry& e = m_Reuse[record->reuseSlot - 1];
        if (e.valid && e.key == drawKey)
        {
            kept = &e;
        }
    }

    // The whole-view geometry: the same in every strip of a frame, so a later
    // strip takes it from the first.
    BuiltinGeometry local;
    BuiltinGeometry& g = geo ? *geo : local;
    if (!g.valid)
    {
        g.wt = obj->GetWorldTransform();  // one dirty check for all five values
        cam.WorldToScreen(g.wt.x, g.wt.y, g.screenX, g.screenY);
        // The object's clip is where the object is, whichever piece this is.
        const float ownerScreenX = g.screenX, ownerScreenY = g.screenY;
        if (isPiece)
        {
            // As Object::RecomputeWorldTransform places a child there.
            float px = 0.0f, py = 0.0f;
            int32_t unusedOrder = 0;
            item.renderer->GetPiecePlace(item.piece, px, py, unusedOrder);
            const float scaledX = px * g.wt.scaleX;
            const float scaledY = py * g.wt.scaleY;
            if (g.wt.rotation == 0.0f)
            {
                g.wt.x += scaledX;
                g.wt.y += scaledY;
            }
            else
            {
                const float cosR = std::cos(g.wt.rotation);
                const float sinR = std::sin(g.wt.rotation);
                g.wt.x += scaledX * cosR - scaledY * sinR;
                g.wt.y += scaledX * sinR + scaledY * cosR;
            }
            cam.WorldToScreen(g.wt.x, g.wt.y, g.screenX, g.screenY);
        }
        if (item.clip)
        {
            const int32_t screenX = static_cast<int32_t>(std::floor(ownerScreenX));
            const int32_t screenY = static_cast<int32_t>(std::floor(ownerScreenY));
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
        if (kept)
        {
            g.hasExtents = kept->hasExtents;
            extentW = kept->extentW;
            extentH = kept->extentH;
        }
        else
        {
            g.hasExtents = item.renderer && (isPiece ? item.renderer->GetPieceExtents(item.piece, extentW, extentH)
                                                     : item.renderer->GetContentExtents(extentW, extentH));
        }
        if (measured)
        {
            // Kept for the next measure; the content below when there is one.
            ReuseEntry* e = keyed ? ReuseFor(*record, true) : (record->reuseSlot ? ReuseFor(*record, false) : nullptr);
            if (e && e != kept)
            {
                e->valid = keyed;
                e->key = drawKey;
                e->hasExtents = g.hasExtents;
                e->extentW = extentW;
                e->extentH = extentH;
                e->contentKnown = false;
            }
        }
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
                    NoteDraw(renderer, 0, Deki::Rect{}, Deki::Rect{}, 0, Deki::Rect{}, 0, record);
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
        // The same content as measured last time, at the same scale and turn.
        const bool sameShape = kept && kept->contentKnown && kept->scaleX == wt.scaleX && kept->scaleY == wt.scaleY &&
                               kept->rotation == wt.rotation;
        if (sameShape && geo && ctx.contentFrame != 0 && !g.contentKnown)
        {
            g.contentAt = static_cast<int32_t>(record->reuseSlot);  // where it is kept: not copied
            g.contentKnown = true;
        }
        const CachedContent* cached = geo && ctx.contentFrame != 0 && g.contentKnown ? &ContentOf(g) : nullptr;
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
            hasContent = isPiece ? renderer->RenderPieceContent(item.piece, obj, source, pivotX, pivotY, tintR, tintG,
                                                                tintB, tintA)
                                 : renderer->RenderContent(obj, source, pivotX, pivotY, tintR, tintG, tintB, tintA);
            if (profiling)
            {
                const Deki::ComponentTypeInfo* info = renderer->GetTypeInfo();
                const uint64_t contentUs = RenderProfiler::Now() - contentStart;
                RenderProfiler::AddContent(renderer->GetType(), contentUs, info ? info->shortName : nullptr);
                // Made while finding what changed, it counts there.
                if (m_PlanMode != PlanMode::Measuring)
                {
                    RenderProfiler::AddObjectDraw(renderer, contentUs, 0, 0);
                }
            }
            if (geo && ctx.contentFrame != 0 && !(hasContent && source.ownsPixels))
            {
                // Kept where the next measure looks for it, when the renderer
                // has a place there; otherwise for this frame only.
                const CachedContent made{ hasContent, source, pivotX, pivotY, tintR, tintG, tintB, tintA };
                ReuseEntry* e = keyed ? ReuseFor(*record, false) : nullptr;
                if (e && e->valid)
                {
                    m_ReuseContent[record->reuseSlot - 1] = made;
                    g.contentAt = static_cast<int32_t>(record->reuseSlot);
                }
                else
                {
                    m_PlanContent.push_back(made);
                    g.contentAt = -static_cast<int32_t>(m_PlanContent.size());
                }
                g.contentKnown = true;
            }
        }
        if (measuring && !hasContent)
        {
            g.areaKnown = true;  // nothing
            NoteDraw(renderer, 0, Deki::Rect{}, Deki::Rect{}, 0, Deki::Rect{}, 0, record);
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
            NoteDraw(renderer, 0, Deki::Rect{}, Deki::Rect{}, 0, Deki::Rect{}, 0, record);
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
                if (sameShape)
                {
                    if (!kept->relEmpty)
                    {
                        left = intScreenX + kept->relLeft;
                        top = intScreenY + kept->relTop;
                        right = intScreenX + kept->relRight;
                        bottom = intScreenY + kept->relBottom;
                    }
                }
                else if (!QuadBlit::BlitArea(source, intScreenX, intScreenY, drawScaleX, drawScaleY, wt.rotation,
                                             pivotX, pivotY, left, top, right, bottom))
                {
                    left = top = right = bottom = 0;
                }
                // Drawn 1:1, only its non-transparent box: an animation
                // frame's empty margin writes nothing.
                Deki::Rect seen;
                if (!sameShape && right > left && drawScaleX == 1.0f && drawScaleY == 1.0f && wt.rotation == 0.0f &&
                    VisibleBoundsOf(source, seen))
                {
                    if (seen.Empty())
                    {
                        right = left;
                        bottom = top;
                    }
                    else
                    {
                        right = left + seen.right;
                        bottom = top + seen.bottom;
                        left += seen.left;
                        top += seen.top;
                    }
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
                // With no clip set the current one still starts at 0, 0: that
                // edge is the view's, cut below, and must not cut the reach.
                const QuadBlit::ClipRect clip = QuadBlit::GetCurrentClipRect();
                if (QuadBlit::IsClipEnabled() && clip.IsSet())
                {
                    area.left = std::max(area.left, clip.left);
                    area.top = std::max(area.top, clip.top);
                    area.right = std::min(area.right, clip.right);
                    area.bottom = std::min(area.bottom, clip.bottom);
                }
                const Deki::Rect reach = area;  // before the view's edges cut it
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
                Fingerprint sig;
                if (sameShape)
                {
                    sig = kept->partial;
                }
                else
                {
                    uint32_t version = 0;
                    if (!(isPiece ? renderer->GetPieceVersion(item.piece, version)
                                  : renderer->GetContentVersion(version)))
                    {
                        version = ++m_AlwaysChanged;
                    }
                    sig.Add(source.pixels);
                    sig.Add(source.width);
                    sig.Add(source.height);
                    sig.Add(source.stride);
                    sig.Add(source.bytesPerPixel);
                    sig.Add(static_cast<uint32_t>(source.hasAlpha) | (static_cast<uint32_t>(source.isRGB565) << 1) |
                            (static_cast<uint32_t>(source.hasChromaKey) << 2) |
                            (static_cast<uint32_t>(source.flipH) << 3) | (static_cast<uint32_t>(source.flipV) << 4) |
                            (static_cast<uint32_t>(source.flipD) << 5) |
                            (static_cast<uint32_t>(useOrderedDither) << 6));
                    sig.Add(static_cast<uint32_t>(source.keyR) | (static_cast<uint32_t>(source.keyG) << 8) |
                            (static_cast<uint32_t>(source.keyB) << 16));
                    sig.Add(source.stretchWidth);
                    sig.Add(source.stretchHeight);
                    sig.Add(pivotX);
                    sig.Add(pivotY);
                    sig.Add(static_cast<uint32_t>(tintR) | (static_cast<uint32_t>(tintG) << 8) |
                            (static_cast<uint32_t>(tintB) << 16) | (static_cast<uint32_t>(tintA) << 24));
                    sig.Add(drawScaleX);
                    sig.Add(drawScaleY);
                    sig.Add(wt.rotation);
                    sig.Add(version);
                    // Kept for the next measure, with the content and its area.
                    ReuseEntry* e = keyed ? ReuseFor(*record, false) : nullptr;
                    if (e && e->valid && g.areaKnown && g.contentKnown)
                    {
                        e->contentKnown = true;
                        e->scaleX = wt.scaleX;
                        e->scaleY = wt.scaleY;
                        e->rotation = wt.rotation;
                        if (g.contentAt != static_cast<int32_t>(record->reuseSlot))
                        {
                            m_ReuseContent[record->reuseSlot - 1] = ContentOf(g);
                        }
                        e->partial = sig;
                        e->relEmpty = g.areaRight <= g.areaLeft || g.areaBottom <= g.areaTop;
                        e->relLeft = g.areaLeft - intScreenX;
                        e->relTop = g.areaTop - intScreenY;
                        e->relRight = g.areaRight - intScreenX;
                        e->relBottom = g.areaBottom - intScreenY;
                    }
                }
                sig.Add(drawIndex);
                Fingerprint onScreen = sig;
                sig.Add(intScreenX - m_ScrollOriginX);  // where it is in the world, as the frame scrolls
                sig.Add(intScreenY - m_ScrollOriginY);
                onScreen.Add(intScreenX);  // where it is on screen
                onScreen.Add(intScreenY);

                // Scrolling, a renderer drawn the same at the same place on
                // screen keeps the pixels of its opaque interior (drawn 1:1).
                // Looked up once: the records live in a hash table, and on a
                // board that is a slow read for every renderer every frame.
                Deki::Rect pin{};
                if ((m_ScrollX != 0 || m_ScrollY != 0) && drawScaleX == 1.0f && drawScaleY == 1.0f &&
                    wt.rotation == 0.0f && tintA == 255)
                {
                    Deki::Rect inside;
                    int32_t blitL = 0, blitT = 0, blitR = 0, blitB = 0;
                    if (record->seen != 0 && record->screenSignature == onScreen.Value() &&
                        OpaqueInteriorOf(source, inside) &&
                        QuadBlit::BlitArea(source, intScreenX, intScreenY, drawScaleX, drawScaleY, wt.rotation, pivotX,
                                           pivotY, blitL, blitT, blitR, blitB))
                    {
                        pin = Deki::Rect{ std::max(area.left, blitL + inside.left),
                                          std::max(area.top, blitT + inside.top),
                                          std::min(area.right, blitL + inside.right),
                                          std::min(area.bottom, blitT + inside.bottom) };
                        if (pin.Empty())
                        {
                            pin = Deki::Rect{};
                        }
                    }
                }
                NoteDraw(renderer, sig.Value(), area, reach, onScreen.Value(), pin, drawIndex, record);
                if (renderer->ignoreClip)
                {
                    QuadBlit::SetClipEnabled(wasClipEnabled);
                }
                return;
            }

            // The frame's first draw: when it covers what is drawn opaquely,
            // the clear would only be drawn over.
            if (QuadBlit::PendingClear())
            {
                const Deki::Rect cover = ctx.redraw.Empty() ? Deki::Rect{ 0, 0, ctx.width, ctx.height } : ctx.redraw;
                if (ctx.buffer == m_FrameBuffer &&
                    QuadBlit::CoversRect(source, intScreenX, intScreenY, drawScaleX, drawScaleY, wt.rotation, pivotX,
                                         pivotY, tintA, cover.left, cover.top, cover.right, cover.bottom))
                {
                    QuadBlit::DropPendingClear();
                }
                else
                {
                    RunClear();
                }
            }

            const uint64_t blitStart = profiling ? RenderProfiler::Now() : 0;
            // Drawn 1:1 and unturned, only its box of non-transparent pixels
            // is blitted, as only that box counts as its area: the empty
            // margin of an animation frame, a character in the middle of its
            // cell, is then not read at all. Where the pixels are slow to
            // read (a sprite sheet in external memory) reading them is most
            // of a blit.
            QuadBlit::Source visible;
            int32_t visibleX = 0, visibleY = 0;
            int32_t blitLeft = 0, blitTop = 0, blitRight = 0, blitBottom = 0;
            if (!m_SinkFrame && drawScaleX == 1.0f && drawScaleY == 1.0f && wt.rotation == 0.0f &&
                VisiblePartOf(source, visible, visibleX, visibleY) &&
                QuadBlit::BlitArea(source, intScreenX, intScreenY, drawScaleX, drawScaleY, wt.rotation, pivotX, pivotY,
                                   blitLeft, blitTop, blitRight, blitBottom))
            {
                if (visible.width > 0 && visible.height > 0)
                {
                    QuadBlit::BlitScaled(visible, ctx.buffer, ctx.width, ctx.height, ctx.format, blitLeft + visibleX,
                                         blitTop + visibleY, visible.width, visible.height, tintR, tintG, tintB, tintA,
                                         useOrderedDither);
                }
            }
            else
            {
                QuadBlit::Blit(source, ctx.buffer, ctx.width, ctx.height, ctx.format, intScreenX, intScreenY,
                               drawScaleX, drawScaleY, wt.rotation, pivotX, pivotY, tintR, tintG, tintB, tintA,
                               useOrderedDither);
            }
            if (profiling)
            {
                const Deki::ComponentTypeInfo* info = renderer->GetTypeInfo();
                const uint64_t blitUs = RenderProfiler::Now() - blitStart;
                const uint64_t pixels = static_cast<uint64_t>(source.width) * static_cast<uint64_t>(source.height);
                RenderProfiler::AddBlit(renderer->GetType(), blitUs, pixels, info ? info->shortName : nullptr);
                RenderProfiler::AddObjectDraw(renderer, 0, blitUs, pixels);
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
        const uint32_t pieces = r.renderer->GetPieceCount();
        if (pieces == 0)
        {
            items.push_back({ obj, r.renderer, r.clip, r.renderer->sortingOrder, static_cast<uint32_t>(items.size()) });
            return;
        }
        // Each piece sorted on its own, as a child object would be.
        for (uint32_t i = 0; i < pieces; i++)
        {
            float x = 0.0f, y = 0.0f;
            int32_t order = 0;
            r.renderer->GetPiecePlace(i, x, y, order);
            items.push_back({ obj, r.renderer, r.clip, order, static_cast<uint32_t>(items.size()), i });
        }
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
    QuadBlit::SetPendingClear(ctx.buffer, ctx.clear && ctx.clear->run ? ctx.clear : nullptr);
    if (!scene || !ctx.camera || !ctx.buffer)
    {
        RunClear();
        return;
    }

    QuadBlit::ClearClipStack();
    m_FrameBuffer = ctx.buffer;
    m_SinkFrame = QuadBlit::SinkTarget() == ctx.buffer;
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
    // The clear waits for the frame's first draw, which may cover it. A pass
    // that may write anywhere has it made before its hooks run; an Execute
    // pass only for an object it wants.
    RenderContext frameCtx = ctx;
    frameCtx.clear = nullptr;
    RunClearFor(m_BeginPasses);
    for (RenderPass* pass : m_BeginPasses)
    {
        pass->BeginFrame(frameCtx);
        AfterHook(pass);
    }

    // Capture the camera once for the frame, against the target the passes
    // settled on. Everything below maps world to screen through this. A strip
    // captures it for the whole view, then moves it to the strip's pixels.
    const int32_t viewWidth = frameCtx.viewWidth > 0 ? frameCtx.viewWidth : frameCtx.width;
    const int32_t viewHeight = frameCtx.viewHeight > 0 ? frameCtx.viewHeight : frameCtx.height;
    frameCtx.cam = frameCtx.camera->CaptureFrameCamera(viewWidth, viewHeight);
    m_Lighting.Build(scene, frameCtx.cam, viewWidth, viewHeight, frameCtx.camera->litUpToSortOrder);
    m_Lit = false;
    frameCtx.cam.halfW -= static_cast<float>(frameCtx.viewLeft);
    frameCtx.cam.halfH -= static_cast<float>(frameCtx.viewTop);
    SetCurrentDrawView({ frameCtx.cam.ppm, viewWidth, viewHeight });

    m_SortDepth = 0;
    if (ctx.contentFrame != 0 && ctx.contentFrame == m_PlanFrame)
    {
        // A later strip of the frame: the order and geometry are recorded.
        m_PlanMode = PlanMode::Replaying;
        if (m_PlanAreas.size() != m_Plan.size())
        {
            FindPlanAreas();
        }
        // An object that draws nothing in this part is passed over from the
        // short list, unless a light is still to come: that is cast before
        // the first object above it, drawn here or not.
        const bool passOver = m_Lit || !m_Lighting.On();
        for (uint32_t i = 0; i < m_PlanRoots; i++)
        {
            if (passOver && OutsidePart(m_PlanAreas[i], frameCtx))
            {
                continue;
            }
            LightFrameIfDue(&m_Plan[i].item, frameCtx);
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
        m_PlanAreas.clear();
        m_PlanContent.clear();
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
            LightFrameIfDue(&sortableItems[i], frameCtx);
            RenderObject(sortableItems[i], frameCtx, m_PlanMode == PlanMode::Recording ? static_cast<int32_t>(i) : -1);
        }

        if (m_PlanMode == PlanMode::Recording)
        {
            m_PlanFrame = ctx.contentFrame;
        }
    }
    m_PlanMode = PlanMode::Off;

    // Nothing was drawn: the clear is all there is, and is lit if the frame is.
    LightFrameIfDue(nullptr, frameCtx);
    RunClear();

    // Post-frame composites (e.g. screen-space overlays).
    for (auto it = m_EndPasses.rbegin(); it != m_EndPasses.rend(); ++it)
    {
        (*it)->EndFrame(frameCtx);
        AfterHook(*it);
    }
    SetCurrentDrawView({});

    QuadBlit::ClearTargetLimit();
    m_SinkFrame = false;
}

namespace
{
// `a` less `b`, as up to four rects added to `out`.
void SubtractRect(const Deki::Rect& a, const Deki::Rect& b, std::vector<Deki::Rect>& out)
{
    if (a.Empty())
    {
        return;
    }
    if (b.Empty() || b.right <= a.left || b.left >= a.right || b.bottom <= a.top || b.top >= a.bottom)
    {
        out.push_back(a);
        return;
    }
    const int32_t top = std::max(a.top, b.top), bottom = std::min(a.bottom, b.bottom);
    if (a.top < top)
    {
        out.push_back(Deki::Rect{ a.left, a.top, a.right, top });
    }
    if (bottom < a.bottom)
    {
        out.push_back(Deki::Rect{ a.left, bottom, a.right, a.bottom });
    }
    if (a.left < b.left)
    {
        out.push_back(Deki::Rect{ a.left, top, b.left, bottom });
    }
    if (b.right < a.right)
    {
        out.push_back(Deki::Rect{ b.right, top, a.right, bottom });
    }
}
}  // namespace

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
    m_ScrollX = 0;
    m_ScrollY = 0;
    const bool sameView = m_HaveRecords && viewWidth == m_RecordViewWidth && viewHeight == m_RecordViewHeight &&
                          sceneSerial == m_RecordScene && cam.ppm == was.ppm && cam.halfW == was.halfW &&
                          cam.halfH == was.halfH && cam.snapStep == was.snapStep;
    const bool moved = cam.camX != was.camX || cam.camY != was.camY;
    bool scroll = false;
    if (sameView && moved && ctx.scrollable)
    {
        // A camera that moved by whole pixels, and sits on whole pixels,
        // moves every pixel of the view by the same whole amount (Y flipped).
        const float fx = (was.camX - cam.camX) * cam.ppm;
        const float fy = (cam.camY - was.camY) * cam.ppm;
        auto whole = [](float v) { return std::fabs(v - std::round(v)) < 1.0e-3f; };
        const int32_t dx = static_cast<int32_t>(std::lround(fx));
        const int32_t dy = static_cast<int32_t>(std::lround(fy));
        scroll = whole(fx) && whole(fy) && whole(cam.camX * cam.ppm) && whole(cam.camY * cam.ppm) && whole(cam.halfW) &&
                 whole(cam.halfH) && std::abs(dx) < viewWidth / 2 && std::abs(dy) < viewHeight / 2;
        // Every pass that reports its own changes must follow a scroll.
        for (RenderPass* pass : m_ExecPasses)
        {
            if (pass->ReportsChanges() && !pass->FollowsScroll())
            {
                scroll = false;
            }
        }
        if (scroll)
        {
            m_ScrollX = dx;
            m_ScrollY = dy;
        }
    }
    // What the last measure found may be reused only through the same view,
    // and with every asset as it was then (a reload moves the epoch).
    {
        const Deki::AssetManager* assets = Deki::AssetManager::Get();
        const uint64_t epoch = assets ? assets->GetEpoch() : 0;
        m_ReuseOn = sameView && epoch == m_ReuseEpoch;
        m_ReuseEpoch = epoch;
    }
    if (!sameView || (moved && !scroll))
    {
        m_Records.clear();
        ForgetReuse();
        m_ScrollOriginX = 0;
        m_ScrollOriginY = 0;
        changed.SetFull();
    }
    else if (scroll)
    {
        // What each renderer drew moves with the frame; one that does not land
        // there again compares as changed, as usual.
        for (auto& entry : m_Records)
        {
            for (Deki::Rect* r : { &entry.second.area, &entry.second.reach })
            {
                if (!r->Empty())
                {
                    r->left += m_ScrollX;
                    r->right += m_ScrollX;
                    r->top += m_ScrollY;
                    r->bottom += m_ScrollY;
                }
            }
        }
        m_ScrollOriginX += m_ScrollX;
        m_ScrollOriginY += m_ScrollY;
        frameCtx.scrollX = m_ScrollX;
        frameCtx.scrollY = m_ScrollY;
        // What the move uncovers.
        if (m_ScrollX > 0)
        {
            changed.Add(Deki::Rect{ 0, 0, m_ScrollX, viewHeight });
        }
        else if (m_ScrollX < 0)
        {
            changed.Add(Deki::Rect{ viewWidth + m_ScrollX, 0, viewWidth, viewHeight });
        }
        if (m_ScrollY > 0)
        {
            changed.Add(Deki::Rect{ 0, 0, viewWidth, m_ScrollY });
        }
        else if (m_ScrollY < 0)
        {
            changed.Add(Deki::Rect{ 0, viewHeight + m_ScrollY, viewWidth, viewHeight });
        }
    }

    QuadBlit::ClearClipStack();
    QuadBlit::SetPendingClear(nullptr, nullptr);
    m_FrameBuffer = ctx.buffer;
    m_Changes = &changed;
    m_DrawIndex = 0;
    m_Pins.clear();
    m_PinRects.clear();
    m_StillChanges.clear();
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
    m_PlanAreas.clear();
    m_PlanContent.clear();
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

    // What stays on screen moved with the frame, but for the pins.
    // Two lists kept from frame to frame, so nothing is allocated; a pin
    // that misses the rect leaves its pieces as they are.
    for (const Deki::Rect& r : m_StillChanges)
    {
        m_StillPieces.clear();
        m_StillPieces.push_back(r);
        for (const Deki::Rect& pin : m_PinRects)
        {
            if (pin.Empty() || pin.right <= r.left || pin.left >= r.right || pin.bottom <= r.top || pin.top >= r.bottom)
            {
                continue;
            }
            m_StillNext.clear();
            for (const Deki::Rect& piece : m_StillPieces)
            {
                SubtractRect(piece, pin, m_StillNext);
            }
            m_StillPieces.swap(m_StillNext);
            if (m_StillPieces.empty())
            {
                break;
            }
        }
        for (const Deki::Rect& piece : m_StillPieces)
        {
            changed.Add(piece);
        }
    }
    m_StillChanges.clear();

    // Renderers not met this time (destroyed, inactive) leave their pixels.
    for (auto it = m_Records.begin(); it != m_Records.end();)
    {
        if (it->second.seen != m_RecordCall)
        {
            changed.Add(it->second.area);
            if (it->second.reuseSlot != 0)
            {
                m_Reuse[it->second.reuseSlot - 1].valid = false;
                m_ReuseFree.push_back(it->second.reuseSlot);
            }
            it = m_Records.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // Where the light changed. Render() works it out again for the frame.
    m_Lighting.Build(scene, cam, viewWidth, viewHeight, frameCtx.camera->litUpToSortOrder);
    m_Lighting.AddChanges(m_LightingRecord, changed);
    if ((m_ScrollX != 0 || m_ScrollY != 0) && m_Lighting.On())
    {
        // Lights are compared on screen: with any on, a moved camera relights
        // the whole frame.
        changed.SetFull();
        m_ScrollX = 0;
        m_ScrollY = 0;
        m_PinRects.clear();
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

void Standard2DRenderer::NoteDraw(const RendererComponent* renderer, uint64_t signature, const Deki::Rect& area,
                                  const Deki::Rect& reach, uint64_t screenSignature, const Deki::Rect& pin,
                                  uint32_t drawIndex, DrawRecord* known)
{
    DrawRecord& record = known ? *known : m_Records[RecordKey{ renderer, kNoPiece }];
    const bool met = record.seen != 0;
    const Deki::Rect& old = record.area;
    auto same = [](const Deki::Rect& a, const Deki::Rect& b)
    { return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom; };
    // While the frame scrolls, a renderer drawn the same, at the same place in
    // the world, is already where it belongs in the moved frame, though the
    // view's edges cut it differently: what the move uncovered of it lies in
    // the uncovered edge, drawn anyway.
    const bool scrolling = m_ScrollX != 0 || m_ScrollY != 0;
    const bool scrolledAlong = scrolling && signature != 0 && !reach.Empty() && same(record.reach, reach);
    // Drawn the same at the same place on screen while the frame moves: its
    // pixels moved with the frame, so both places are drawn again, except the
    // pins, which keep their pixels.
    const bool stillOnScreen =
        scrolling && met && signature != 0 && screenSignature != 0 && record.screenSignature == screenSignature;
    // Drawn over a pin met earlier: unless it too is the same on screen, its
    // pixels there are last frame's, where it was then.
    if (scrolling && met && !stillOnScreen)
    {
        const Deki::Rect was{ old.left - m_ScrollX, old.top - m_ScrollY, old.right - m_ScrollX,
                              old.bottom - m_ScrollY };
        for (const Pin& p : m_Pins)
        {
            for (const Deki::Rect& r : { was, area })
            {
                const Deki::Rect cut{ std::max(r.left, p.rect.left), std::max(r.top, p.rect.top),
                                      std::min(r.right, p.rect.right), std::min(r.bottom, p.rect.bottom) };
                if (!r.Empty() && !cut.Empty())
                {
                    RenderProfiler::AddObjectRedraw(renderer, RenderProfiler::Redraw::OverPin);
                    m_Changes->Add(cut);
                }
            }
        }
    }
    if (stillOnScreen)
    {
        if (!same(old, area) || record.signature != signature)
        {
            RenderProfiler::AddObjectRedraw(renderer, RenderProfiler::Redraw::StillOnScreen);
            // Both places in one: a scroll of a few pixels leaves them
            // overlapping all but a band, and one rect less the pins is
            // fewer pieces than two.
            if (old.Empty() || area.Empty())
            {
                m_StillChanges.push_back(old.Empty() ? area : old);
            }
            else
            {
                m_StillChanges.push_back(Deki::Rect{ std::min(old.left, area.left), std::min(old.top, area.top),
                                                     std::max(old.right, area.right),
                                                     std::max(old.bottom, area.bottom) });
            }
        }
        if (!pin.Empty())
        {
            m_Pins.push_back(Pin{ pin, drawIndex });
            m_PinRects.push_back(pin);
        }
    }
    else if (!met || record.signature != signature || (!same(old, area) && !scrolledAlong))
    {
        RenderProfiler::AddObjectRedraw(renderer, met ? RenderProfiler::Redraw::Changed : RenderProfiler::Redraw::New);
        if (met)
        {
            m_Changes->Add(old);
        }
        m_Changes->Add(area);
    }
    record.signature = signature;
    record.area = area;
    record.reach = reach;
    record.seen = m_RecordCall;
    record.screenSignature = screenSignature;
}

namespace
{
// A rect of a source's pixels as drawn with its flips.
Deki::Rect Flipped(const QuadBlit::Source& source, const Deki::Rect& r)
{
    Deki::Rect out = r;
    if (source.flipH)
    {
        out.left = source.width - r.right;
        out.right = source.width - r.left;
    }
    if (source.flipV)
    {
        out.top = source.height - r.bottom;
        out.bottom = source.height - r.top;
    }
    return out;
}
}  // namespace

Standard2DRenderer::~Standard2DRenderer()
{
    Deki::Memory::Free(m_SourceFacts);  // trivially destructible entries
    Deki::Memory::Free(m_Reuse);
    Deki::Memory::Free(m_ReuseContent);
}

Standard2DRenderer::ReuseEntry* Standard2DRenderer::ReuseFor(DrawRecord& record, bool create)
{
    if (record.reuseSlot != 0)
    {
        return &m_Reuse[record.reuseSlot - 1];
    }
    if (!create)
    {
        return nullptr;
    }
    uint32_t slot = 0;
    if (!m_ReuseFree.empty())
    {
        slot = m_ReuseFree.back();
        m_ReuseFree.pop_back();
    }
    else
    {
        if (m_ReuseUsed == m_ReuseCapacity)
        {
            // Grown by doubling, entries moved as they are (plain values).
            const uint32_t capacity = m_ReuseCapacity ? m_ReuseCapacity * 2 : 64;
            auto* grown =
                static_cast<ReuseEntry*>(Deki::Memory::Allocate(sizeof(ReuseEntry) * capacity, Deki::Memory::External));
            auto* grownContent = static_cast<CachedContent*>(
                Deki::Memory::Allocate(sizeof(CachedContent) * capacity, Deki::Memory::External));
            if (!grown || !grownContent)
            {
                if (grown)
                {
                    Deki::Memory::Free(grown);
                }
                if (grownContent)
                {
                    Deki::Memory::Free(grownContent);
                }
                return nullptr;  // measured in full, as without a key
            }
            if (m_Reuse)
            {
                std::memcpy(static_cast<void*>(grown), m_Reuse, sizeof(ReuseEntry) * m_ReuseUsed);
                std::memcpy(static_cast<void*>(grownContent), m_ReuseContent, sizeof(CachedContent) * m_ReuseUsed);
                Deki::Memory::Free(m_Reuse);
                Deki::Memory::Free(m_ReuseContent);
            }
            m_Reuse = grown;
            m_ReuseContent = grownContent;
            m_ReuseCapacity = capacity;
        }
        slot = ++m_ReuseUsed;
    }
    new (&m_Reuse[slot - 1]) ReuseEntry();
    new (&m_ReuseContent[slot - 1]) CachedContent();
    record.reuseSlot = slot;
    return &m_Reuse[slot - 1];
}

void Standard2DRenderer::ForgetReuse()
{
    m_ReuseUsed = 0;
    m_ReuseFree.clear();
    m_ReuseOn = false;
}

Standard2DRenderer::SourceFacts* Standard2DRenderer::FactsOf(const QuadBlit::Source& source)
{
    if (source.revision == 0 || !source.pixels)
    {
        return nullptr;
    }
    if (!m_SourceFacts)
    {
        m_SourceFacts = static_cast<SourceFacts*>(
            Deki::Memory::Allocate(sizeof(SourceFacts) * kSourceFacts, Deki::Memory::External));
        if (!m_SourceFacts)
        {
            return nullptr;
        }
        for (size_t i = 0; i < kSourceFacts; ++i)
        {
            new (&m_SourceFacts[i]) SourceFacts();
        }
    }
    // A few places for each key: sources that share one (a sheet's frames,
    // a sprite and a frame at its first pixel) would otherwise push each
    // other out every frame, and what is kept is a walk of the pixels to
    // find again.
    uint32_t h = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(source.pixels)) * 2654435761u;
    h ^= source.revision * 0x9E3779B1u;
    h ^= h >> 15;
    const int32_t stride = source.stride > 0 ? source.stride : source.width * source.bytesPerPixel;
    SourceFacts* empty = nullptr;
    for (uint32_t probe = 0; probe < kSourceFactWays; ++probe)
    {
        SourceFacts& f = m_SourceFacts[(h + probe) % kSourceFacts];
        if (f.pixels == source.pixels && f.revision == source.revision && f.width == source.width &&
            f.height == source.height && f.stride == stride)
        {
            return &f;
        }
        if (!f.pixels && !empty)
        {
            empty = &f;
        }
    }
    SourceFacts& f = empty ? *empty : m_SourceFacts[(h + (m_SourceFactTurn++ % kSourceFactWays)) % kSourceFacts];
    f = SourceFacts{};
    f.pixels = source.pixels;
    f.revision = source.revision;
    f.width = source.width;
    f.height = source.height;
    f.stride = stride;
    return &f;
}

bool Standard2DRenderer::OpaqueInteriorOf(const QuadBlit::Source& source, Deki::Rect& rect)
{
    QuadBlit::Source plain = source;
    plain.flipH = plain.flipV = false;
    SourceFacts* f = FactsOf(source);
    if (f && (f->known & 1))
    {
        rect = Flipped(source, f->interior);
        return f->hasInterior;
    }
    int32_t left = 0, top = 0, right = 0, bottom = 0;
    const bool found = QuadBlit::OpaqueInterior(plain, left, top, right, bottom);
    const Deki::Rect r{ left, top, right, bottom };
    if (f)
    {
        f->known |= 1;
        f->hasInterior = found;
        f->interior = r;
    }
    rect = Flipped(source, r);
    return found;
}

bool Standard2DRenderer::VisibleBoundsOf(const QuadBlit::Source& source, Deki::Rect& rect)
{
    // Only pixels kept by revision: others may change in place unseen, and
    // walking them every frame would cost more than it saves.
    SourceFacts* f = FactsOf(source);
    if (!f)
    {
        return false;
    }
    if (!(f->known & 2))
    {
        QuadBlit::Source plain = source;
        plain.flipH = plain.flipV = false;
        int32_t left = 0, top = 0, right = 0, bottom = 0;
        f->hasVisible = QuadBlit::VisibleBounds(plain, left, top, right, bottom);
        f->visible = Deki::Rect{ left, top, right, bottom };
        f->known |= 2;
    }
    rect = f->visible.Empty() ? Deki::Rect{} : Flipped(source, f->visible);
    return f->hasVisible;
}

bool Standard2DRenderer::VisiblePartOf(const QuadBlit::Source& source, QuadBlit::Source& part, int32_t& x, int32_t& y)
{
    // Rows that already say what to skip (spans, runs, one colour a row) are
    // left whole: their tables are laid out for the whole source.
    if (!source.hasAlpha || source.alphaRowSpans || source.chromaRowSpans || source.clearRowSpans ||
        source.uniformRuns || source.rowColors || source.uniformRows || source.uniformColumns)
    {
        return false;
    }
    SourceFacts* f = FactsOf(source);
    Deki::Rect drawn;
    if (!f || !VisibleBoundsOf(source, drawn))
    {
        return false;
    }
    if (drawn.Empty())
    {
        part = source;
        part.width = 0;
        part.height = 0;
        x = y = 0;
        return true;
    }
    if (drawn.left == 0 && drawn.top == 0 && drawn.right == source.width && drawn.bottom == source.height)
    {
        return false;  // nothing to leave out
    }
    // The box in the pixels as stored; drawn, a flip mirrors it inside the
    // source, and mirrors the part the same way.
    const Deki::Rect& stored = f->visible;
    const int32_t stride = source.stride > 0 ? source.stride : source.width * source.bytesPerPixel;
    part = source;
    part.pixels = source.pixels + static_cast<size_t>(stored.top) * static_cast<size_t>(stride) +
                  static_cast<size_t>(stored.left) * static_cast<size_t>(source.bytesPerPixel);
    part.width = stored.right - stored.left;
    part.height = stored.bottom - stored.top;
    part.stride = stride;
    part.ownsPixels = false;
    x = drawn.left;
    y = drawn.top;
    return true;
}

void Standard2DRenderer::LightFrameIfDue(const SortItem* next, const RenderContext& ctx)
{
    if (m_Lit || !m_Lighting.On())
    {
        return;
    }
    if (next && next->order <= m_Lighting.LitUpToSortOrder() && !(next->renderer && next->renderer->LightsItself()))
    {
        return;
    }
    m_Lit = true;
    // Nothing has written the frame yet, and it is lit the same all over:
    // lit, the clear is one colour, so it is made in that colour instead of
    // lit after, still when the frame is first written.
    const DeferredClear* clear = QuadBlit::PendingClear();
    if (clear && clear->runLit && !m_SinkFrame && m_Lighting.Uniform())
    {
        m_LitFrom = clear;
        m_LitFormat = ctx.format;
        m_LitViewLeft = ctx.viewLeft;
        m_LitViewTop = ctx.viewTop;
        m_LitClear.run = &Standard2DRenderer::RunLitClear;
        m_LitClear.owner = this;
        m_LitClear.pixel = clear->pixel ? &Standard2DRenderer::LitClearPixel : nullptr;
        m_LitClear.colour = clear->colour ? &Standard2DRenderer::LitClearColour : nullptr;
        QuadBlit::SetPendingClear(m_FrameBuffer, &m_LitClear);
        return;
    }
    RunClear();  // the clear colour is lit too: it shows wherever nothing covers it
    if (m_SinkFrame)
    {
        // The frame is on the GPU: the CPU's buffer holds none of it.
        static bool s_Told = false;
        if (!QuadBlit::LightSinkFrame(m_Lighting) && !s_Told)
        {
            s_Told = true;
            DEKI_LOG_ERROR("Lights: the GPU drawing this frame cannot light it, so it is drawn unlit");
        }
        return;
    }
    m_Lighting.Apply(ctx.buffer, ctx.width, ctx.height, ctx.format, ctx.redraw, ctx.viewLeft, ctx.viewTop);
}

void Standard2DRenderer::RunLitClear(void* owner)
{
    Standard2DRenderer* self = static_cast<Standard2DRenderer*>(owner);
    self->m_LitFrom->runLit(self->m_LitFrom->owner, self->m_Lighting, self->m_LitViewLeft, self->m_LitViewTop);
}

bool Standard2DRenderer::LitClearColour(void* owner, uint8_t* out)
{
    // As LitClearPixel, whatever the clear covers.
    Standard2DRenderer* self = static_cast<Standard2DRenderer*>(owner);
    if (!self->m_LitFrom->colour(self->m_LitFrom->owner, out))
    {
        return false;
    }
    self->m_Lighting.Apply(out, 1, 1, self->m_LitFormat, Deki::Rect{ 0, 0, 1, 1 }, self->m_LitViewLeft,
                           self->m_LitViewTop);
    return true;
}

bool Standard2DRenderer::LitClearPixel(void* owner, uint8_t* out)
{
    // The clear's pixel, lit by Apply itself, as DekiRenderSystem's lit clear.
    Standard2DRenderer* self = static_cast<Standard2DRenderer*>(owner);
    if (!self->m_LitFrom->pixel(self->m_LitFrom->owner, out))
    {
        return false;
    }
    self->m_Lighting.Apply(out, 1, 1, self->m_LitFormat, Deki::Rect{ 0, 0, 1, 1 }, self->m_LitViewLeft,
                           self->m_LitViewTop);
    return true;
}

void Standard2DRenderer::ForgetRecords()
{
    m_Records.clear();
    ForgetReuse();
    m_HaveRecords = false;
    m_ScrollX = 0;
    m_ScrollY = 0;
    m_ScrollOriginX = 0;
    m_ScrollOriginY = 0;
    m_Pins.clear();
    m_PinRects.clear();
}

bool Standard2DRenderer::ScrolledBy(int32_t& dx, int32_t& dy) const
{
    dx = m_ScrollX;
    dy = m_ScrollY;
    return m_ScrollX != 0 || m_ScrollY != 0;
}

Deki::Rect Standard2DRenderer::PlanAreaOf(const PlanEntry& entry) const
{
    // The pixels of the view the object can draw in, or everywhere when that
    // takes more than its own renderer's area to know: a clip, children, a
    // render pass that wants it, an area not yet found.
    const bool plain = m_PrePasses.empty() && m_PostPasses.empty() && m_ExecPasses.size() <= 32;
    const uint32_t passBits = m_ExecPasses.size() >= 32 ? 0xFFFFFFFFu : ((1u << m_ExecPasses.size()) - 1u);
    if (plain && entry.childCount == 0 && !entry.item.clip && (entry.execPasses & passBits) == 0)
    {
        const BuiltinGeometry& g = entry.geo;
        if (!entry.item.renderer)
        {
            return Deki::Rect{};
        }
        if (g.areaKnown)
        {
            return Deki::Rect{ g.areaLeft, g.areaTop, g.areaRight, g.areaBottom };
        }
    }
    return Deki::Rect{ INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX };
}

void Standard2DRenderer::FindPlanAreas()
{
    m_PlanAreas.resize(m_Plan.size());
    for (size_t i = 0; i < m_Plan.size(); i++)
    {
        m_PlanAreas[i] = PlanAreaOf(m_Plan[i]);
    }
}

bool Standard2DRenderer::OutsidePart(const Deki::Rect& area, const RenderContext& ctx)
{
    int32_t left = ctx.viewLeft, top = ctx.viewTop;
    int32_t right = ctx.viewLeft + ctx.width, bottom = ctx.viewTop + ctx.height;
    if (!ctx.redraw.Empty())
    {
        left = ctx.viewLeft + ctx.redraw.left;
        top = ctx.viewTop + ctx.redraw.top;
        right = ctx.viewLeft + ctx.redraw.right;
        bottom = ctx.viewTop + ctx.redraw.bottom;
    }
    return area.bottom <= top || area.top >= bottom || area.right <= left || area.left >= right;
}

void Standard2DRenderer::RenderObject(const SortItem& item, const RenderContext& ctx, int32_t planIndex)
{
    // Replaying a part of the frame (or a strip), an object whose only
    // drawing is its own renderer, outside that part, draws nothing: it is
    // passed over before anything is set up for it. On a board the plan is in
    // external memory, and a frame redrawn in parts replays it once a part.
    if (m_PlanMode == PlanMode::Replaying && m_PrePasses.empty() && m_PostPasses.empty() && m_ExecPasses.size() <= 32)
    {
        const PlanEntry& entry = m_Plan[planIndex];
        const uint32_t passBits = m_ExecPasses.size() == 32 ? 0xFFFFFFFFu : ((1u << m_ExecPasses.size()) - 1u);
        if (entry.childCount == 0 && !item.clip && (entry.execPasses & passBits) == 0)
        {
            if (!item.renderer)
            {
                return;
            }
            const BuiltinGeometry& g = entry.geo;
            if (g.areaKnown)
            {
                int32_t left = ctx.viewLeft, top = ctx.viewTop;
                int32_t right = ctx.viewLeft + ctx.width, bottom = ctx.viewTop + ctx.height;
                if (!ctx.redraw.Empty())
                {
                    left = ctx.viewLeft + ctx.redraw.left;
                    top = ctx.viewTop + ctx.redraw.top;
                    right = ctx.viewLeft + ctx.redraw.right;
                    bottom = ctx.viewTop + ctx.redraw.bottom;
                }
                if (g.areaBottom <= top || g.areaTop >= bottom || g.areaRight <= left || g.areaLeft >= right)
                {
                    return;
                }
            }
        }
    }

    Deki::Object* obj = item.obj;
    RenderContext objCtx = ctx;

    // Phase 1: PreExecute passes, which may redirect ctx.buffer for this object.
    RunClearFor(m_PrePasses);
    for (RenderPass* pass : m_PrePasses)
    {
        pass->PreExecute(obj, objCtx);
        AfterHook(pass);
    }

    // Phase 2: built-ins (sprite blit), into any target PreExecute redirected
    // to. Recording, the entry's geometry is filled in; replaying, reused.
    // (Taken by index here: m_Plan only grows below, after this call.)
    if (m_PlanMode == PlanMode::Measuring && item.renderer && RenderProfiler::IsEnabled())
    {
        const uint64_t start = RenderProfiler::Now();
        ExecuteBuiltins(item, objCtx, planIndex >= 0 ? &m_Plan[planIndex].geo : nullptr);
        RenderProfiler::AddObjectMeasure(item.renderer, RenderProfiler::Now() - start);
    }
    else
    {
        ExecuteBuiltins(item, objCtx, planIndex >= 0 ? &m_Plan[planIndex].geo : nullptr);
    }

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
                    // Drawn after a pin, it may draw over it without saying.
                    for (const Pin& p : m_Pins)
                    {
                        m_Changes->Add(p.rect);
                    }
                }
                else
                {
                    m_Changes->SetFull();
                }
            }
            continue;
        }
        if (QuadBlit::PendingClear() && pass->WritesBuffer() && pass->WantsObject(obj))
        {
            RunClear();
        }
        if (RenderProfiler::IsEnabled())
        {
            const uint64_t start = RenderProfiler::Now();
            pass->Execute(obj, objCtx);
            const uint64_t us = RenderProfiler::Now() - start;
            RenderProfiler::AddContent(RenderProfiler::kPasses, us, "Render passes");
            RenderProfiler::AddObjectPass(pass, pass->Name(), obj, us);
        }
        else
        {
            pass->Execute(obj, objCtx);
        }
        AfterHook(pass);
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
            if (OutsidePart(m_PlanAreas[first + i], objCtx))
            {
                continue;
            }
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
    RunClearFor(m_PostPasses);
    for (auto it = m_PostPasses.rbegin(); it != m_PostPasses.rend(); ++it)
    {
        (*it)->PostExecute(obj, objCtx);
        AfterHook(*it);
    }

    // Phase 6: built-ins again (clip pop).
    PostExecuteBuiltins(item);

    // The plan's short list (m_PlanAreas), while the entry is at hand.
    if (BuildingPlan() && planIndex >= 0)
    {
        if (m_PlanAreas.size() < m_Plan.size())
        {
            m_PlanAreas.resize(m_Plan.size(), Deki::Rect{ INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX });
        }
        m_PlanAreas[static_cast<size_t>(planIndex)] = PlanAreaOf(m_Plan[static_cast<size_t>(planIndex)]);
    }
}

}  // namespace DekiRendering
