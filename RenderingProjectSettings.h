#pragma once

#include <cstdint>
#include "DekiEngine.h"
#include "reflection/DekiProperty.h"

/**
 * @brief Project-wide rendering tradeoff toggles.
 *
 * Lives in the editor's Project Settings panel under the "Rendering" section.
 * Backing renderer implementations for each toggle ship as separate follow-up
 * work — declaring the field here gives the user a stable place to set the
 * preference, and DekiRenderSystem::Setup logs which non-default values are
 * active so it's clear what's expected to take effect once the impls land.
 */
class RenderingProjectSettings : public DekiComponent
{
public:
    DEKI_COMPONENT(RenderingProjectSettings, DekiComponent, "Settings",
                   "f8a3c891-9b4d-4e2a-9f81-3c5b2d8e4a17", "")
    DEKI_PROJECT_SETTINGS_SECTION("Rendering")

    DEKI_TOOLTIP("Render at half horizontal resolution and double-up at present time. Halves blit cost and framebuffer memory but pixel art looks 2:1-stretched horizontally.")
    DEKI_EXPORT
    bool half_width_framebuffer = false;

    DEKI_TOOLTIP("Render odd scanlines one frame, even the next, at 60Hz. Halves per-frame work but produces visible combing on vertical motion.")
    DEKI_EXPORT
    bool interlaced_60hz = false;

    DEKI_TOOLTIP("Track dirty tiles and only push changed tiles to the display. Big win for UI scenes; small cost for fully-animating ones. Requires display backend support for partial updates.")
    DEKI_EXPORT
    bool dirty_tile_tracking = false;

    DEKI_TOOLTIP("Pixel size of dirty-tracking tiles when dirty_tile_tracking is enabled. Smaller = finer-grained updates but more bookkeeping.")
    DEKI_RANGE(8, 64)
    DEKI_VISIBLE_WHEN(dirty_tile_tracking, 1)
    DEKI_EXPORT
    int32_t dirty_tile_size = 32;
};

#include "generated/RenderingProjectSettings.gen.h"
