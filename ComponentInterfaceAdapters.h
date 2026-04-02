#pragma once

#include "DekiComponent.h"

/**
 * @file ComponentInterfaceAdapters.h
 * @brief Registry for extracting interfaces from components without RTTI or vtable changes
 *
 * Modules register adapter functions that convert a DekiComponent* to an interface pointer
 * based on the component's StaticType. The renderer queries this to find IClipProvider,
 * ISortableProvider, etc. without knowing the concrete component class.
 *
 * Usage:
 * @code
 * // In your component .cpp:
 * #include "ComponentInterfaceAdapters.h"
 * static struct MyAdapterRegistrar {
 *     MyAdapterRegistrar() {
 *         ComponentInterfaceAdapters::Register(
 *             IClipProvider::InterfaceID, MyComponent::StaticType,
 *             [](DekiComponent* c) -> void* {
 *                 return static_cast<IClipProvider*>(static_cast<MyComponent*>(c));
 *             });
 *     }
 * } s_reg;
 * @endcode
 */

using InterfaceAdapter = void*(*)(DekiComponent*);

namespace ComponentInterfaceAdapters {

void Register(uint32_t interfaceId, ComponentType componentType, InterfaceAdapter adapter);
void* Query(uint32_t interfaceId, DekiComponent* comp);

} // namespace ComponentInterfaceAdapters
