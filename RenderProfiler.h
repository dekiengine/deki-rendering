#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "DekiRenderingPackage.h"

namespace Deki
{
class Object;
}

namespace DekiRendering
{
class RendererComponent;

/// Where a frame's render time goes, per kind of component: how long the
/// renderer spent getting each one's pixels ready (RenderContent: rasterising
/// text, baking a gradient) and blitting them, and how many source pixels
/// that was. Off until a clock is given; a platform turns it on to measure a
/// game on the hardware (the ESP32 HAL's DEKI:PROFILE).
namespace RenderProfiler
{

/// Microseconds, monotonic. nullptr turns the profiler off.
using Clock = uint64_t (*)();

DEKI_RENDERING_API void Enable(Clock clock);
DEKI_RENDERING_API bool IsEnabled();
DEKI_RENDERING_API uint64_t Now();

/// Type ids for the frame's clear, for handing its strips to the display and
/// for finding what changed (redrawChanges), which belong to no component.
constexpr uint32_t kClear = 1;
constexpr uint32_t kPresent = 2;
constexpr uint32_t kChanges = 3;
constexpr uint32_t kScroll = 4;  // moving the last frame for a camera that scrolled
constexpr uint32_t kPasses = 5;  // render passes' Execute, their blits included

struct Entry
{
    uint32_t typeId = 0;         // the component's type, kClear or kPresent
    const char* name = nullptr;  // its short name, when the caller gave one
    uint64_t contentUs = 0;
    uint64_t blitUs = 0;
    uint32_t draws = 0;
    uint64_t pixels = 0;  // source pixels blitted
};

/// `name` (static storage, such as a ComponentTypeInfo's) labels the type in
/// the report, where a stripped build's registry cannot.
DEKI_RENDERING_API void AddContent(uint32_t typeId, uint64_t us, const char* name = nullptr);
DEKI_RENDERING_API void AddBlit(uint32_t typeId, uint64_t us, uint64_t pixels, const char* name = nullptr);
DEKI_RENDERING_API void CountFrame();

/// What was measured since the last call, most expensive first, and over how
/// many frames; then starts again.
DEKI_RENDERING_API std::vector<Entry> Take(uint32_t& frames);

/// Time per blitter path (QuadBlit's fast loops and its generic pipeline):
/// which paths a game's frames go through, and what each costs a pixel.
struct PathEntry
{
    const char* name = nullptr;  // static storage
    uint32_t blits = 0;
    uint64_t pixels = 0;  // destination pixels inside the clip
    uint64_t us = 0;
};

DEKI_RENDERING_API void AddPath(uint32_t path, const char* name, uint64_t us, uint64_t pixels);

/// The paths measured since the last call, most expensive first; then starts
/// again. Call it before Take, which counts the frames for both.
DEKI_RENDERING_API std::vector<PathEntry> TakePaths();

/// Why a renderer's area was drawn again.
enum class Redraw : uint8_t
{
    New = 0,            // first drawn, or back on screen
    Changed = 1,        // drawn differently, or somewhere else
    StillOnScreen = 2,  // the same on screen while the world scrolled under it
    OverPin = 3,        // drawn where something kept on screen sits
};
constexpr int kRedrawReasons = 4;

/// What one renderer, or one render pass on one object, cost: the
/// breakdown of a scene a developer reads to find what is slow.
struct ObjectEntry
{
    std::string path;            // the object's path from its root, "Camera/Counter"
    const char* type = nullptr;  // the component's short name, or the pass's
    int32_t order = 0;           // its sorting order (renderers)
    uint64_t measureUs = 0;      // finding what changed in it (redrawChanges); includes its content made then
    uint64_t contentUs = 0;      // making its pixels while drawing (RenderContent), or a pass's Execute
    uint64_t blitUs = 0;         // drawing its pixels
    uint64_t pixels = 0;         // source pixels drawn
    uint32_t draws = 0;          // times drawn: once for each part of a frame it was drawn in
    uint32_t redraws[kRedrawReasons] = {};  // times its area was drawn again, by Redraw
};

/// A renderer's share of finding what changed, of drawing, and why it was
/// drawn again. Counted only while the profiler is on.
DEKI_RENDERING_API void AddObjectMeasure(const RendererComponent* renderer, uint64_t us);
DEKI_RENDERING_API void AddObjectDraw(const RendererComponent* renderer, uint64_t contentUs, uint64_t blitUs,
                                      uint64_t pixels);
DEKI_RENDERING_API void AddObjectRedraw(const RendererComponent* renderer, Redraw why);
/// A render pass's Execute on one object.
DEKI_RENDERING_API void AddObjectPass(const void* pass, const char* passName, const Deki::Object* object, uint64_t us);

/// Every object measured since the last call, most expensive first (finding
/// changes, content and drawing together); then starts again. Over the
/// frames Take counts: call it before Take.
DEKI_RENDERING_API std::vector<ObjectEntry> TakeObjects();

}  // namespace RenderProfiler
}  // namespace DekiRendering
