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
