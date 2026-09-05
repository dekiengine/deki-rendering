#pragma once
#include <cstdint>
#include <vector>

#include "DekiEngine.h"
#include "Color.h"
#include "DirtyRegion.h"
#include "providers/IDekiRenderSystem.h"


// Forward declarations
namespace Deki { class Object; }
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
    // Display whose GetRenderBuffer() was last offered to us; adoption is
    // attempted once per display so Render() does not re-query when owning.
    class Deki::IDisplay* m_AdoptionCheckedDisplay = nullptr;

    // Active renderer (non-owning — caller manages lifetime)
    DekiRenderer* m_Renderer = nullptr;

    // ---- dirty-rect present (RenderingProjectSettings::dirtyTileTracking) ----
    // With tracking on, a frame clears only what the previous frame on the
    // same buffer drew, and the present covers this frame's draws plus the
    // previous frame's (the display still shows those; they are cleared or
    // overdrawn now). One history entry per buffer pointer handles displays
    // that hand out alternating buffers. Anything the bookkeeping cannot
    // vouch for is a full clear and a full present.
    bool m_TrackDirty = false;
    int32_t m_DirtyAlign = 32;  // rectangle alignment (px), a policy not a limit
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
    /// Switch to the display's internal buffer when it exists and matches our
    /// size. Returns true when the render buffer now points at the display's.
    bool TryAdoptDisplayBuffer();
    void Render(Deki::Scene* current_scene) override;
    void ClearBuffer(uint8_t r, uint8_t g, uint8_t b);
    void ClearBuffer(const Deki::Color& color);
    /// Fill [x, x+w) x [y, y+h) of the framebuffer (clipped) with a colour.
    void ClearRect(int32_t x, int32_t y, int32_t w, int32_t h, uint8_t r, uint8_t g, uint8_t b);

    // Dirty-rect present. Setup() reads the project setting; hosts and tests
    // can override it here (resets the history, so the next frame is full).
    void SetDirtyTracking(bool enabled, int32_t alignment);
    bool IsDirtyTracking() const { return m_TrackDirty; }
    void MarkAllDirty() override { m_ForceFull = true; }
    const Deki::Rect* GetPresentRects(int32_t* count) const override;

    // Renderer management
    void SetRenderer(DekiRenderer* renderer) override { m_Renderer = renderer; }
    DekiRenderer* GetRenderer() const override { return m_Renderer; }

    // Access methods for external systems (like HAL)
    const uint8_t* GetFrameBuffer() const override { return m_RenderBuffer; }
    int32_t GetScreenWidth() const override { return m_ScreenWidth; }
    int32_t GetScreenHeight() const override { return m_ScreenHeight; }
    Deki::ColorFormat GetColorFormat() const override { return m_ColorFormat; }

    // Pixel operations (optimized for fast execution)
    void GetPixel(int32_t x, int32_t y, uint8_t* r, uint8_t* g, uint8_t* b) const;
    Deki::Color GetPixel(int32_t x, int32_t y) const;

    int GetBytesPerPixel(Deki::ColorFormat format);

    // Deki::IRenderSystem interface — delegates to the static implementation
    void RenderToBuffer(Deki::Scene* scene, Deki::ICamera* camera,
                        uint8_t* buffer, int32_t width, int32_t height,
                        Deki::ColorFormat format) override;

    // Static render function (the actual implementation)
    static void RenderToBufferStatic(Deki::Scene* scene, Deki::ICamera* camera,
                                     uint8_t* buffer, int32_t width, int32_t height,
                                     Deki::ColorFormat format);
};
