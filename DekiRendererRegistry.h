#pragma once

// Factory registry for DekiRenderer implementations.
//
// Packages register their renderers during static initialisation. At startup
// the rendering system creates the renderer named in Deki::ProjectSettings.
//
// Usage, in your renderer .cpp file:
//
//   #include "DekiRendererRegistry.h"
//   static struct MyRendererRegistrar {
//       MyRendererRegistrar() {
//           DekiRendererRegistry::Register("myrenderer",
//               []() -> DekiRenderer* { return new MyRenderer(); });
//       }
//   } s_registrar;

#include <functional>
#include <vector>
#include <string>

namespace DekiRendering
{

class DekiRenderer;

using DekiRendererFactory = std::function<DekiRenderer*()>;

namespace DekiRendererRegistry
{

/// Registers a renderer factory under a unique `name` (e.g. "standard2d").
void Register(const char* name, DekiRendererFactory factory);

/// Creates the renderer registered as `name`. nullptr if the name is empty or
/// not registered.
DekiRenderer* Create(const char* name);

/// Fills `outNames` with the names of all registered renderers.
void GetAllNames(std::vector<std::string>& outNames);

}  // namespace DekiRendererRegistry

}  // namespace DekiRendering
