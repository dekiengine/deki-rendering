#pragma once

#include <cstdint>
#include <vector>

#include "DekiRenderingPackage.h"

namespace DekiRendering
{

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
DEKI_RENDERING_API void AddBlit(uint32_t typeId, uint64_t us, uint64_t pixels);
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

}  // namespace RenderProfiler
}  // namespace DekiRendering
