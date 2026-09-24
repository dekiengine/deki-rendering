#pragma once

#include <deki/Engine.h>

// Sets the project's design area (and screen fit, pixel perfect, art density)
// in EngineSettings::Global() for one test, and restores the previous values.
// A camera at zoom 1 maps a W x H px target at `ppm` when the design area is
// W/ppm x H/ppm meters, which is how the tests keep their pixel expectations.
struct ScopedDesignArea
{
    Deki::EngineSettings saved;

    ScopedDesignArea(float widthMeters, float heightMeters,
                     Deki::ScreenFit fit = Deki::ScreenFit::ShowAll,
                     bool pixelPerfect = false, float ppm = 16.0f)
        : saved(Deki::EngineSettings::Global())
    {
        Deki::EngineSettings& g = Deki::EngineSettings::Global();
        g.designWidth = widthMeters;
        g.designHeight = heightMeters;
        g.screenFit = fit;
        g.pixelPerfect = pixelPerfect;
        g.pixelsPerMeter = ppm;
    }
    ~ScopedDesignArea() { Deki::EngineSettings::Global() = saved; }

    ScopedDesignArea(const ScopedDesignArea&) = delete;
    ScopedDesignArea& operator=(const ScopedDesignArea&) = delete;
};
