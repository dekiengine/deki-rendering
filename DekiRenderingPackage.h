#pragma once

#ifdef DEKI_EDITOR
#include <cstdint>
#endif

namespace Deki
{
struct ComponentMeta;
}

namespace DekiRendering
{

// The rendering package:
// - DekiRenderSystem: framebuffer management
// - Standard2DRenderer: the default 2D render pipeline
// - QuadBlit: 2D blitting with transforms
// - RendererComponent: base class for renderable components
// - CameraComponent: camera and projection
//
// The engine runs without this package for headless and automation use.

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

extern "C"
{
    DEKI_RENDERING_API int DekiRenderingEnsureRegistered(void);

}  // extern "C"

#endif  // DEKI_EDITOR

}  // namespace DekiRendering
