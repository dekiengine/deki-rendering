#include "ScanOrderedDisplay.h"

namespace DekiRendering
{

namespace
{
// The display that offers a scan order, and what it offers. One at a time:
// there is one display.
const Deki::IDisplay* s_ScanOwner = nullptr;
IScanOrderedDisplay* s_ScanDisplay = nullptr;
}  // namespace

void RegisterScanOrderedDisplay(Deki::IDisplay* display, IScanOrderedDisplay* scan)
{
    if (scan)
    {
        s_ScanOwner = display;
        s_ScanDisplay = scan;
    }
    else if (s_ScanOwner == display)
    {
        s_ScanOwner = nullptr;
        s_ScanDisplay = nullptr;
    }
}

IScanOrderedDisplay* FindScanOrderedDisplay(const Deki::IDisplay* display)
{
    return display && display == s_ScanOwner ? s_ScanDisplay : nullptr;
}

}  // namespace DekiRendering
