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
 * Called by a module's DekiPlugin_Shutdown so its pass instance is removed
 * from the active renderer (and deleted) BEFORE the module's DLL unloads —
 * otherwise the renderer keeps invoking Execute() on a vtable that no longer
 * exists. Safe to call when no matching pass is attached.
 */
void DekiRendering_DetachPass(const char* name);
