#pragma once

#include <cstdint>
#include "DekiEngine.h"
#include "reflection/DekiProperty.h"

/**
 * @brief Project-wide rendering tradeoff toggles.
 *
 * Lives in the editor's Project Settings panel under the "Rendering" section.
 * dirtyTileTracking / dirtyTileSize drive DekiRenderSystem's dirty-rect
 * present (the field names predate the implementation and are the scene
 * format, so they stay). halfWidthFramebuffer and interlaced60hz have no
 * implementation yet; DekiRenderSystem::Setup logs when they are set.
 */
class RenderingProjectSettings : public DekiComponent
{
public:
    DEKI_COMPONENT(RenderingProjectSettings, DekiComponent, "Settings",
                   "f8a3c891-9b4d-4e2a-9f81-3c5b2d8e4a17", "")
    DEKI_PROJECT_SETTINGS_SECTION("Rendering")

    DEKI_TOOLTIP("Render at half horizontal resolution and double-up at present time. Halves blit cost and framebuffer memory but pixel art looks 2:1-stretched horizontally.")
    DEKI_EXPORT
    bool halfWidthFramebuffer = false;

    DEKI_TOOLTIP("Render odd scanlines one frame, even the next, at 60Hz. Halves per-frame work but produces visible combing on vertical motion.")
    DEKI_EXPORT
    bool interlaced60hz = false;

    DEKI_TOOLTIP("Dirty-rect tracking: record the rectangles each frame draws, clear only those next frame and push only what changed (this frame's and last frame's rectangles) to the display. Big win for mostly-static scenes; small bookkeeping cost for fully-animating ones. Displays that cannot present partial frames still receive whole frames.")
    DEKI_EXPORT
    bool dirtyTileTracking = false;

    DEKI_TOOLTIP("Alignment of dirty rectangles in pixels: each rectangle is rounded out to multiples of this. Larger = fewer, bigger pushes and a small movement reuses the same rectangle; smaller = tighter rectangles.")
    DEKI_RANGE(1, 256)
    DEKI_VISIBLE_WHEN(dirtyTileTracking, 1)
    DEKI_EXPORT
    int32_t dirtyTileSize = 32;
};

#include "generated/RenderingProjectSettings.gen.h"
