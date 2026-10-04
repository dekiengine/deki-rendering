#include "DekiRenderPassRegistry.h"
#include <unordered_map>
#include <string>

namespace DekiRendering
{

// From DekiRenderingInit.h, declared here so this registry does not include
// the init header.
void DekiRenderingDetachPass(const char* name);

namespace DekiRenderPassRegistry
{

static std::unordered_map<std::string, RenderPassInfo>& GetRegistry()
{
    static std::unordered_map<std::string, RenderPassInfo> s_Reg;
    return s_Reg;
}

static AutoAttachCallback& GetAutoAttachCallback()
{
    static AutoAttachCallback s_Cb;
    return s_Cb;
}

void Register(const char* name, RenderPassInfo info)
{
    if (!name || !info.factory)
    {
        return;
    }

    GetRegistry()[name] = info;

    // Attach passes of packages that load after DekiRenderingInitSystem's
    // scan. DekiRenderingInitSystem installs the callback.
    if (info.autoAttach)
    {
        auto& cb = GetAutoAttachCallback();
        if (cb)
        {
            cb(name, info);
        }
    }
}

const RenderPassInfo* Get(const char* name)
{
    if (!name || name[0] == '\0')
    {
        return nullptr;
    }

    auto& reg = GetRegistry();
    auto it = reg.find(name);
    if (it != reg.end())
    {
        return &it->second;
    }

    return nullptr;
}

void Unregister(const char* name)
{
    if (!name)
    {
        return;
    }
    // Destroy the live pass before removing the factory. Its vtable lives in
    // the caller's DLL, which is usually about to unload (the static
    // destructor calling this runs during DLL detach); otherwise
    // deki-rendering's later shutdown would delete the pass through a freed
    // vtable.
    DekiRenderingDetachPass(name);
    GetRegistry().erase(name);
}

void GetAllNames(std::vector<std::string>& outNames)
{
    outNames.clear();
    for (const auto& [name, info] : GetRegistry())
    {
        outNames.push_back(name);
    }
}

void SetAutoAttachCallback(AutoAttachCallback cb)
{
    GetAutoAttachCallback() = std::move(cb);
}

}  // namespace DekiRenderPassRegistry

}  // namespace DekiRendering
