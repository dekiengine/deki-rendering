#pragma once
#include <cstdint>
#include <vector>

#include <deki/Engine.h>
#include <deki/Color.h>
#include "DirtyRegion.h"
#include "RenderingProjectSettings.h"
#include "ScanOrderedDisplay.h"
#include <deki/providers/IRenderSystem.h>

namespace Deki
{
class Object;
}

namespace DekiRendering
{
class CameraComponent;
class DekiRenderer;

class DekiRenderSystem : public Deki::IRenderSystem
{
private:
    uint8_t* m_RenderBuffer;
    int32_t m_ScreenWidth;
    int32_t m_ScreenHeight;
    Deki::ColorFormat m_ColorFormat;
    bool m_OwnsBuffer = true;
    // The display whose buffer adoption was last tried. Adoption is tried once
    // per display, so Render() does not re-query while it owns its buffer.
    class Deki::IDisplay* m_AdoptionCheckedDisplay = nullptr;

    // Active renderer. Not owned; the caller manages its lifetime.
    DekiRenderer* m_Renderer = nullptr;

    // ---- dirty-rect present (RenderingProjectSettings::dirtyTileTracking) ----
    // With tracking on, a frame clears only what the previous frame on the
    // same buffer drew, and the present covers this frame's draws plus the
    // previous frame's (the display still shows those; they are cleared or
    // overdrawn now). One history entry per buffer pointer handles displays
    // that alternate buffers. Whenever the bookkeeping is unsure, the frame is
    // a full clear and a full present.
    bool m_TrackDirty = false;
    int32_t m_DirtyAlign = 32;  // rectangle alignment in px; a policy, not a limit
    struct BufferHistory
    {
        const uint8_t* buffer;
        DirtyRegion lastDrawn;  // what the frame rendered into this buffer drew
        bool valid;             // false until the buffer has been fully cleared once
    };
    std::vector<BufferHistory> m_History;
    DirtyRegion m_LastDrawn;  // the previous frame's draws, whatever buffer
    bool m_HaveLastDrawn = false;
    bool m_ForceFull = true;
    Deki::Color m_LastClearColor;
    bool m_LastClearEveryFrame = true;
    bool m_HaveClearColor = false;
    DirtyRegion m_DrawnScratch;
    DirtyRegion m_PresentScratch;
    std::vector<Deki::Rect> m_PresentRects;
    int32_t m_PresentCount = -1;

    BufferHistory& HistoryFor(const uint8_t* buffer);
    void ResetDirtyHistory();

    // ---- strip rendering (RenderingProjectSettings::bandRows) ----
    // With bandRows set there is no framebuffer: each frame is drawn a strip
    // of rows at a time into m_BandBuffer, in internal RAM, and each strip is
    // sent to the display before the next is drawn.
    //
    // A display that offers IScanOrderedDisplay has its strips cut along its
    // panel's refresh instead: strips of columns where the panel is shown
    // turned, in the order the panel refreshes them, each sent with
    // PresentStrip. bandRows is then the strip's width.
    //
    // Everything whole frames do, strips do too:
    // - dirty-rect tracking: every strip is drawn, but only the rows of it
    //   that changed (drawn this frame or the last) are sent, a strip with
    //   none is not sent at all, and the strip buffer is cleared only where
    //   the strip drawn into it before drew;
    // - a camera that does not clear: the frame is kept in m_WholeFrame,
    //   each strip read from it before drawing and written back after;
    // - screenshots: RequestFrameCapture() copies the next frame's strips
    //   into m_WholeFrame, which GetFrameBuffer() then returns;
    // - a renderer or pass that cannot draw in strips (DrawsInStrips): whole
    //   frames are drawn into m_WholeFrame while it cannot.
    int32_t m_BandRows = 0;       // a strip's rows, or its columns
    bool m_BandColumns = false;   // strips of columns
    bool m_BandReverse = false;   // the last strip first (the panel refreshes from there)
    int32_t m_BandAsked = 0;      // what SetBandRows was asked for, before rounding
    int32_t m_BandMaxPixels = 0;  // the scan-ordered display's GetMaxStripPixels when cut
    uint8_t* m_BandBuffer = nullptr;
    uint32_t m_ContentFrame = 0;
    bool m_BandWarned = false;
    // What the strip buffer holds besides the clear colour (in its own rows),
    // so the next strip clears only that. Invalid: clear all of it.
    // What each strip buffer holds from the strip last drawn in it (ours, or
    // one of those a display lends), so the next strip in it clears only that.
    struct HeldStrip
    {
        const uint8_t* buffer = nullptr;
        DirtyRegion held;
        bool valid = false;
        int32_t width = 0;  // the strip's width when `held` was recorded
    };
    HeldStrip m_Held[3];
    uint32_t m_HeldNext = 0;
    HeldStrip& HeldFor(const uint8_t* buffer);
    void ForgetHeld();
    DirtyRegion m_BandFrameDrawn;  // what this frame's strips drew, in view rows
    // A whole frame, in external memory where there is some, allocated only
    // while something needs it and freed the first frame nothing does.
    uint8_t* m_WholeFrame = nullptr;
    bool m_WholeFrameHolds = false;  // holds the frame last drawn
    bool m_WholeFrames = false;      // drawing whole frames into it (DrawsInStrips said no)
    bool m_WholeFrameWarned = false;
    uint32_t m_CaptureSeen = 0;
    bool AllocateBand();
    void FreeBand();
    void RenderBands(Deki::Scene* scene, CameraComponent* camera, bool capture);
    uint8_t* EnsureWholeFrame(const Deki::Color& clear);
    void FreeWholeFrame();
    bool TakeCaptureRequest();
    // True when the camera's clear colour or clearEveryFrame differs from
    // the last frame's (what was not drawn is then something else); notes it.
    bool ClearChanged(const CameraComponent& camera);

public:
    DekiRenderSystem();
    ~DekiRenderSystem() override;
    bool Setup(int32_t width, int32_t height, Deki::ColorFormat format) override;
    /// Switches to the display's own buffer when it has one of our size.
    /// Returns true when the render buffer now points at the display's.
    bool TryAdoptDisplayBuffer();
    void Render(Deki::Scene* currentScene) override;
    void ClearBuffer(uint8_t r, uint8_t g, uint8_t b);
    void ClearBuffer(const Deki::Color& color);
    /// Fills [x, x+w) x [y, y+h) of the framebuffer, clipped, with a colour.
    void ClearRect(int32_t x, int32_t y, int32_t w, int32_t h, uint8_t r, uint8_t g, uint8_t b);

