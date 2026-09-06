#include "DekiSortingCallbackRegistry.h"
#include <unordered_map>
#include <string>

namespace DekiSortingCallbackRegistry {

static std::unordered_map<std::string, SortingCallback>& GetRegistry()
{
    static std::unordered_map<std::string, SortingCallback> reg;
    return reg;
}

void Register(const char* name, SortingCallback callback)
{
    if (name && callback)
        GetRegistry()[name] = callback;
}

void GetAll(std::vector<SortingCallback>& outCallbacks)
{
    outCallbacks.clear();
    for (const auto& [name, cb] : GetRegistry())
        outCallbacks.push_back(cb);
}

} // namespace DekiSortingCallbackRegistry
