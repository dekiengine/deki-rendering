#pragma once

/**
 * @file DekiRendererRegistry.h
 * @brief Factory registry for DekiRenderer implementations
 *
 * Modules self-register their renderers at static init time.
 * The rendering system creates the configured renderer at startup
 * by looking up the name from ProjectSettings.
 *
 * Usage:
 * @code
 * // In your renderer .cpp file:
 * #include "DekiRendererRegistry.h"
 * static struct MyRendererRegistrar {
 *     MyRendererRegistrar() {
 *         DekiRendererRegistry::Register("myrenderer",
 *             []() -> DekiRenderer* { return new MyRenderer(); });
 *     }
 * } s_registrar;
 * @endcode
 */

#include <functional>
#include <vector>
#include <string>

class DekiRenderer;

using DekiRendererFactory = std::function<DekiRenderer*()>;

namespace DekiRendererRegistry {

/**
 * @brief Register a renderer factory by name
 * @param name Unique identifier (e.g., "standard2d")
 * @param factory Function that creates a new renderer instance
 */
void Register(const char* name, DekiRendererFactory factory);

/**
 * @brief Create a renderer by name
 * @param name The registered name (e.g., "standard2d")
 * @return New renderer instance, or nullptr if name not found or empty
 */
DekiRenderer* Create(const char* name);

/**
 * @brief Get names of all registered renderers
 * @param outNames Vector to fill with registered renderer names
 */
void GetAllNames(std::vector<std::string>& outNames);

} // namespace DekiRendererRegistry
