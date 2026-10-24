#pragma once

#include <cstdint>
#include <deki/Engine.h>
#include <deki/reflection/Property.h>

namespace DekiRendering
{

/// How each frame reaches the screen; see RenderingProjectSettings::frameMode.
enum class FrameMode : uint8_t
{
    Auto = 0,
    WholeFrames = 1,
    Strips = 2,
};

/// Project-wide rendering trade-offs, in the "Rendering" section of the
/// editor's Project Settings panel.
///
/// dirtyTileTracking and dirtyTileSize drive DekiRenderSystem's dirty-rect
/// present. The feature tracks rectangles, not tiles, but the field names are
/// part of the saved format and must stay. frameMode and bandRows choose
/// between whole frames and strips (DekiRenderSystem::Setup).
DEKI_CATEGORY("Settings")
DEKI_SETTINGS_SECTION("Rendering")
class RenderingProjectSettings : public Deki::Component
{
public:
    DEKI_TOOLTIP("Dirty-rect tracking: record the rectangles each frame draws, clear only those next frame and push "
                 "only what changed (this frame's and last frame's rectangles) to the display. Big win for "
                 "mostly-static scenes; small bookkeeping cost for fully-animating ones. Displays that cannot present "
                 "partial frames still receive whole frames.")
    DEKI_EXPORT
    bool dirtyTileTracking = false;

    DEKI_TOOLTIP("Alignment of dirty rectangles in pixels: each rectangle is rounded out to multiples of this. Larger "
                 "= fewer, bigger pushes and a small movement reuses the same rectangle; smaller = tighter rectangles.")
    DEKI_RANGE(1, 256)
    DEKI_VISIBLE_WHEN(dirtyTileTracking, 1)
    DEKI_EXPORT
    int32_t dirtyTileSize = 32;

    DEKI_TOOLTIP("How each frame reaches the screen. Auto (the default) chooses on the device when the game starts: "
                 "whole frames when the framebuffer fits in internal RAM with room to spare, strips when it does not "
                 "(a 536x240 screen at 2 bytes a pixel is 257 KB, more than the free internal RAM of an ESP32-S3). "
                 "Whole frames draws each frame once, in one buffer, and is the faster choice whenever that buffer "
                 "is in internal RAM. Strips draws the frame a strip of rows at a time into a small buffer in "
                 "internal RAM and sends each strip while the next is drawn, which is much faster than drawing a "
                 "whole frame in PSRAM. Both give the same pixels and keep every feature. The boot log and "
                 "DEKI:STATS say which one a board uses and why. The editor always draws whole frames.")
    DEKI_EXPORT
    FrameMode frameMode = FrameMode::Auto;

    DEKI_TOOLTIP("Rows in each strip, when frames are drawn in strips. 40 suits most screens. Taller strips walk the "
                 "scene fewer times a frame but need more internal RAM: a strip, and two copies of it for sending, "
                 "of the screen's width times this many rows at 2 bytes a pixel.")
    DEKI_RANGE(2, 256)
    DEKI_EXPORT
    int32_t bandRows = 40;
};

}  // namespace DekiRendering
