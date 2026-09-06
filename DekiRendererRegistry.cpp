#include "DekiRendererRegistry.h"
#include <unordered_map>
#include <string>

namespace DekiRendererRegistry {

// Meyer's singleton — avoids static init order issues across translation units
static std::unordered_map<std::string, DekiRendererFactory>& GetRegistry()
{
    static std::unordered_map<std::string, DekiRendererFactory> reg;
    return reg;
}

void Register(const char* name, DekiRendererFactory factory)
{
    if (name && factory)
        GetRegistry()[name] = factory;
}

DekiRenderer* Create(const char* name)
{
    if (!name || name[0] == '\0')
        return nullptr;

    auto& reg = GetRegistry();
    auto it = reg.find(name);
    if (it != reg.end())
        return it->second();

    return nullptr;
}

void GetAllNames(std::vector<std::string>& outNames)
{
    outNames.clear();
    for (const auto& [name, factory] : GetRegistry())
        outNames.push_back(name);
}

} // namespace DekiRendererRegistry
