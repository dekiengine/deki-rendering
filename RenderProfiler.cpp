#include "RenderProfiler.h"
#include "RendererComponent.h"

#include <deki/Object.h>

#include <algorithm>
#include <map>
#include <utility>

namespace DekiRendering
{
namespace RenderProfiler
{

namespace
{
Clock s_Clock = nullptr;
std::map<uint32_t, Entry> s_Entries;
std::map<uint32_t, PathEntry> s_Paths;
// Keyed by the component (or pass) and its object: a pass draws many.
std::map<std::pair<const void*, const void*>, ObjectEntry> s_Objects;
uint32_t s_Frames = 0;

std::string PathOf(const Deki::Object* object)
{
    std::string path;
    for (const Deki::Object* o = object; o; o = o->GetParent())
    {
        path = path.empty() ? o->GetName() : o->GetName() + "/" + path;
    }
    return path;
}

// The row for `renderer`, made the first time it is met.
ObjectEntry& RowOf(const RendererComponent* renderer)
{
    const Deki::Object* owner = renderer->GetOwner();
    ObjectEntry& e = s_Objects[{ renderer, owner }];
    if (!e.type)
    {
        const Deki::ComponentTypeInfo* info = renderer->GetTypeInfo();
        e.type = info && info->shortName ? info->shortName : "Renderer";
        e.path = PathOf(owner);
    }
    e.order = renderer->sortingOrder;
    return e;
}
}  // namespace

void Enable(Clock clock)
{
    s_Clock = clock;
    s_Entries.clear();
    s_Paths.clear();
    s_Objects.clear();
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

void AddBlit(uint32_t typeId, uint64_t us, uint64_t pixels, const char* name)
{
    Entry& e = s_Entries[typeId];
    e.typeId = typeId;
    if (name)
    {
        e.name = name;
    }
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

void AddObjectMeasure(const RendererComponent* renderer, uint64_t us)
{
    if (s_Clock && renderer)
    {
        RowOf(renderer).measureUs += us;
    }
}

void AddObjectDraw(const RendererComponent* renderer, uint64_t contentUs, uint64_t blitUs, uint64_t pixels)
{
    if (!s_Clock || !renderer)
    {
        return;
    }
    ObjectEntry& e = RowOf(renderer);
    e.contentUs += contentUs;
    e.blitUs += blitUs;
    e.pixels += pixels;
    if (blitUs > 0 || pixels > 0)
    {
        ++e.draws;
    }
}

void AddObjectRedraw(const RendererComponent* renderer, Redraw why)
{
    if (s_Clock && renderer)
    {
        ++RowOf(renderer).redraws[static_cast<int>(why)];
    }
}

void AddObjectPass(const void* pass, const char* passName, const Deki::Object* object, uint64_t us)
{
    if (!s_Clock || !pass)
    {
        return;
    }
    ObjectEntry& e = s_Objects[{ pass, object }];
    if (!e.type)
    {
        e.type = passName ? passName : "Render pass";
        e.path = PathOf(object);
    }
    e.contentUs += us;
    ++e.draws;
}

std::vector<ObjectEntry> TakeObjects()
{
    std::vector<ObjectEntry> out;
    out.reserve(s_Objects.size());
    for (auto& [key, entry] : s_Objects)
    {
        out.push_back(std::move(entry));
    }
    std::sort(out.begin(), out.end(),
              [](const ObjectEntry& a, const ObjectEntry& b)
              {
                  return a.measureUs + a.contentUs + a.blitUs > b.measureUs + b.contentUs + b.blitUs;
              });
    s_Objects.clear();
    return out;
}

}  // namespace RenderProfiler
}  // namespace DekiRendering
