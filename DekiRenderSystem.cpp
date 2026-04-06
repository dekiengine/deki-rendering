#include "DekiRenderSystem.h"
#include "DekiRenderer.h"
#include "DekiEngine.h"
#include "PrefabSystem.h"
#include "providers/DekiMemoryProvider.h"
#include "providers/DekiDisplayProvider.h"
#include "CameraComponent.h"
#include "DekiObject.h"
#include "Prefab.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>

DekiRenderSystem::DekiRenderSystem()
: render_buffer(nullptr)
, screen_width(0)
, screen_height(0)
, color_format(DekiColorFormat::RGB565)
{
}

DekiRenderSystem::~DekiRenderSystem()
{
    if (render_buffer && m_OwnsBuffer)
    {
        DekiMemoryProvider::Free(render_buffer, "RenderSystem-framebuffer");
    }
    render_buffer = nullptr;
}

bool DekiRenderSystem::Setup(int32_t width, int32_t height, DekiColorFormat format)
{
    // Clean up existing buffers if any
    if (render_buffer && m_OwnsBuffer)
    {
        DekiMemoryProvider::Free(render_buffer, "RenderSystem-framebuffer");
    }
    render_buffer = nullptr;
    m_OwnsBuffer = true;

    int bytes_per_pixel = GetBytesPerPixel(format);
    size_t buffer_size = width * height * bytes_per_pixel;

    // Try display-provided internal RAM buffer first (avoids PSRAM→internal memcpy in Present)
    IDekiDisplay* display = DekiDisplayProvider::GetDisplay();
    if (display)
    {
        int32_t dw = 0, dh = 0;
        uint8_t* directBuf = display->GetRenderBuffer(&dw, &dh);
        if (directBuf && dw == width && dh == height)
        {
            render_buffer = directBuf;
            m_OwnsBuffer = false;
            screen_width = width;
            screen_height = height;
            color_format = format;
            m_IsFirstRender = true;
            return true;
        }
    }

    // Fallback: allocate in PSRAM
    render_buffer = (uint8_t*)DekiMemoryProvider::Allocate(buffer_size, true, "DekiRenderSystem::Setup-framebuffer");
    if (!render_buffer)
    {
        return false;
    }

    screen_width = width;
    screen_height = height;
    color_format = format;

    // Reset first render flag and camera cache
    m_IsFirstRender = true;
    m_CachedCamera = nullptr;
    m_CachedCameraPrefab = nullptr;

    return true;
}

void DekiRenderSystem::Render(Prefab* current_prefab)
{
    if (!current_prefab || !render_buffer || !m_Renderer)
    {
        return;
    }

    // Use cached camera when prefab hasn't changed; re-search on first frame or prefab switch
    if (m_CachedCameraPrefab != current_prefab)
    {
        m_CachedCamera = nullptr;
        m_CachedCameraPrefab = current_prefab;
    }

    if (!m_CachedCamera)
    {
        // Search recursively through prefab objects
        std::function<CameraComponent*(DekiObject*)> findCamera = [&](DekiObject* obj) -> CameraComponent* {
            CameraComponent* cam = obj->GetComponent<CameraComponent>();
            if (cam) return cam;
            for (auto* child : obj->GetChildren())
            {
                cam = findCamera(child);
                if (cam) return cam;
            }
            return nullptr;
        };
        for (DekiObject* obj : current_prefab->GetObjects())
        {
            m_CachedCamera = findCamera(obj);
            if (m_CachedCamera) break;
        }

        // Fall back to Persistent objects
        if (!m_CachedCamera)
        {
            const auto& persistentObjects = DekiEngine::GetInstance().GetPrefabSystem().GetPersistentObjects();
            for (DekiObject* obj : persistentObjects)
            {
                m_CachedCamera = obj->GetComponent<CameraComponent>();
                if (m_CachedCamera) break;
            }
        }
    }

    // No camera = nothing to render
    if (!m_CachedCamera)
    {
        return;
    }

    CameraComponent* camera = m_CachedCamera;

    // Always clear the entire buffer before rendering
    ClearBuffer(camera->clear_color);

    // Delegate to the active renderer
    RenderContext ctx{camera, render_buffer, screen_width, screen_height, color_format};
    m_Renderer->Render(current_prefab, ctx);
}

void DekiRenderSystem::RenderToBuffer(Prefab* prefab, ICamera* camera,
                                       uint8_t* buffer, int32_t width, int32_t height,
                                       DekiColorFormat format)
{
    RenderToBufferStatic(prefab, camera, buffer, width, height, format);
}

