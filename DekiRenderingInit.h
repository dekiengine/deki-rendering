#pragma once

// Starts the rendering package: creates the render system, the renderer and
// the passes named in Deki::ProjectSettings. Called by the generated
// DekiInitPackageSystems() on all platforms, and in editor builds also from
// DekiRenderingEnsureRegistered(). Safe to call more than once (e.g. during
// hot reload).
//
// These two must stay at global scope, unlike the rest of this package. The
// editor generates a translation unit that declares them as plain
// `extern void DekiRenderingInitSystem();` to start a static simulator or
// firmware build, and it cannot know a package's namespace, so the name
// carries the package prefix instead (like DekiRenderingRegisterComponents
// from the reflection codegen). Moving them into the namespace breaks every
// simulator and firmware link.
void DekiRenderingInitSystem();
void DekiRenderingShutdownSystem();

namespace DekiRendering
{

/// Removes the pass registered as `name` from the active renderer and deletes
/// it. Does nothing when no such pass is attached. DekiRenderPassRegistry::
/// Unregister already calls this, so most code never needs to; it is public
/// for detaching a pass without unregistering its factory.
void DekiRenderingDetachPass(const char* name);

}  // namespace DekiRendering
