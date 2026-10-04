/**
 * @file DekiRenderingPackage.cpp
 * @brief Package entry point for deki-rendering DLL
 *
 * Registers the rendering subsystem with Deki::Engine.
 * For editor builds, this is a separate DLL that can be hot-reloaded.
 * For runtime builds, sources are statically linked into the engine.
 */

#include "DekiRenderingPackage.h"
#include "DekiRenderingInit.h"
#include <deki/Engine.h>
#include <deki/LogSystem.h>
#include <deki/interop/Plugin.h>
#include "CameraComponent.h"
#include "RendererComponent.h"
#include <deki/reflection/ComponentRegistry.h>
#include <deki/reflection/ComponentFactory.h>

extern void DekiRenderingRegisterComponents();
extern int DekiRenderingGetAutoComponentCount();
extern const Deki::ComponentMeta* DekiRenderingGetAutoComponentMeta(int index);

namespace DekiRendering
{

#ifdef DEKI_EDITOR

#ifndef DEKI_PLUGIN_EXPORTS
// Auto-generated registration helpers (standalone DLL only)

static bool s_Registered = false;

// The exports below are C symbols at global scope; the package's own
// registration helpers and statics live in its namespace.
using namespace DekiRendering;

extern "C"
{
    DEKI_RENDERING_API int DekiRenderingEnsureRegistered(void)
    {
        if (s_Registered)
        {
            return ::DekiRenderingGetAutoComponentCount();
        }
        s_Registered = true;

        // Auto-generated: registers rendering components with ComponentRegistry + ComponentFactory
        ::DekiRenderingRegisterComponents();

        // Initialize the rendering system (idempotent — may already be initialized
        // by DekiInitPackageSystems() during Deki::Engine::Initialize())
        DekiRenderingInitSystem();

        return ::DekiRenderingGetAutoComponentCount();
    }

}  // extern "C"
#endif  // DEKI_PLUGIN_EXPORTS

// =============================================================================
// Plugin metadata (for dynamic loading compatibility)
// =============================================================================

extern "C"
{
#ifndef DEKI_PLUGIN_EXPORTS
    DEKI_PLUGIN_API const char* DekiPluginGetName(void)
    {
        return "Deki Rendering Package";
    }

    DEKI_PLUGIN_API const char* DekiPluginGetVersion(void)
    {
#ifdef DEKI_PACKAGE_VERSION
        return DEKI_PACKAGE_VERSION;
#else
        return "0.0.0-dev";
#endif
    }

    DEKI_PLUGIN_API int DekiPluginInit(void)
    {
        return 0;
    }

    DEKI_PLUGIN_API void DekiPluginShutdown(void)
    {
        // Shutdown the rendering system (shared with non-editor builds)
        DekiRenderingShutdownSystem();
        s_Registered = false;
    }

    DEKI_PLUGIN_API int DekiPluginGetComponentCount(void)
    {
        return ::DekiRenderingGetAutoComponentCount();
    }

    DEKI_PLUGIN_API const Deki::ComponentMeta* DekiPluginGetComponentMeta(int index)
    {
        return ::DekiRenderingGetAutoComponentMeta(index);
    }

    DEKI_PLUGIN_API void DekiPluginRegisterComponents(void)
    {
        DekiRenderingEnsureRegistered();
    }

#endif  // DEKI_PLUGIN_EXPORTS

}  // extern "C"

// =============================================================================
// Package-specific API
// =============================================================================

DEKI_RENDERING_API const char* DekiRenderingGetName(void)
{
    return "Rendering";
}

#endif  // DEKI_EDITOR
}  // namespace DekiRendering
