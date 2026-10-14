#include "RenderProfiler.h"

#include <algorithm>
#include <map>

namespace DekiRendering
{
namespace RenderProfiler
{

namespace
{
Clock s_Clock = nullptr;
std::map<uint32_t, Entry> s_Entries;
std::map<uint32_t, PathEntry> s_Paths;
uint32_t s_Frames = 0;
}  // namespace

void Enable(Clock clock)
{
    s_Clock = clock;
    s_Entries.clear();
    s_Paths.clear();
    s_Frames = 0;
}

bool IsEnabled()
{
    return s_Clock != nullptr;
}

uint64_t Now()
{
    return s_Clock ? s_Clock() : 0;
}

void AddContent(uint32_t typeId, uint64_t us, const char* name)
{
    Entry& e = s_Entries[typeId];
    e.typeId = typeId;
    if (name)
    {
        e.name = name;
    }
    e.contentUs += us;
    ++e.draws;
}

void AddBlit(uint32_t typeId, uint64_t us, uint64_t pixels)
{
    Entry& e = s_Entries[typeId];
    e.typeId = typeId;
    e.blitUs += us;
    e.pixels += pixels;
}

void CountFrame()
{
    if (s_Clock)
    {
        ++s_Frames;
    }
}

std::vector<Entry> Take(uint32_t& frames)
{
    std::vector<Entry> out;
    out.reserve(s_Entries.size());
    for (const auto& [id, entry] : s_Entries)
    {
        out.push_back(entry);
    }
    std::sort(out.begin(), out.end(),
              [](const Entry& a, const Entry& b) { return a.contentUs + a.blitUs > b.contentUs + b.blitUs; });
    frames = s_Frames;
    s_Entries.clear();
    s_Frames = 0;
    return out;
}

void AddPath(uint32_t path, const char* name, uint64_t us, uint64_t pixels)
{
    PathEntry& e = s_Paths[path];
    e.name = name;
    ++e.blits;
    e.pixels += pixels;
    e.us += us;
}

std::vector<PathEntry> TakePaths()
{
    std::vector<PathEntry> out;
    out.reserve(s_Paths.size());
    for (const auto& [id, entry] : s_Paths)
    {
        out.push_back(entry);
    }
    std::sort(out.begin(), out.end(), [](const PathEntry& a, const PathEntry& b) { return a.us > b.us; });
    s_Paths.clear();
    return out;
}

}  // namespace RenderProfiler
}  // namespace DekiRendering
