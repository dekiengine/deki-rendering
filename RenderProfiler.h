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

/// Type id for the frame's clear, which belongs to no component.
constexpr uint32_t kClear = 1;

struct Entry
{
    uint32_t typeId = 0;   // the component's type, or kClear
    uint64_t contentUs = 0;
    uint64_t blitUs = 0;
    uint32_t draws = 0;
    uint64_t pixels = 0;  // source pixels blitted
};

DEKI_RENDERING_API void AddContent(uint32_t typeId, uint64_t us);
DEKI_RENDERING_API void AddBlit(uint32_t typeId, uint64_t us, uint64_t pixels);
DEKI_RENDERING_API void CountFrame();

/// What was measured since the last call, most expensive first, and over how
/// many frames; then starts again.
DEKI_RENDERING_API std::vector<Entry> Take(uint32_t& frames);

}  // namespace RenderProfiler
}  // namespace DekiRendering
