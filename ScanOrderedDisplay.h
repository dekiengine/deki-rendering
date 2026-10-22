#pragma once

#include <cstdint>

#include <deki/Engine.h>  // ColorFormat
#include <deki/providers/IDisplay.h>
#include <deki/providers/Rect.h>

#include "DekiRenderingPackage.h"  // DEKI_RENDERING_API

namespace DekiRendering
{

/// The order a panel refreshes its pixels in, as the screen sees it. A panel
/// refreshes its own rows one after another; shown turned, its rows are the
/// screen's columns.
enum class ScanOrder : uint8_t
{
    RowsDown,      // top row first: every panel shown the way it is built
    RowsUp,        // bottom row first: a panel shown upside down
    ColumnsRight,  // left column first: a panel shown turned
    ColumnsLeft,   // right column first: a panel shown turned the other way
};

/// One strip of a frame for PresentStrip.
struct ScanStrip
{
    /// The whole strip's pixels: its top-left one first, its rows `stride`
    /// pixels apart.
    const uint8_t* pixels = nullptr;
    int32_t stride = 0;
    /// Where the strip is on the screen (half-open).
    Deki::Rect strip;
    /// What changed, inside `strip`: what must reach the panel. A display may
    /// send more of the strip than this, never less.
    Deki::Rect changed;
    Deki::ColorFormat format = Deki::ColorFormat::RGB565;
    /// The first strip sent this frame: a display that waits for its panel's
    /// refresh waits before this one.
    bool firstOfFrame = false;
};

/// A display that wants frames drawn in strips cut along its panel's
/// refresh, so that each strip reaches the panel before the refresh does and
/// the panel never shows part of one frame and part of the next.
///
/// A display offers it with RegisterScanOrderedDisplay. While it does,
/// DekiRenderSystem cuts strips along GetScanOrder() in the order the panel
/// refreshes, and sends each one with PresentStrip instead of
/// IDisplay::PresentRegions. This lives here rather than on Deki::IDisplay so
/// that the engine's interface, which every package is built against, does
/// not change.
class IScanOrderedDisplay
{
public:
    virtual ScanOrder GetScanOrder() const = 0;

    /// Sends what changed of one strip. Strips arrive in the scan order;
    /// those with nothing changed are not sent at all.
    virtual void PresentStrip(const ScanStrip& strip) = 0;

protected:
    ~IScanOrderedDisplay() = default;
};

/// Offers `scan` for `display`, or withdraws it (`scan` null). One display at
/// a time; a display withdraws itself before it goes.
DEKI_RENDERING_API void RegisterScanOrderedDisplay(Deki::IDisplay* display, IScanOrderedDisplay* scan);

/// What `display` offers, or null.
DEKI_RENDERING_API IScanOrderedDisplay* FindScanOrderedDisplay(const Deki::IDisplay* display);

}  // namespace DekiRendering
