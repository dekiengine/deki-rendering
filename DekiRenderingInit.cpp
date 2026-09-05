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

// Passes this system created, in attach order. No cap: a fixed array of 8
// used to silently drop the ninth pass.
struct AttachedPass
{
    std::string name;
    RenderPass* pass;
};
static std::vector<AttachedPass> s_Passes;

static void AttachPass(const char* name, const RenderPassInfo& info)
{
    if (!s_PassReceiver || !info.factory) return;
    for (const AttachedPass& p : s_Passes)
        if (p.name == name) return;  // already attached

    RenderPass* pass = info.factory();
    s_Passes.push_back({ name, pass });
    s_PassReceiver->AddPass(pass);
    DEKI_LOG_INTERNAL("DekiRendering: Attached pass '%s'", name);
}

void DekiRendering_InitSystem()
{
    if (s_RenderSystem)
        return;

    // 1. Create render system (framebuffer + camera management)
    s_RenderSystem = new DekiRenderSystem();

    // 2. Create renderer from project settings
    const char* rendererName = Deki::ProjectSettings::GetRenderPipeline();
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
    int passCount = Deki::ProjectSettings::GetPassCount();
    if (s_Renderer && s_Renderer->GetRendererType() == Standard2DRenderer::RendererTypeID)
        s_PassReceiver = static_cast<Standard2DRenderer*>(s_Renderer);

    for (int i = 0; i < passCount; i++)
    {
        const char* passName = Deki::ProjectSettings::GetPassName(i);
        const RenderPassInfo* info = DekiRenderPassRegistry::Get(passName);
        if (info && info->factory)
            AttachPass(passName, *info);
        else
            DEKI_LOG_WARNING("DekiRendering: No pass registered for '%s'", passName ? passName : "(null)");
    }

    // 3b. Auto-attach passes flagged autoAttach=true that the project's
    //     .rpipeline didn't already list. This lets package-owned passes
    //     (e.g. tilemap) participate without forcing every project to know
    //     package pass names. Projects can still mention an autoAttach pass
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

    // 3c. Install a hook so passes registered after this point (e.g. packages
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
    Deki::Engine::GetInstance().SetRenderSystem(s_RenderSystem);
    DEKI_LOG_INTERNAL("DekiRendering: Init complete (renderer=%p, %d passes)", (void*)s_Renderer, (int)s_Passes.size());
}

void DekiRendering_DetachPass(const char* name)
{
    if (!name) return;
    for (auto it = s_Passes.begin(); it != s_Passes.end(); ++it)
    {
        if (it->name != name) continue;

        if (s_PassReceiver)
            s_PassReceiver->RemovePass(it->pass);
        delete it->pass;
        s_Passes.erase(it);
        DEKI_LOG_INTERNAL("DekiRendering: Detached pass '%s'", name);
        return;
    }
}

void DekiRendering_ShutdownSystem()
{
    Deki::Engine::GetInstance().SetRenderSystem(nullptr);

    // Drop the late-attach hook so a stale lambda doesn't reference a freed
    // renderer if a package re-registers after shutdown.
    DekiRenderPassRegistry::SetAutoAttachCallback(nullptr);

    for (AttachedPass& p : s_Passes)
        delete p.pass;
    s_Passes.clear();
    s_PassReceiver = nullptr;

    delete s_Renderer;
    s_Renderer = nullptr;

    delete s_RenderSystem;
    s_RenderSystem = nullptr;
}
