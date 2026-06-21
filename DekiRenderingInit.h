#pragma once

/**
 * @file DekiRenderingInit.h
 * @brief Rendering module system initialization
 *
 * Creates the render system, renderer, and passes based on ProjectSettings.
 * Called by the auto-generated deki_init_module_systems() on all platforms.
 * In editor builds, also called from DekiRendering_EnsureRegistered().
 *
 * Idempotent — safe to call multiple times (e.g., during hot-reload).
 */

void DekiRendering_InitSystem();
void DekiRendering_ShutdownSystem();

/**
 * @brief Detach an autoAttached render pass by registry name.
 *
 * Removes the pass instance from the active renderer and deletes it. Most
 * callers should not invoke this directly — DekiRenderPassRegistry::Unregister
 * already calls this so a module's static destructor cleans up correctly on
 * DLL detach. Exposed for the rare case of detaching without unregistering
 * the factory. Safe to call when no matching pass is attached.
 */
void DekiRendering_DetachPass(const char* name);
