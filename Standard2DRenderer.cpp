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
        int32_t screenX, screenY;
        ctx.camera->WorldToScreen(obj->GetWorldX(), obj->GetWorldY(),
                                   ctx.width, ctx.height, screenX, screenY);

        float zoom = static_cast<float>(ctx.camera->GetZoom());
        float scaledW = clip->GetClipWidth() * zoom * obj->GetWorldScaleX();
        float scaledH = clip->GetClipHeight() * zoom * obj->GetWorldScaleY();
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
        QuadBlit::Source source;
        float pivotX, pivotY;
        uint8_t tintR, tintG, tintB, tintA;
        if (renderer->RenderContent(obj, source, pivotX, pivotY,
                                     tintR, tintG, tintB, tintA))
        {
            int32_t screenX, screenY;
            ctx.camera->WorldToScreen(obj->GetWorldX(), obj->GetWorldY(),
                                       ctx.width, ctx.height, screenX, screenY);

            // Temporarily disable clipping if renderer has ignore_clip set
            bool wasClipEnabled = QuadBlit::IsClipEnabled();
            if (renderer->ignore_clip)
                QuadBlit::SetClipEnabled(false);

            QuadBlit::Blit(
                source,
                ctx.buffer,
                ctx.width,
                ctx.height,
                ctx.format,
                screenX,
                screenY,
                obj->GetWorldScaleX(),
                obj->GetWorldScaleY(),
                obj->GetWorldRotation(),
                pivotX,
                pivotY,
                tintR,
                tintG,
                tintB,
                tintA
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
        RenderObject(sortableItems[i].first, ctx);
}

void Standard2DRenderer::RenderObject(DekiObject* obj, const RenderContext& ctx)
{
    if (!obj || !obj->IsActiveInHierarchy())
        return;

    RenderContext objCtx = ctx;

    // Phase 1: Execute built-in handling (sprite blit)
    ExecuteBuiltins(obj, objCtx);

    // Phase 2: Execute custom passes (clip push, etc.)
    for (int p = 0; p < m_PassCount; p++)
        m_Passes[p]->Execute(obj, objCtx);

    // Phase 3: Recurse into sorted children
    std::pair<DekiObject*, int> childItems[64];
    int childCount = 0;
    for (auto* child : obj->GetChildren())
        CollectSortableItems(child, childItems, childCount, 64);

    std::stable_sort(childItems, childItems + childCount,
        [](const auto& a, const auto& b) { return a.second < b.second; });

    for (int i = 0; i < childCount; i++)
        RenderObject(childItems[i].first, ctx);

    // Phase 4: Post-execute custom passes (clip pop, reverse order)
    for (int p = m_PassCount - 1; p >= 0; p--)
        m_Passes[p]->PostExecute(obj, objCtx);

    // Phase 5: Post-execute built-ins
    PostExecuteBuiltins(obj, objCtx);
}
