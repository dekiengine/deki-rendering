#pragma once
#include <cstdint>

#include "DekiEngine.h"
#include "Color.h"
#include "providers/IDekiRenderSystem.h"


// Forward declarations
class DekiObject;
class CameraComponent;
class DekiRenderer;

class DekiRenderSystem : public IDekiRenderSystem
{
   private:
    uint8_t* render_buffer;
    int32_t screen_width;
    int32_t screen_height;
    DekiColorFormat color_format;
    bool m_IsFirstRender = true;
    bool m_OwnsBuffer = true;

    // Active renderer (non-owning — caller manages lifetime)
    DekiRenderer* m_Renderer = nullptr;

    // Cached camera (searched once, invalidated on prefab change)
    CameraComponent* m_CachedCamera = nullptr;
    Prefab* m_CachedCameraPrefab = nullptr;

   public:
    DekiRenderSystem();
    ~DekiRenderSystem() override;
    bool Setup(int32_t width, int32_t height, DekiColorFormat format) override;
    void Render(Prefab* current_prefab) override;
    void ClearBuffer(uint8_t r, uint8_t g, uint8_t b);
    void ClearBuffer(const deki::Color& color);

    // Renderer management
    void SetRenderer(DekiRenderer* renderer) override { m_Renderer = renderer; }
    DekiRenderer* GetRenderer() const override { return m_Renderer; }

    // Access methods for external systems (like HAL)
    const uint8_t* GetFrameBuffer() const override { return render_buffer; }
    int32_t GetScreenWidth() const override { return screen_width; }
    int32_t GetScreenHeight() const override { return screen_height; }
    DekiColorFormat GetColorFormat() const override { return color_format; }

    // Pixel operations (optimized for fast execution)
    void GetPixel(int32_t x, int32_t y, uint8_t* r, uint8_t* g, uint8_t* b) const;
    deki::Color GetPixel(int32_t x, int32_t y) const;

    int GetBytesPerPixel(DekiColorFormat format);

    // IDekiRenderSystem interface — delegates to the static implementation
    void RenderToBuffer(Prefab* prefab, ICamera* camera,
                        uint8_t* buffer, int32_t width, int32_t height,
                        DekiColorFormat format) override;

    // Static render function (the actual implementation)
    static void RenderToBufferStatic(Prefab* prefab, ICamera* camera,
                                     uint8_t* buffer, int32_t width, int32_t height,
                                     DekiColorFormat format);
};
