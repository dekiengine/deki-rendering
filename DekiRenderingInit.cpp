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

static constexpr int MAX_INIT_PASSES = 8;
static RenderPass* s_Passes[MAX_INIT_PASSES] = {};
static int s_PassCount = 0;

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
    Standard2DRenderer* passReceiver = nullptr;
    if (s_Renderer && s_Renderer->GetRendererType() == Standard2DRenderer::RendererTypeID)
        passReceiver = static_cast<Standard2DRenderer*>(s_Renderer);

    std::vector<std::string> attachedNames;
    attachedNames.reserve(static_cast<size_t>(passCount));
    for (int i = 0; i < passCount && passReceiver && s_PassCount < MAX_INIT_PASSES; i++)
    {
        const char* passName = ProjectSettings::GetPassName(i);
        const RenderPassInfo* info = DekiRenderPassRegistry::Get(passName);
        if (info && info->factory)
        {
            s_Passes[s_PassCount] = info->factory();
            passReceiver->AddPass(s_Passes[s_PassCount]);
            DEKI_LOG_INTERNAL("DekiRendering: Added pass '%s'", passName);
            s_PassCount++;
            if (passName) attachedNames.emplace_back(passName);
        }
        else
        {
            DEKI_LOG_WARNING("DekiRendering: No pass registered for '%s'", passName ? passName : "(null)");
        }
    }

    // 3b. Auto-attach passes flagged autoAttach=true that the project's
    //     .rpipeline didn't already list. This lets module-owned passes
    //     (e.g. tilemap) participate without forcing every project to know
    //     module pass names. Projects can still mention an autoAttach pass
    //     explicitly in .rpipeline to control its ordering.
    if (passReceiver)
    {
        std::vector<std::string> allPassNames;
        DekiRenderPassRegistry::GetAllNames(allPassNames);
        for (const auto& name : allPassNames)
        {
            if (s_PassCount >= MAX_INIT_PASSES) break;
            if (std::find(attachedNames.begin(), attachedNames.end(), name) != attachedNames.end())
                continue;
            const RenderPassInfo* info = DekiRenderPassRegistry::Get(name.c_str());
            if (!info || !info->autoAttach || !info->factory) continue;
            s_Passes[s_PassCount] = info->factory();
            passReceiver->AddPass(s_Passes[s_PassCount]);
            DEKI_LOG_INTERNAL("DekiRendering: Auto-attached pass '%s'", name.c_str());
            s_PassCount++;
        }
    }

    // 4. Add all registered sorting callbacks (always-on, not tied to passes)
    if (passReceiver)
    {
        std::vector<SortingCallback> sortingCallbacks;
        DekiSortingCallbackRegistry::GetAll(sortingCallbacks);
        for (auto cb : sortingCallbacks)
            passReceiver->AddSortingCallback(cb);
        if (!sortingCallbacks.empty())
            DEKI_LOG_INTERNAL("DekiRendering: Added %d sorting callbacks", (int)sortingCallbacks.size());
    }

    // 5. Register with engine
    DekiEngine::GetInstance().SetRenderSystem(s_RenderSystem);
    DEKI_LOG_INTERNAL("DekiRendering: Init complete (renderer=%p, %d passes)", (void*)s_Renderer, s_PassCount);
}

void DekiRendering_ShutdownSystem()
{
    DekiEngine::GetInstance().SetRenderSystem(nullptr);

    for (int i = 0; i < s_PassCount; i++)
    {
        delete s_Passes[i];
        s_Passes[i] = nullptr;
    }
    s_PassCount = 0;

    delete s_Renderer;
    s_Renderer = nullptr;

    delete s_RenderSystem;
    s_RenderSystem = nullptr;
}
