#include "ComponentInterfaceAdapters.h"
#include <unordered_map>

namespace ComponentInterfaceAdapters {

// Key: (interfaceId << 32) | componentType
// This allows O(1) lookup for a specific interface on a specific component type
static std::unordered_map<uint64_t, InterfaceAdapter>& GetRegistry()
{
    static std::unordered_map<uint64_t, InterfaceAdapter> reg;
    return reg;
}

static uint64_t MakeKey(uint32_t interfaceId, ComponentType componentType)
{
    return (static_cast<uint64_t>(interfaceId) << 32) | componentType;
}

void Register(uint32_t interfaceId, ComponentType componentType, InterfaceAdapter adapter)
{
    if (adapter)
        GetRegistry()[MakeKey(interfaceId, componentType)] = adapter;
}

void* Query(uint32_t interfaceId, DekiComponent* comp)
{
    if (!comp) return nullptr;

    auto& reg = GetRegistry();

    // Try exact type match first
    auto it = reg.find(MakeKey(interfaceId, comp->getType()));
    if (it != reg.end())
        return it->second(comp);

    // Try base type match (for inherited components)
    ComponentType baseType = comp->getBaseType();
    if (baseType != 0)
    {
        it = reg.find(MakeKey(interfaceId, baseType));
        if (it != reg.end())
            return it->second(comp);
    }

    return nullptr;
}

} // namespace ComponentInterfaceAdapters
