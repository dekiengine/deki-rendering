#include "Standard2DRenderer.h"
#include "DekiRendererRegistry.h"
#include "IClipProvider.h"
#include "ISortableProvider.h"
#include "DekiEngine.h"
#include "PrefabSystem.h"
#include "CameraComponent.h"
#include "RendererComponent.h"
#include "QuadBlit.h"
#include "DekiObject.h"
#include "Prefab.h"
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
        const bool useOrderedDither = (renderer->alpha_mode == AlphaMode::OrderedDither);

        QuadBlit::Source source;
        float pivotX, pivotY;
        uint8_t tintR, tintG, tintB, tintA;
        if (renderer->RenderContent(obj, source, pivotX, pivotY,
                                     tintR, tintG, tintB, tintA))
        {
            float fScreenX, fScreenY;
            ctx.camera->WorldToScreen(obj->GetWorldX(), obj->GetWorldY(),
                                       ctx.width, ctx.height, fScreenX, fScreenY);

            // Temporarily disable clipping if renderer has ignore_clip set
            bool wasClipEnabled = QuadBlit::IsClipEnabled();
            if (renderer->ignore_clip)
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

            // pixel_snap = true → round to nearest pixel (sharp, sprite-art).
            // pixel_snap = false → truncate (sub-pixel motion accumulates;
            // visually smoother under continuous movement, no bilinear yet).
            const int32_t intScreenX = renderer->pixel_snap
                ? static_cast<int32_t>(std::lround(fScreenX))
                : static_cast<int32_t>(fScreenX);
            const int32_t intScreenY = renderer->pixel_snap
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
            if (renderer->ignore_clip)
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

void Standard2DRenderer::CollectSortableItems(DekiObject* obj,
    std::pair<DekiObject*, int>* items, int& count, int maxItems)
{
    if (!obj || count >= maxItems) return;

    // Check built-in components first
    int32_t order;
    if (GetBuiltinSortingOrder(obj, order))
    {
        items[count++] = {obj, order};
        return;
    }

    // Then check custom sorting callbacks
    for (int s = 0; s < m_SortingCallbackCount; s++)
    {
        if (m_SortingCallbacks[s](obj, order))
        {
            items[count++] = {obj, order};
            return;
        }
    }

    // No one claimed it — transparent container, children float up
    for (auto* child : obj->GetChildren())
        CollectSortableItems(child, items, count, maxItems);
}

// --- Main render loop ---

void Standard2DRenderer::Render(Prefab* prefab, const RenderContext& ctx)
{
    if (!prefab || !ctx.camera || !ctx.buffer)
        return;

    QuadBlit::ClearClipStack();

    // Frame-scoped mutable context. Passes can swap frameCtx.buffer in
    // BeginFrame to install a default render target for the whole frame;
    // every RenderObject below uses frameCtx, not the original ctx.
    RenderContext frameCtx = ctx;
    for (int p = 0; p < m_PassCount; p++)
        m_Passes[p]->BeginFrame(frameCtx);

    // Collect and sort root objects
    std::pair<DekiObject*, int> sortableItems[64];
    int sortableCount = 0;

    for (DekiObject* obj : prefab->GetObjects())
        CollectSortableItems(obj, sortableItems, sortableCount, 64);

    // Also collect persistent objects
    const auto& persistentObjects = DekiEngine::GetInstance().GetPrefabSystem().GetPersistentObjects();
    for (DekiObject* obj : persistentObjects)
        CollectSortableItems(obj, sortableItems, sortableCount, 64);

    // Sort by sortingOrder (lower = behind)
    std::stable_sort(sortableItems, sortableItems + sortableCount,
        [](const auto& a, const auto& b) { return a.second < b.second; });

    // Render in sorted order
    for (int i = 0; i < sortableCount; i++)
        RenderObject(sortableItems[i].first, frameCtx);

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
    std::pair<DekiObject*, int> childItems[64];
    int childCount = 0;
    for (auto* child : obj->GetChildren())
        CollectSortableItems(child, childItems, childCount, 64);

    std::stable_sort(childItems, childItems + childCount,
        [](const auto& a, const auto& b) { return a.second < b.second; });

    for (int i = 0; i < childCount; i++)
        RenderObject(childItems[i].first, objCtx);

    // Phase 5: Post-execute custom passes (clip pop, reverse order)
    for (int p = m_PassCount - 1; p >= 0; p--)
        m_Passes[p]->PostExecute(obj, objCtx);

    // Phase 6: Post-execute built-ins
    PostExecuteBuiltins(obj, objCtx);
}
