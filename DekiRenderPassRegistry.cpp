#include "DekiRenderPassRegistry.h"
#include <unordered_map>
#include <string>

namespace DekiRenderPassRegistry {

static std::unordered_map<std::string, RenderPassInfo>& GetRegistry()
{
    static std::unordered_map<std::string, RenderPassInfo> reg;
    return reg;
}

static AutoAttachCallback& GetAutoAttachCallback()
{
    static AutoAttachCallback cb;
    return cb;
}

void Register(const char* name, RenderPassInfo info)
{
    if (!name || !info.factory)
        return;

    GetRegistry()[name] = info;

    // Late-attach for modules that load after DekiRendering_InitSystem already
    // ran its scan. The callback is installed by DekiRendering_InitSystem.
    if (info.autoAttach)
    {
        auto& cb = GetAutoAttachCallback();
        if (cb) cb(name, info);
    }
}

const RenderPassInfo* Get(const char* name)
{
    if (!name || name[0] == '\0')
        return nullptr;

    auto& reg = GetRegistry();
    auto it = reg.find(name);
    if (it != reg.end())
        return &it->second;

    return nullptr;
}

void GetAllNames(std::vector<std::string>& outNames)
{
    outNames.clear();
    for (const auto& [name, info] : GetRegistry())
        outNames.push_back(name);
}

void SetAutoAttachCallback(AutoAttachCallback cb)
{
    GetAutoAttachCallback() = std::move(cb);
}

} // namespace DekiRenderPassRegistry