    // Dirty-rect present. Setup() reads the project setting; hosts and tests
    // can override it here. Doing so resets the history, so the next frame is
    // full.
    void SetDirtyTracking(bool enabled, int32_t alignment);
    bool IsDirtyTracking() const { return m_TrackDirty; }
    void MarkAllDirty() override { m_ForceFull = true; }

    // Strip rendering. Setup() reads the project setting on a device (the
    // editor ignores it); hosts and tests can set it here, after Setup. 0
    // renders whole frames. Returns false when the strip cannot be allocated.
    // Strips run along the scan order the display offers
    // (FindScanOrderedDisplay), read here: rows, a whole number of pairs of
    // them, or columns, a whole number of eights of them (the ordered
    // dither's tile), so a strip never moves the dither pattern.
    bool SetBandRows(int32_t rows);
    int32_t GetBandRows() const { return m_BandRows; }
    bool IsBandColumns() const { return m_BandColumns; }

    /// Whole frames or strips, as Setup decides it from frameMode. Auto
    /// chooses whole frames when a framebuffer of `frameBytes` fits in
    /// `freeInternal` with kInternalHeadroom to spare for the game, and strips
    /// otherwise; a display that cannot take part of a frame always gets
    /// whole frames. `rows` 0: whole frames.
    struct FrameChoice
    {
        int32_t rows;
        enum class Why : uint8_t
        {
            Chosen,        // WholeFrames or Strips in the project settings
            Fits,          // Auto: the framebuffer fits in internal RAM
            DoesNotFit,    // Auto: it does not
            WholeDisplay,  // Auto: the display takes only whole frames
            Editor,        // the editor always draws whole frames
        } why;
    };
    static constexpr size_t kInternalHeadroom = 64 * 1024;
    static FrameChoice ChooseFrameMode(FrameMode mode, int32_t rows, size_t frameBytes, size_t freeInternal,
                                       bool displayTakesPartialFrames);
    /// The last Setup's choice and its reason, as one line for the log; and
    /// the strip height it chose (0: whole frames), for DEKI:STATS.
    static const char* LastFrameModeNote();
    static int32_t LastFrameModeRows();

private:
    static void NoteFrameChoice(const FrameChoice& choice, size_t frameBytes, size_t freeInternal);

public:
    /// True while strips are set but the renderer cannot draw in them, so
    /// whole frames are drawn instead.
    bool IsDrawingWholeFrames() const { return m_BandRows > 0 && m_WholeFrames; }
    const Deki::Rect* GetPresentRects(int32_t* count) const override;

    /// Asks every render system drawing in strips to also put its next frame
    /// together whole, for a screenshot: GetFrameBuffer() returns it after
    /// that frame, until the frame after. Costs one frame's copy into
    /// external memory, once. Whole frames need nothing: GetFrameBuffer()
    /// always has one. Safe from any task.
    static void RequestFrameCapture();

    void SetRenderer(DekiRenderer* renderer) override { m_Renderer = renderer; }
    DekiRenderer* GetRenderer() const override { return m_Renderer; }

    // For external systems such as the HAL. In strips there is a whole frame
    // only after RequestFrameCapture(), while the camera does not clear, or
    // while whole frames are drawn instead; otherwise null.
    const uint8_t* GetFrameBuffer() const override
    {
        if (m_BandRows > 0)
        {
            return m_WholeFrameHolds ? m_WholeFrame : nullptr;
        }
        return m_RenderBuffer;
    }
    int32_t GetScreenWidth() const override { return m_ScreenWidth; }
    int32_t GetScreenHeight() const override { return m_ScreenHeight; }
    Deki::ColorFormat GetColorFormat() const override { return m_ColorFormat; }

    void GetPixel(int32_t x, int32_t y, uint8_t* r, uint8_t* g, uint8_t* b) const;
    Deki::Color GetPixel(int32_t x, int32_t y) const;

    int GetBytesPerPixel(Deki::ColorFormat format) { return GetBytesPerPixelOf(format); }
    static int GetBytesPerPixelOf(Deki::ColorFormat format);

    // Deki::IRenderSystem: calls RenderToBufferStatic.
    void RenderToBuffer(Deki::Scene* scene, Deki::ICamera* camera, uint8_t* buffer, int32_t width, int32_t height,
                        Deki::ColorFormat format) override;

    // Renders `scene` through `camera` into `buffer` with the engine's active
    // renderer.
    static void RenderToBufferStatic(Deki::Scene* scene, Deki::ICamera* camera, uint8_t* buffer, int32_t width,
                                     int32_t height, Deki::ColorFormat format);
};

}  // namespace DekiRendering
