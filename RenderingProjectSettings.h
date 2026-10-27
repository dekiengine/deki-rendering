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
/// frameMode and bandRows choose between whole frames and strips
/// (DekiRenderSystem::Setup); redrawChanges draws only what changed.
DEKI_CATEGORY("Settings")
DEKI_SETTINGS_SECTION("Rendering")
class RenderingProjectSettings : public Deki::Component
{
public:
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

    DEKI_TOOLTIP("Redraw only what changed: each frame compares what every object draws with the last frame and "
                 "draws and sends only the parts of the screen that differ. A frame where nothing moved costs almost "
                 "nothing. When most of the screen changes (a scrolling camera, a full-screen animation) that frame "
                 "is drawn whole as usual. A render pass that cannot say where it draws, or a camera that does not "
                 "clear, makes every frame whole.")
    DEKI_EXPORT
    bool redrawChanges = false;
};

}  // namespace DekiRendering