void DekiRenderSystem::RenderToBufferStatic(Prefab* prefab, ICamera* camera,
                                             uint8_t* buffer, int32_t width, int32_t height,
                                             DekiColorFormat format)
{
    if (!prefab || !camera || !buffer)
        return;

    // Get the renderer from the engine's render system
    DekiRenderer* renderer = DekiEngine::GetInstance().GetRenderSystem()->GetRenderer();
    if (!renderer)
        return;

    // RenderContext uses CameraComponent* internally — safe cast since
    // the rendering module owns CameraComponent and knows the concrete type
    RenderContext ctx{static_cast<CameraComponent*>(camera), buffer, width, height, format};
    renderer->Render(prefab, ctx);
}

void DekiRenderSystem::ClearBuffer(uint8_t r, uint8_t g, uint8_t b)
{
    if (!render_buffer) return;
    size_t pixel_count = screen_width * screen_height;
    switch (color_format)
    {
        case DekiColorFormat::RGB565:
        {
            uint16_t rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            uint32_t pattern = (rgb565 << 16) | rgb565;
            uint32_t* buf32 = (uint32_t*)render_buffer;
            size_t count32 = pixel_count / 2;
            for (size_t i = 0; i < count32; i++)
            {
                buf32[i] = pattern;
            }
            if (pixel_count & 1)
            {
                ((uint16_t*)render_buffer)[pixel_count - 1] = rgb565;
            }
            break;
        }
        case DekiColorFormat::RGB888:
        {
            for (size_t i = 0; i < pixel_count; i++)
            {
                size_t index = i * 3;
                render_buffer[index] = r;
                render_buffer[index + 1] = g;
                render_buffer[index + 2] = b;
            }
            break;
        }
        case DekiColorFormat::ARGB8888:
        {
            uint32_t argb8888 = (0xFF << 24) | (r << 16) | (g << 8) | b;
            uint32_t* buffer32 = (uint32_t*)render_buffer;
            for (size_t i = 0; i < pixel_count; i++)
            {
                buffer32[i] = argb8888;
            }
            break;
        }
    }
}

void DekiRenderSystem::ClearBuffer(const deki::Color& color)
{
    ClearBuffer(color.r, color.g, color.b);
}

DEKI_FAST_ATTR void DekiRenderSystem::GetPixel(int32_t x, int32_t y, uint8_t* r, uint8_t* g, uint8_t* b) const
{
    if (!render_buffer || !r || !g || !b)
    {
        if (r) *r = 0;
        if (g) *g = 0;
        if (b) *b = 0;
        return;
    }

    // Bounds check
    if (x < 0 || x >= screen_width || y < 0 || y >= screen_height)
    {
        *r = *g = *b = 0;
        return;
    }

    // Get pixel from render buffer based on format
    switch (color_format)
    {
        case DekiColorFormat::RGB565:
        {
            size_t pixel_index = (y * screen_width + x) * 2;
            uint16_t pixel = *((uint16_t*)(render_buffer + pixel_index));
            *r = ((pixel >> 11) & 0x1F) << 3;  // 5 bits -> 8 bits
            *g = ((pixel >> 5) & 0x3F) << 2;  // 6 bits -> 8 bits
            *b = (pixel & 0x1F) << 3;  // 5 bits -> 8 bits
            break;
        }
        case DekiColorFormat::RGB888:
        {
            size_t pixel_index = (y * screen_width + x) * 3;
            *r = render_buffer[pixel_index];
            *g = render_buffer[pixel_index + 1];
            *b = render_buffer[pixel_index + 2];
            break;
        }
        case DekiColorFormat::ARGB8888:
        {
            size_t pixel_index = (y * screen_width + x) * 4;
            uint32_t pixel = *((uint32_t*)(render_buffer + pixel_index));
            *r = (pixel >> 16) & 0xFF;
            *g = (pixel >> 8) & 0xFF;
            *b = pixel & 0xFF;
            break;
        }
    }
}

DEKI_FAST_ATTR deki::Color DekiRenderSystem::GetPixel(int32_t x, int32_t y) const
{
    uint8_t r, g, b;
    GetPixel(x, y, &r, &g, &b);
    return deki::Color(r, g, b);
}

int DekiRenderSystem::GetBytesPerPixel(DekiColorFormat format)
{
    switch (format)
    {
        case DekiColorFormat::RGB565:
            return 2;
        case DekiColorFormat::RGB888:
            return 3;
        case DekiColorFormat::ARGB8888:
            return 4;
        default:
            return 2;
    }
}

