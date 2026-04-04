/**
 * @file DekiRenderingModule.cpp
 * @brief Module entry point for deki-rendering DLL
 *
 * Registers the rendering subsystem with DekiEngine.
 * For editor builds, this is a separate DLL that can be hot-reloaded.
 * For runtime builds, sources are statically linked into the engine.
 */

#include "DekiRenderingModule.h"
#include "DekiRenderingInit.h"
#include "DekiEngine.h"
#include "DekiLogSystem.h"
#include "interop/DekiPlugin.h"
#include "DekiModuleFeatureMeta.h"
#include "CameraComponent.h"
#include "RendererComponent.h"
#include "reflection/ComponentRegistry.h"
#include "reflection/ComponentFactory.h"

#ifdef DEKI_EDITOR

#include "imgui.h"

#ifndef DEKI_PLUGIN_EXPORTS
// Auto-generated registration helpers (standalone DLL only)
extern void DekiRendering_RegisterComponents();
extern int DekiRendering_GetAutoComponentCount();
extern const DekiComponentMeta* DekiRendering_GetAutoComponentMeta(int index);

static bool s_Registered = false;

extern "C" {

DEKI_RENDERING_API void DekiRendering_SetImGuiContext(void* ctx)
{
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx));
}

DEKI_RENDERING_API int DekiRendering_EnsureRegistered(void)
{
    if (s_Registered)
        return DekiRendering_GetAutoComponentCount();
    s_Registered = true;

    // Auto-generated: registers rendering components with ComponentRegistry + ComponentFactory
    DekiRendering_RegisterComponents();

    // Initialize the rendering system (idempotent — may already be initialized
    // by deki_init_module_systems() during DekiEngine::Initialize())
    DekiRendering_InitSystem();

    return DekiRendering_GetAutoComponentCount();
}

} // extern "C"
#endif // DEKI_PLUGIN_EXPORTS

// =============================================================================
// Plugin metadata (for dynamic loading compatibility)
// =============================================================================

extern "C" {

#ifndef DEKI_PLUGIN_EXPORTS
DEKI_PLUGIN_API const char* DekiPlugin_GetName(void)
{
    return "Deki Rendering Module";
}

DEKI_PLUGIN_API const char* DekiPlugin_GetVersion(void)
{
#ifdef DEKI_MODULE_VERSION
    return DEKI_MODULE_VERSION;
#else
    return "0.0.0-dev";
#endif
}

DEKI_PLUGIN_API const char* DekiPlugin_GetReflectionJson(void)
{
    return "{}";
}

DEKI_PLUGIN_API int DekiPlugin_Init(void)
{
    return 0;
}

DEKI_PLUGIN_API void DekiPlugin_Shutdown(void)
{
    // Shutdown the rendering system (shared with non-editor builds)
    DekiRendering_ShutdownSystem();
    s_Registered = false;
}

DEKI_PLUGIN_API void DekiPlugin_SetImGuiContext(void* ctx)
{
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx));
}

DEKI_PLUGIN_API int DekiPlugin_GetComponentCount(void)
{
    return DekiRendering_GetAutoComponentCount();
}

DEKI_PLUGIN_API const DekiComponentMeta* DekiPlugin_GetComponentMeta(int index)
{
    return DekiRendering_GetAutoComponentMeta(index);
}

DEKI_PLUGIN_API void DekiPlugin_RegisterComponents(void)
{
    DekiRendering_EnsureRegistered();
}

#endif // DEKI_PLUGIN_EXPORTS (resume after feature data)

// =============================================================================
// Module Feature API
// =============================================================================

static const char* s_CameraGuids[] = { CameraComponent::StaticGuid };
static const char* s_RendererGuids[] = { RendererComponent::StaticGuid };

static const DekiModuleFeatureInfo s_Features[] = {
    {"camera",   "Camera",   "2D camera with projection",      true, "",  s_CameraGuids,   1},
    {"renderer", "Renderer", "Base renderer component",        true, "",  s_RendererGuids,  1},
};

#ifndef DEKI_PLUGIN_EXPORTS
DEKI_PLUGIN_API int DekiPlugin_GetFeatureCount(void)
{
    return sizeof(s_Features) / sizeof(s_Features[0]);
}

DEKI_PLUGIN_API const DekiModuleFeatureInfo* DekiPlugin_GetFeature(int index)
{
    if (index < 0 || index >= DekiPlugin_GetFeatureCount())
        return nullptr;
    return &s_Features[index];
}

#endif // DEKI_PLUGIN_EXPORTS

} // extern "C"

// =============================================================================
// Module-specific API
// =============================================================================

DEKI_RENDERING_API const char* DekiRendering_GetName(void)
{
    return "Rendering";
}

DEKI_RENDERING_API int DekiRendering_GetFeatureCount(void)
{
    return static_cast<int>(sizeof(s_Features) / sizeof(s_Features[0]));
}

DEKI_RENDERING_API const DekiModuleFeatureInfo* DekiRendering_GetFeature(int index)
{
    if (index < 0 || index >= DekiRendering_GetFeatureCount())
        return nullptr;
    return &s_Features[index];
}

#endif // DEKI_EDITOR
