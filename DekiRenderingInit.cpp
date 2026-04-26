#include "DekiRenderingInit.h"
#include "DekiRenderSystem.h"
#include "DekiRendererRegistry.h"
#include "DekiRenderPassRegistry.h"
#include "DekiSortingCallbackRegistry.h"
#include "Standard2DRenderer.h"
#include "DekiEngine.h"
#include "ProjectSettings.h"
#include "DekiLogSystem.h"

#include <algorithm>
#include <string>
#include <vector>

static DekiRenderSystem* s_RenderSystem = nullptr;
static DekiRenderer* s_Renderer = nullptr;
static Standard2DRenderer* s_PassReceiver = nullptr;

static constexpr int MAX_INIT_PASSES = 8;
static RenderPass* s_Passes[MAX_INIT_PASSES] = {};
static std::string s_PassNames[MAX_INIT_PASSES];
static int s_PassCount = 0;

static void AttachPass(const char* name, const RenderPassInfo& info)
{
    if (!s_PassReceiver || !info.factory) return;
    if (s_PassCount >= MAX_INIT_PASSES) return;
    for (int i = 0; i < s_PassCount; ++i)
        if (s_PassNames[i] == name) return;  // already attached

    s_Passes[s_PassCount]    = info.factory();
    s_PassNames[s_PassCount] = name;
    s_PassReceiver->AddPass(s_Passes[s_PassCount]);
    DEKI_LOG_INTERNAL("DekiRendering: Attached pass '%s'", name);
    s_PassCount++;
}

void DekiRendering_InitSystem()
{
    if (s_RenderSystem)
        return;

    // 1. Create render system (framebuffer + camera management)
    s_RenderSystem = new DekiRenderSystem();

    // 2. Create renderer from project settings
    const char* rendererName = ProjectSettings::GetRenderPipeline();
    s_Renderer = DekiRendererRegistry::Create(rendererName);
    if (s_Renderer)
    {
        s_RenderSystem->SetRenderer(s_Renderer);
        DEKI_LOG_INTERNAL("DekiRendering: Created renderer '%s' (%p)", rendererName, (void*)s_Renderer);
    }
    else
    {
        DEKI_LOG_WARNING("DekiRendering: No renderer registered for '%s'", rendererName ? rendererName : "(null)");
    }

    // 3. Create and add passes from project settings
    //    Safe downcast via GetRendererType() — no RTTI needed.
    int passCount = ProjectSettings::GetPassCount();
    if (s_Renderer && s_Renderer->GetRendererType() == Standard2DRenderer::RendererTypeID)
        s_PassReceiver = static_cast<Standard2DRenderer*>(s_Renderer);

    for (int i = 0; i < passCount; i++)
    {
        const char* passName = ProjectSettings::GetPassName(i);
        const RenderPassInfo* info = DekiRenderPassRegistry::Get(passName);
        if (info && info->factory)
            AttachPass(passName, *info);
        else
            DEKI_LOG_WARNING("DekiRendering: No pass registered for '%s'", passName ? passName : "(null)");
    }

    // 3b. Auto-attach passes flagged autoAttach=true that the project's
    //     .rpipeline didn't already list. This lets module-owned passes
    //     (e.g. tilemap) participate without forcing every project to know
    //     module pass names. Projects can still mention an autoAttach pass
    //     explicitly in .rpipeline to control its ordering.
    if (s_PassReceiver)
    {
        std::vector<std::string> allPassNames;
        DekiRenderPassRegistry::GetAllNames(allPassNames);
        for (const auto& name : allPassNames)
        {
            const RenderPassInfo* info = DekiRenderPassRegistry::Get(name.c_str());
            if (!info || !info->autoAttach) continue;
            AttachPass(name.c_str(), *info);
        }
    }

    // 3c. Install a hook so passes registered after this point (e.g. modules
    //     that load after deki-rendering inits) still get auto-attached. This
    //     is the path deki-tilemap takes — its DLL loads after the rendering
    //     system has already finished its first scan.
    DekiRenderPassRegistry::SetAutoAttachCallback(
        [](const char* name, const RenderPassInfo& info) { AttachPass(name, info); });

    // 4. Add all registered sorting callbacks (always-on, not tied to passes)
    if (s_PassReceiver)
    {
        std::vector<SortingCallback> sortingCallbacks;
        DekiSortingCallbackRegistry::GetAll(sortingCallbacks);
        for (auto cb : sortingCallbacks)
            s_PassReceiver->AddSortingCallback(cb);
        if (!sortingCallbacks.empty())
            DEKI_LOG_INTERNAL("DekiRendering: Added %d sorting callbacks", (int)sortingCallbacks.size());
    }

    // 5. Register with engine
    DekiEngine::GetInstance().SetRenderSystem(s_RenderSystem);
    DEKI_LOG_INTERNAL("DekiRendering: Init complete (renderer=%p, %d passes)", (void*)s_Renderer, s_PassCount);
}

void DekiRendering_DetachPass(const char* name)
{
    if (!name) return;
    for (int i = 0; i < s_PassCount; ++i)
    {
        if (s_PassNames[i] != name) continue;

        if (s_PassReceiver)
            s_PassReceiver->RemovePass(s_Passes[i]);
        delete s_Passes[i];

        // Compact arrays so indices stay contiguous.
        for (int j = i; j < s_PassCount - 1; ++j)
        {
            s_Passes[j]    = s_Passes[j + 1];
            s_PassNames[j] = std::move(s_PassNames[j + 1]);
        }
        s_PassCount--;
        s_Passes[s_PassCount] = nullptr;
        s_PassNames[s_PassCount].clear();
        DEKI_LOG_INTERNAL("DekiRendering: Detached pass '%s'", name);
        return;
    }
}

void DekiRendering_ShutdownSystem()
{
    DekiEngine::GetInstance().SetRenderSystem(nullptr);

    // Drop the late-attach hook so a stale lambda doesn't reference a freed
    // renderer if a module re-registers after shutdown.
    DekiRenderPassRegistry::SetAutoAttachCallback(nullptr);

    for (int i = 0; i < s_PassCount; i++)
    {
        delete s_Passes[i];
        s_Passes[i] = nullptr;
        s_PassNames[i].clear();
    }
    s_PassCount = 0;
    s_PassReceiver = nullptr;

    delete s_Renderer;
    s_Renderer = nullptr;

    delete s_RenderSystem;
    s_RenderSystem = nullptr;
}
