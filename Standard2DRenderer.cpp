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
    if (m_PassCount < MAX_PASSES)
        m_Passes[m_PassCount++] = pass;
}

void Standard2DRenderer::RemovePass(RenderPass* pass)
{
    for (int i = 0; i < m_PassCount; i++)
    {
        if (m_Passes[i] == pass)
        {
            // Shift remaining passes down
            for (int j = i; j < m_PassCount - 1; j++)
                m_Passes[j] = m_Passes[j + 1];
            m_Passes[--m_PassCount] = nullptr;
            return;
        }
    }
}

void Standard2DRenderer::AddSortingCallback(SortingCallback cb)
{
    if (m_SortingCallbackCount < MAX_SORTING_CALLBACKS)
        m_SortingCallbacks[m_SortingCallbackCount++] = cb;
}

void Standard2DRenderer::RemoveSortingCallback(SortingCallback cb)
{
    for (int i = 0; i < m_SortingCallbackCount; i++)
    {
        if (m_SortingCallbacks[i] == cb)
        {
            for (int j = i; j < m_SortingCallbackCount - 1; j++)
                m_SortingCallbacks[j] = m_SortingCallbacks[j + 1];
            m_SortingCallbacks[--m_SortingCallbackCount] = nullptr;
            return;
        }
    }
}

// --- Built-in component handling ---

bool Standard2DRenderer::GetBuiltinSortingOrder(DekiObject* obj, int32_t& outOrder)
{
    // Check RendererComponent
    auto* renderer = obj->GetComponent<RendererComponent>();
    if (renderer)
    {
        outOrder = renderer->GetSortingOrder();
        return true;
    }

    // Check ISortableProvider (ClipComponent, SortingGroupComponent, etc.)
    auto* sortable = obj->FindInterface<ISortableProvider>();
    if (sortable)
    {
        outOrder = sortable->GetSortingOrder();
        return true;
    }

    return false;
}

