#pragma once

/**
 * @file DekiRenderingModule.h
 * @brief Central header for the Deki Rendering Module
 *
 * This module provides the rendering subsystem:
 * - DekiRenderSystem: framebuffer management
 * - Standard2DRenderer: default 2D render pipeline
 * - QuadBlit: 2D blitting with transforms
 * - RendererComponent: abstract base for renderable components
 * - CameraComponent: camera/projection
 *
 * The engine can run without this module for headless/automation use cases.
 */

// DLL export macro
#ifdef _WIN32
    #if defined(DEKI_RENDERING_EXPORTS) || defined(DEKI_PLUGIN_EXPORTS)
        #define DEKI_RENDERING_API __declspec(dllexport)
    #else
        #define DEKI_RENDERING_API __declspec(dllimport)
    #endif
#else
    #define DEKI_RENDERING_API __attribute__((visibility("default")))
#endif

#ifdef DEKI_EDITOR
#include <cstdint>

// Forward declarations
struct DekiComponentMeta;

extern "C" {

DEKI_RENDERING_API void DekiRendering_SetImGuiContext(void* ctx);
DEKI_RENDERING_API int DekiRendering_EnsureRegistered(void);

} // extern "C"

#endif // DEKI_EDITOR
