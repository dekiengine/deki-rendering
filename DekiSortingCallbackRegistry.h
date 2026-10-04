#pragma once

// Registry for sorting callbacks. These always apply; they are not tied to
// passes.
//
// Packages register sorting callbacks during static initialisation. At
// startup every registered callback is added to the renderer, whichever
// passes are in the pipeline.
//
//   #include "DekiSortingCallbackRegistry.h"
//   static struct MySortingRegistrar {
//       MySortingRegistrar() {
//           DekiSortingCallbackRegistry::Register("mysorting", &MySortingCallback);
//       }
//   } s_registrar;

#include "RenderPass.h"  // SortingCallback
#include <vector>

namespace DekiRendering
{

namespace DekiSortingCallbackRegistry
{

void Register(const char* name, SortingCallback callback);
void GetAll(std::vector<SortingCallback>& outCallbacks);

}  // namespace DekiSortingCallbackRegistry

}  // namespace DekiRendering
