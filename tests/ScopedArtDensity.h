#pragma once

#include <deki/Engine.h>

// Sets the project's art density (pixels per meter) in EngineSettings::Global()
// for one test, and restores the previous value. Cameras frame the world
// themselves; the art density only matters to pixel-perfect scaling.
struct ScopedArtDensity
{
    float saved;

    explicit ScopedArtDensity(float ppm = 16.0f) : saved(Deki::EngineSettings::Global().pixelsPerMeter)
    {
        Deki::EngineSettings::Global().pixelsPerMeter = ppm;
    }
    ~ScopedArtDensity() { Deki::EngineSettings::Global().pixelsPerMeter = saved; }

    ScopedArtDensity(const ScopedArtDensity&) = delete;
    ScopedArtDensity& operator=(const ScopedArtDensity&) = delete;
};
