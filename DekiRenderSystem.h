#pragma once
#include <cstdint>
#include <vector>

#include <deki/Engine.h>
#include <deki/Color.h>
#include "DirtyRegion.h"
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
    bool m_HaveClearColor = false;
    DirtyRegion m_DrawnScratch;
    DirtyRegion m_PresentScratch;
    std::vector<Deki::Rect> m_PresentRects;
    int32_t m_PresentCount = -1;

    BufferHistory& HistoryFor(const uint8_t* buffer);
    void ResetDirtyHistory();

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
    const Deki::Rect* GetPresentRects(int32_t* count) const override;

    void SetRenderer(DekiRenderer* renderer) override { m_Renderer = renderer; }
    DekiRenderer* GetRenderer() const override { return m_Renderer; }

    // For external systems such as the HAL.
    const uint8_t* GetFrameBuffer() const override { return m_RenderBuffer; }
    int32_t GetScreenWidth() const override { return m_ScreenWidth; }
    int32_t GetScreenHeight() const override { return m_ScreenHeight; }
    Deki::ColorFormat GetColorFormat() const override { return m_ColorFormat; }

    void GetPixel(int32_t x, int32_t y, uint8_t* r, uint8_t* g, uint8_t* b) const;
    Deki::Color GetPixel(int32_t x, int32_t y) const;

    int GetBytesPerPixel(Deki::ColorFormat format);

    // Deki::IRenderSystem: calls RenderToBufferStatic.
    void RenderToBuffer(Deki::Scene* scene, Deki::ICamera* camera, uint8_t* buffer, int32_t width, int32_t height,
                        Deki::ColorFormat format) override;

    // Renders `scene` through `camera` into `buffer` with the engine's active
    // renderer.
    static void RenderToBufferStatic(Deki::Scene* scene, Deki::ICamera* camera, uint8_t* buffer, int32_t width,
                                     int32_t height, Deki::ColorFormat format);
};

}  // namespace DekiRendering
