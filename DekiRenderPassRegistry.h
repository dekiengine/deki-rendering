#pragma once

/**
 * @file DekiRenderPassRegistry.h
 * @brief Factory registry for RenderPass implementations
 *
 * Modules self-register their render passes at static init time.
 * The render pipeline asset (.rpipeline) specifies which passes to activate.
 * At startup, the rendering system creates and adds the configured passes.
 *
 * Usage:
 * @code
 * // In your render pass .cpp file:
 * #include "DekiRenderPassRegistry.h"
 * static struct MyPassRegistrar {
 *     MyPassRegistrar() {
 *         DekiRenderPassRegistry::Register("mypass", {
 *             []() -> RenderPass* { return new MyRenderPass(); },
 *             &MySortingCallback  // or nullptr if no sorting
 *         });
 *     }
 * } s_registrar;
 * @endcode
 */

#include "RenderPass.h"
#include <functional>
#include <vector>
#include <string>

using RenderPassFactory = std::function<RenderPass*()>;

/**
 * @brief Registration info for a render pass type
 */
struct RenderPassInfo
{
    RenderPassFactory factory;           // Creates a new pass instance

    // If true, the rendering init auto-attaches this pass to the active
    // Standard2DRenderer when it isn't already listed in the project's
    // .rpipeline. Use for passes that *must* run whenever their owning module
    // is loaded (e.g. tilemaps) — avoids forcing every project to know module
    // pass names. Projects can still override by listing the pass explicitly
    // in .rpipeline to control ordering relative to other passes.
    bool autoAttach = false;
};

namespace DekiRenderPassRegistry {

/**
 * @brief Register a render pass factory by name
 * @param name Unique identifier (e.g., "clip2d")
 * @param info Factory function and optional sorting callback
 */
void Register(const char* name, RenderPassInfo info);

/**
 * @brief Look up a render pass by name
 * @param name The registered name
 * @return Pointer to registration info, or nullptr if not found
 */
const RenderPassInfo* Get(const char* name);

/**
 * @brief Get names of all registered render passes
 * @param outNames Vector to fill with registered pass names
 */
void GetAllNames(std::vector<std::string>& outNames);

} // namespace DekiRenderPassRegistry
