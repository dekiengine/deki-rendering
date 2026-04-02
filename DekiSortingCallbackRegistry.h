#pragma once

/**
 * @file DekiSortingCallbackRegistry.h
 * @brief Registry for sorting callbacks — always-on, not tied to passes
 *
 * Modules self-register sorting callbacks at static init time.
 * All registered callbacks are added to the renderer at startup,
 * regardless of which passes are in the pipeline.
 *
 * Usage:
 * @code
 * #include "DekiSortingCallbackRegistry.h"
 * static struct MySortingRegistrar {
 *     MySortingRegistrar() {
 *         DekiSortingCallbackRegistry::Register("mysorting", &MySortingCallback);
 *     }
 * } s_registrar;
 * @endcode
 */

#include "RenderPass.h"  // For SortingCallback typedef
#include <vector>

namespace DekiSortingCallbackRegistry {

void Register(const char* name, SortingCallback callback);
void GetAll(std::vector<SortingCallback>& outCallbacks);

} // namespace DekiSortingCallbackRegistry
