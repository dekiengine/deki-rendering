#pragma once

/**
 * @file DekiRenderingInit.h
 * @brief Rendering package system initialization
 *
 * Creates the render system, renderer, and passes based on Deki::ProjectSettings.
 * Called by the auto-generated deki_init_package_systems() on all platforms.
 * In editor builds, also called from DekiRendering_EnsureRegistered().
 *
 * Idempotent — safe to call multiple times (e.g., during hot-reload).
 *
 * GLOBAL SCOPE, deliberately, unlike the rest of this package. The editor
 * generates a translation unit that declares these as plain
 * `extern void DekiRendering_InitSystem();` to bring a static simulator or
 * firmware build up, and it cannot know a package's namespace — which is why
 * the symbol carries the package prefix itself, as DekiRendering_RegisterComponents
 * from the reflection codegen does. 0.16.0 swept these into the namespace with
 * everything else and silently broke every simulator and firmware link.
 */
void DekiRendering_InitSystem();
void DekiRendering_ShutdownSystem();

namespace DekiRendering
{

/**
 * @brief Detach an autoAttached render pass by registry name.
 *
 * Removes the pass instance from the active renderer and deletes it. Most
 * callers should not invoke this directly — DekiRenderPassRegistry::Unregister
 * already calls this so a package's static destructor cleans up correctly on
 * DLL detach. Exposed for the rare case of detaching without unregistering
 * the factory. Safe to call when no matching pass is attached.
 */
void DekiRendering_DetachPass(const char* name);

}  // namespace DekiRendering
