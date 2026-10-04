#pragma once

// Factory registry for RenderPass implementations.
//
// Packages register their render passes during static initialisation. The
// render pipeline asset (.rpipeline) lists which passes to use, and at startup
// the rendering system creates and adds them.
//
// Usage, in your render pass .cpp file:
//
//   #include "DekiRenderPassRegistry.h"
//   static struct MyPassRegistrar {
//       MyPassRegistrar() {
//           DekiRenderPassRegistry::Register("mypass", {
//               []() -> RenderPass* { return new MyRenderPass(); },
//               false  // autoAttach
//           });
//       }
//       ~MyPassRegistrar() {
//           // Required so the registry doesn't outlive this DLL's code.
//           DekiRenderPassRegistry::Unregister("mypass");
//       }
//   } s_registrar;

#include "RenderPass.h"
#include <functional>
#include <vector>
#include <string>

namespace DekiRendering
{

using RenderPassFactory = std::function<RenderPass*()>;

/// Registration info for a render pass type.
struct RenderPassInfo
{
    RenderPassFactory factory;  // Creates a new pass instance

    // If true, the pass is attached to the active Standard2DRenderer even when
    // the project's .rpipeline does not list it. For passes that must run
    // whenever their package is loaded (e.g. tilemaps), so projects need not
    // know package pass names. A project can still list the pass in .rpipeline
    // to control its order relative to other passes.
    bool autoAttach = false;
};

namespace DekiRenderPassRegistry
{

/// Registers a render pass factory under a unique `name` (e.g. "clip2d").
void Register(const char* name, RenderPassInfo info);

/// The pass registered as `name`, or nullptr.
const RenderPassInfo* Get(const char* name);

/// Removes a registered render pass.
///
/// A package that registers a pass must unregister it on DLL detach.
/// Otherwise the registry holds a std::function whose code lives in the
/// unloaded package, and destroying it later (when deki-rendering unloads)
/// jumps to unmapped memory.
///
/// Also detaches the live pass from the active renderer, like
/// DekiRenderingDetachPass(name): the pass's vtable lives in the caller's DLL,
/// which is usually about to unload, so the pass must be destroyed while that
/// code is still mapped.
void Unregister(const char* name);

/// Fills `outNames` with the names of all registered passes.
void GetAllNames(std::vector<std::string>& outNames);

/// Installs a callback called whenever an autoAttach pass is registered.
/// DekiRenderingInit installs it once the active renderer exists, so a package
/// that loads after the rendering system starts (e.g. deki-tilemap) still gets
/// its pass attached. Packages do not need to know about it.
using AutoAttachCallback = std::function<void(const char*, const RenderPassInfo&)>;
void SetAutoAttachCallback(AutoAttachCallback cb);

}  // namespace DekiRenderPassRegistry

}  // namespace DekiRendering