void Standard2DRenderer::ExecuteBuiltins(DekiObject* obj, RenderContext& ctx)
{
    // Clip: push clip rect if IClipProvider is present
    auto* clip = obj->FindInterface<IClipProvider>();
    if (clip)
    {
        float fScreenX, fScreenY;
        ctx.camera->WorldToScreen(obj->GetWorldX(), obj->GetWorldY(),
                                   ctx.width, ctx.height, fScreenX, fScreenY);
        int32_t screenX = static_cast<int32_t>(std::floor(fScreenX));
        int32_t screenY = static_cast<int32_t>(std::floor(fScreenY));

        const float effective = ctx.camera->GetPixelsPerMeter();
        float scaledW = clip->GetClipWidth() * effective * obj->GetWorldScaleX();
        float scaledH = clip->GetClipHeight() * effective * obj->GetWorldScaleY();
        int32_t left = screenX - static_cast<int32_t>(std::floor(scaledW * 0.5f));
        int32_t top  = screenY - static_cast<int32_t>(std::floor(scaledH * 0.5f));

        QuadBlit::PushClipRect(left, top,
                               left + static_cast<int32_t>(scaledW),
                               top  + static_cast<int32_t>(scaledH));
    }

    // Sprite: blit content
    auto* renderer = obj->GetComponent<RendererComponent>();
    if (renderer)
    {
        const bool useOrderedDither = (renderer->alphaMode == AlphaMode::OrderedDither);

        // Cull before RenderContent, which may rasterise text, bake a gradient
        // or copy a frame: a conservative screen box from the component's
        // world extents - any pivot (the content lies within one full size of
        // the origin) and any rotation (within width + height). Objects of
        // unknown size are drawn.
        float extentW = 0.0f, extentH = 0.0f;
        if (renderer->GetContentExtents(extentW, extentH))
        {
            float cx, cy;
            ctx.camera->WorldToScreen(obj->GetWorldX(), obj->GetWorldY(), ctx.width, ctx.height, cx, cy);
            const float reach = (std::fabs(extentW * obj->GetWorldScaleX()) +
                                 std::fabs(extentH * obj->GetWorldScaleY())) *
                                    ctx.camera->GetPixelsPerMeter() +
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
            float fScreenX, fScreenY;
            ctx.camera->WorldToScreen(obj->GetWorldX(), obj->GetWorldY(),
                                       ctx.width, ctx.height, fScreenX, fScreenY);

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
            const float worldToScreen = ctx.camera->GetPixelsPerMeter();
            const float spritePPM = (source.pixelsPerMeter > 0.0f) ? source.pixelsPerMeter : 1.0f;
            const float invSourcePPM = 1.0f / spritePPM;
            const float drawScaleX = obj->GetWorldScaleX() * worldToScreen * invSourcePPM;
            const float drawScaleY = obj->GetWorldScaleY() * worldToScreen * invSourcePPM;

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
                obj->GetWorldRotation(),
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

void Standard2DRenderer::PostExecuteBuiltins(DekiObject* obj, RenderContext& ctx)
{
    // Pop clip rect if IClipProvider is present
    if (obj->FindInterface<IClipProvider>())
        QuadBlit::PopClipRect();
}

// --- Sortable item collection ---

std::vector<Standard2DRenderer::SortItem>& Standard2DRenderer::SortListForDepth(int depth)
{
    if (static_cast<size_t>(depth) >= m_SortScratch.size())
        m_SortScratch.resize(depth + 1);
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
    // RenderObject skips inactive objects; not collecting them keeps the
    // sort (and the draw order) about what is actually visible.
    if (!obj || !obj->IsActive()) return;

    // Check built-in components first
    int32_t order;
    if (GetBuiltinSortingOrder(obj, order))
    {
        items.push_back({obj, order, static_cast<uint32_t>(items.size())});
        return;
    }

    // Then check custom sorting callbacks
    for (int s = 0; s < m_SortingCallbackCount; s++)
    {
        if (m_SortingCallbacks[s](obj, order))
        {
            items.push_back({obj, order, static_cast<uint32_t>(items.size())});
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

    // Frame-scoped mutable context. Passes can swap frameCtx.buffer in
    // BeginFrame to install a default render target for the whole frame;
    // every RenderObject below uses frameCtx, not the original ctx.
    RenderContext frameCtx = ctx;
    for (int p = 0; p < m_PassCount; p++)
        m_Passes[p]->BeginFrame(frameCtx);

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
        RenderObject(sortableItems[i].obj, frameCtx);

    // Post-frame composites (e.g. screen-space overlays).
    for (int p = m_PassCount - 1; p >= 0; p--)
        m_Passes[p]->EndFrame(frameCtx);
}

void Standard2DRenderer::RenderObject(DekiObject* obj, const RenderContext& ctx)
{
    if (!obj || !obj->IsActiveInHierarchy())
        return;

    RenderContext objCtx = ctx;

    // Phase 1: Pre-execute custom passes (may redirect ctx.buffer for this object)
    for (int p = 0; p < m_PassCount; p++)
        m_Passes[p]->PreExecute(obj, objCtx);

    // Phase 2: Execute built-in handling (sprite blit) — uses any ctx redirect from PreExecute
    ExecuteBuiltins(obj, objCtx);

    // Phase 3: Execute custom passes (clip push, etc.)
    for (int p = 0; p < m_PassCount; p++)
        m_Passes[p]->Execute(obj, objCtx);

    // Phase 4: Recurse into sorted children — uses objCtx so children inherit any
    // buffer redirect applied by this object's PreExecute / Execute hooks.
    ++m_SortDepth;
    std::vector<SortItem>& childItems = SortListForDepth(m_SortDepth);
    for (auto* child : obj->GetChildren())
        CollectSortableItems(child, childItems);

    SortItems(childItems);

    for (size_t i = 0; i < childItems.size(); i++)
        RenderObject(childItems[i].obj, objCtx);
    --m_SortDepth;

    // Phase 5: Post-execute custom passes (clip pop, reverse order)
    for (int p = m_PassCount - 1; p >= 0; p--)
        m_Passes[p]->PostExecute(obj, objCtx);

    // Phase 6: Post-execute built-ins
    PostExecuteBuiltins(obj, objCtx);
}
