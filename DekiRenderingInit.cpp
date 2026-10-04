#include "DekiRenderingInit.h"
#include "DekiRenderSystem.h"
#include "DekiRendererRegistry.h"
#include "DekiRenderPassRegistry.h"
#include "DekiSortingCallbackRegistry.h"
#include "Standard2DRenderer.h"
#include <deki/Engine.h>
#include <deki/ProjectSettings.h>
#include <deki/LogSystem.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace DekiRendering
{

static DekiRenderSystem* s_RenderSystem = nullptr;
static DekiRenderer* s_Renderer = nullptr;
static Standard2DRenderer* s_PassReceiver = nullptr;

// Passes this system created, in attach order. Any number of passes.
struct AttachedPass
{
    std::string name;
    RenderPass* pass;
};
static std::vector<AttachedPass> s_Passes;

// Pass names that no package registers because the renderer performs the work
// itself, and that a pipeline may still list.
//
// "clip2d" is the one: Standard2DRenderer clips itself, pushing a clip rect
// around any object exposing IClipProvider, but older projects still list a
// clip2d pass. It is recognised here, not registered as a no-op pass, so
// those projects log no false warning and the pipeline shows what really runs.
static bool IsBuiltinPassName(const char* name)
{
    return name && std::strcmp(name, "clip2d") == 0;
}

static void AttachPass(const char* name, const RenderPassInfo& info)
{
    if (!s_PassReceiver || !info.factory)
    {
        return;
    }
    for (const AttachedPass& p : s_Passes)
    {
        if (p.name == name)
        {
            return;  // already attached
        }
    }

    RenderPass* pass = info.factory();
    s_Passes.push_back({ name, pass });
    s_PassReceiver->AddPass(pass);
    DEKI_LOG_INTERNAL("DekiRendering: Attached pass '%s'", name);
}

void DekiRenderingDetachPass(const char* name)
{
    if (!name)
    {
        return;
    }
    for (auto it = s_Passes.begin(); it != s_Passes.end(); ++it)
    {
        if (it->name != name)
        {
            continue;
        }

        if (s_PassReceiver)
        {
            s_PassReceiver->RemovePass(it->pass);
        }
        delete it->pass;
        s_Passes.erase(it);
        DEKI_LOG_INTERNAL("DekiRendering: Detached pass '%s'", name);
        return;
    }
}

}  // namespace DekiRendering

// Global scope, matching the editor's generated glue. See DekiRenderingInit.h.
using namespace DekiRendering;

void DekiRenderingInitSystem()
{
    if (s_RenderSystem)
    {
        return;
    }

    // 1. The render system: framebuffer and camera lookup.
    s_RenderSystem = new DekiRenderSystem();

    // 2. The renderer named in the project settings.
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

    // 3. The passes named in the project settings. GetRendererType() makes
    //    the downcast safe without RTTI.
    int passCount = Deki::ProjectSettings::GetPassCount();
    if (s_Renderer && s_Renderer->GetRendererType() == Standard2DRenderer::kRendererTypeID)
    {
        s_PassReceiver = static_cast<Standard2DRenderer*>(s_Renderer);
    }

    for (int i = 0; i < passCount; i++)
    {
        const char* passName = Deki::ProjectSettings::GetPassName(i);
        const RenderPassInfo* info = DekiRenderPassRegistry::Get(passName);
        if (info && info->factory)
        {
            AttachPass(passName, *info);
        }
        else if (IsBuiltinPassName(passName))
        {
            continue;  // the renderer does it itself; see IsBuiltinPassName
        }
        else
        {
            DEKI_LOG_WARNING("DekiRendering: No pass registered for '%s'", passName ? passName : "(null)");
        }
    }

    // 3b. autoAttach passes the project's .rpipeline does not list, so
    //     package passes (e.g. tilemap) run without every project knowing
    //     their names. A project can still list one to control its order.
    if (s_PassReceiver)
    {
        std::vector<std::string> allPassNames;
        DekiRenderPassRegistry::GetAllNames(allPassNames);
        for (const auto& name : allPassNames)
        {
            const RenderPassInfo* info = DekiRenderPassRegistry::Get(name.c_str());
            if (!info || !info->autoAttach)
            {
                continue;
            }
            AttachPass(name.c_str(), *info);
        }
    }

    // 3c. A hook so autoAttach passes registered later, by packages that load
    //     after this scan (deki-tilemap does), are attached too.
    DekiRenderPassRegistry::SetAutoAttachCallback([](const char* name, const RenderPassInfo& info)
                                                  { AttachPass(name, info); });

    // 4. Every registered sorting callback. These always apply and are not
    //    tied to passes.
    if (s_PassReceiver)
    {
        std::vector<SortingCallback> sortingCallbacks;
        DekiSortingCallbackRegistry::GetAll(sortingCallbacks);
        for (auto cb : sortingCallbacks)
        {
            s_PassReceiver->AddSortingCallback(cb);
        }
        if (!sortingCallbacks.empty())
        {
            DEKI_LOG_INTERNAL("DekiRendering: Added %d sorting callbacks", (int)sortingCallbacks.size());
        }
    }

    // 5. Hand the render system to the engine.
    Deki::Engine::GetInstance().SetRenderSystem(s_RenderSystem);
    DEKI_LOG_INTERNAL("DekiRendering: Init complete (renderer=%p, %d passes)", (void*)s_Renderer, (int)s_Passes.size());
}

void DekiRenderingShutdownSystem()
{
    Deki::Engine::GetInstance().SetRenderSystem(nullptr);

    // Remove the late-attach hook, so a package registering after shutdown
    // does not reach a freed renderer.
    DekiRenderPassRegistry::SetAutoAttachCallback(nullptr);

    for (AttachedPass& p : s_Passes)
    {
        delete p.pass;
    }
    s_Passes.clear();
    s_PassReceiver = nullptr;

    delete s_Renderer;
    s_Renderer = nullptr;

    delete s_RenderSystem;
    s_RenderSystem = nullptr;
}
