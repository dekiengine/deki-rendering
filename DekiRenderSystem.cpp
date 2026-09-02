#include "DekiRenderSystem.h"
#include "DekiRenderer.h"
#include "DekiEngine.h"
#include "DekiLogSystem.h"
#include "SceneSystem.h"
#include "providers/DekiMemory.h"
#include "providers/IDekiDisplay.h"
#include "CameraComponent.h"
#include "DekiObject.h"
#include "Scene.h"
#include "RenderingProjectSettings.h"
#include "reflection/SettingsRegistry.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

DekiRenderSystem::DekiRenderSystem()
: m_RenderBuffer(nullptr)
, m_ScreenWidth(0)
, m_ScreenHeight(0)
, m_ColorFormat(DekiColorFormat::RGB565)
{
}

DekiRenderSystem::~DekiRenderSystem()
{
    if (m_RenderBuffer && m_OwnsBuffer)
    {
        DekiMemory::FreeInternal(m_RenderBuffer);
    }
    m_RenderBuffer = nullptr;
}

bool DekiRenderSystem::Setup(int32_t width, int32_t height, DekiColorFormat format)
{
    // Read project-wide rendering settings. Backing implementations for the
    // perf toggles (half-width framebuffer, interlaced, dirty-tile tracking)
    // ship as separate follow-ups; until then we just log non-default values
    // so the user can verify the registry is being hydrated correctly.
    if (auto* rs = DekiSettingsRegistry::Instance().Get<RenderingProjectSettings>())
    {
        if (rs->halfWidthFramebuffer || rs->interlaced60hz || rs->dirtyTileTracking)
        {
            DEKI_LOG(LogLevel::Info,
                     "[Rendering] settings: half_width=%d interlaced=%d dirty_tracking=%d tile_size=%d (impls pending)",
                     (int)rs->halfWidthFramebuffer, (int)rs->interlaced60hz,
                     (int)rs->dirtyTileTracking, rs->dirtyTileSize);
        }
    }

    if (width <= 0 || height <= 0)
    {
        DEKI_LOG_ERROR("DekiRenderSystem::Setup: invalid size %dx%d", width, height);
        return false;
    }

    // Clean up existing buffers if any
    if (m_RenderBuffer && m_OwnsBuffer)
    {
        DekiMemory::FreeInternal(m_RenderBuffer);
    }
    m_RenderBuffer = nullptr;
    m_OwnsBuffer = true;
    m_AdoptionCheckedDisplay = nullptr;

    m_ScreenWidth = width;
    m_ScreenHeight = height;
    m_ColorFormat = format;
    m_CachedCamera = nullptr;
    m_CachedCameraScene = nullptr;

    // Prefer a display-provided internal RAM buffer (avoids a memcpy in Present).
    if (TryAdoptDisplayBuffer())
        return true;

    // No display yet, or its buffer does not match: own one. This used to
    // "defer allocation until a display is available" and return true with a
    // null buffer — but Render() only re-queried the display for non-owned
    // buffers, so the allocation never happened and nothing was ever drawn,
    // with no error. Now Setup either yields a usable buffer or says so.
    int bytes_per_pixel = GetBytesPerPixel(format);
    size_t buffer_size = (size_t)width * (size_t)height * (size_t)bytes_per_pixel;
    m_RenderBuffer = (uint8_t*)DekiMemory::AllocateInternal(buffer_size, "DekiRenderSystem::Setup-framebuffer");
    if (!m_RenderBuffer)
    {
        DEKI_LOG_ERROR("DekiRenderSystem::Setup: failed to allocate %zu-byte framebuffer (%dx%d)",
                       buffer_size, width, height);
        return false;
    }
    return true;
}

bool DekiRenderSystem::TryAdoptDisplayBuffer()
{
    IDekiDisplay* display = DekiEngine::GetInstance().GetDisplay();
    if (!display || display == m_AdoptionCheckedDisplay)
        return false;
    m_AdoptionCheckedDisplay = display;

    int32_t dw = 0, dh = 0;
    uint8_t* directBuf = display->GetRenderBuffer(&dw, &dh);
    if (!directBuf || dw != m_ScreenWidth || dh != m_ScreenHeight)
        return false;

    if (m_RenderBuffer && m_OwnsBuffer)
        DekiMemory::FreeInternal(m_RenderBuffer);
    m_RenderBuffer = directBuf;
    m_OwnsBuffer = false;
    return true;
}

void DekiRenderSystem::Render(Scene* current_scene)
{
    if (!current_scene || !m_Renderer)
    {
        return;
    }

    // A display registered after Setup() may offer a direct buffer: adopt it
    // once. Otherwise re-query the display buffer each frame for double-buffer
    // support (render_index alternates in Present, so the pointer changes).
    if (m_OwnsBuffer)
    {
        TryAdoptDisplayBuffer();
    }
    else
    {
        IDekiDisplay* display = DekiEngine::GetInstance().GetDisplay();
        if (display)
        {
            int32_t dw = 0, dh = 0;
            uint8_t* buf = display->GetRenderBuffer(&dw, &dh);
            if (buf)
                m_RenderBuffer = buf;
        }
    }

    if (!m_RenderBuffer)
    {
        return;
    }

    // Use cached camera when scene hasn't changed; re-search on first frame or scene switch
    if (m_CachedCameraScene != current_scene)
    {
        m_CachedCamera = nullptr;
        m_CachedCameraScene = current_scene;
    }

    if (!m_CachedCamera)
    {
        // Search recursively through scene objects
        for (DekiObject* obj : current_scene->GetObjects())
        {
            DekiObject* holder = FindInSubtree(obj, [](DekiObject* o)
                                               { return o->GetComponent<CameraComponent>() != nullptr; });
            if (holder)
            {
                m_CachedCamera = holder->GetComponent<CameraComponent>();
                break;
            }
        }

        // Fall back to Persistent objects
        if (!m_CachedCamera)
        {
            const auto& persistentObjects = DekiEngine::GetInstance().GetSceneSystem().GetPersistentObjects();
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
    ClearBuffer(camera->clearColor);

    // Delegate to the active renderer
    RenderContext ctx{camera, m_RenderBuffer, m_ScreenWidth, m_ScreenHeight, m_ColorFormat};
    m_Renderer->Render(current_scene, ctx);
}

void DekiRenderSystem::RenderToBuffer(Scene* scene, ICamera* camera,
                                       uint8_t* buffer, int32_t width, int32_t height,
                                       DekiColorFormat format)
{
    RenderToBufferStatic(scene, camera, buffer, width, height, format);
}

void DekiRenderSystem::RenderToBufferStatic(Scene* scene, ICamera* camera,
                                             uint8_t* buffer, int32_t width, int32_t height,
                                             DekiColorFormat format)
{
    if (!scene || !camera || !buffer)
        return;

    // Get the renderer from the engine's render system
    DekiRenderer* renderer = DekiEngine::GetInstance().GetRenderSystem()->GetRenderer();
    if (!renderer)
        return;

    // RenderContext uses CameraComponent* internally — safe cast since
    // the rendering package owns CameraComponent and knows the concrete type
    RenderContext ctx{static_cast<CameraComponent*>(camera), buffer, width, height, format};
    renderer->Render(scene, ctx);
}

void DekiRenderSystem::ClearBuffer(uint8_t r, uint8_t g, uint8_t b)
{
    if (!m_RenderBuffer) return;
    size_t pixel_count = m_ScreenWidth * m_ScreenHeight;
    switch (m_ColorFormat)
    {
        case DekiColorFormat::RGB565:
        {
            uint16_t rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            // memset works when both bytes of the RGB565 value are the same
            if ((rgb565 >> 8) == (rgb565 & 0xFF))
            {
                memset(m_RenderBuffer, rgb565 & 0xFF, pixel_count * 2);
            }
            else
            {
                // Fill using memcpy doubling -- faster than a loop
                uint32_t pattern = (rgb565 << 16) | rgb565;
                size_t total = pixel_count * 2;
                memcpy(m_RenderBuffer, &pattern, 4);
                for (size_t written = 4; written < total; written *= 2)
                {
                    size_t chunk = (written * 2 <= total) ? written : total - written;
                    memcpy(m_RenderBuffer + written, m_RenderBuffer, chunk);
                }
            }
            break;
        }
        case DekiColorFormat::RGB888:
        {
            for (size_t i = 0; i < pixel_count; i++)
            {
                size_t index = i * 3;
                m_RenderBuffer[index] = r;
                m_RenderBuffer[index + 1] = g;
                m_RenderBuffer[index + 2] = b;
            }
            break;
        }
        case DekiColorFormat::ARGB8888:
        {
            uint32_t argb8888 = (0xFF << 24) | (r << 16) | (g << 8) | b;
            uint32_t* buffer32 = (uint32_t*)m_RenderBuffer;
            for (size_t i = 0; i < pixel_count; i++)
            {
                buffer32[i] = argb8888;
            }
            break;
        }
        case DekiColorFormat::RGB565A8:
        {
            uint16_t rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            uint8_t lo = uint8_t(rgb565 & 0xFF);
            uint8_t hi = uint8_t((rgb565 >> 8) & 0xFF);
            for (size_t i = 0; i < pixel_count; i++)
            {
                size_t idx = i * 3;
                m_RenderBuffer[idx]     = lo;
                m_RenderBuffer[idx + 1] = hi;
                m_RenderBuffer[idx + 2] = 0xFF;  // opaque
            }
            break;
        }
    }
}

void DekiRenderSystem::ClearBuffer(const Deki::Color& color)
{
    ClearBuffer(color.r, color.g, color.b);
}

DEKI_FAST_ATTR void DekiRenderSystem::GetPixel(int32_t x, int32_t y, uint8_t* r, uint8_t* g, uint8_t* b) const
{
    if (!m_RenderBuffer || !r || !g || !b)
    {
        if (r) *r = 0;
        if (g) *g = 0;
        if (b) *b = 0;
        return;
    }

    // Bounds check
    if (x < 0 || x >= m_ScreenWidth || y < 0 || y >= m_ScreenHeight)
    {
        *r = *g = *b = 0;
        return;
    }

    // Get pixel from render buffer based on format
    switch (m_ColorFormat)
    {
        case DekiColorFormat::RGB565:
        {
            size_t pixel_index = (y * m_ScreenWidth + x) * 2;
            uint16_t pixel = *((uint16_t*)(m_RenderBuffer + pixel_index));
            *r = ((pixel >> 11) & 0x1F) << 3;  // 5 bits -> 8 bits
            *g = ((pixel >> 5) & 0x3F) << 2;  // 6 bits -> 8 bits
            *b = (pixel & 0x1F) << 3;  // 5 bits -> 8 bits
            break;
        }
        case DekiColorFormat::RGB888:
        {
            size_t pixel_index = (y * m_ScreenWidth + x) * 3;
            *r = m_RenderBuffer[pixel_index];
            *g = m_RenderBuffer[pixel_index + 1];
            *b = m_RenderBuffer[pixel_index + 2];
            break;
        }
        case DekiColorFormat::ARGB8888:
        {
            size_t pixel_index = (y * m_ScreenWidth + x) * 4;
            uint32_t pixel = *((uint32_t*)(m_RenderBuffer + pixel_index));
            *r = (pixel >> 16) & 0xFF;
            *g = (pixel >> 8) & 0xFF;
            *b = pixel & 0xFF;
            break;
        }
        case DekiColorFormat::RGB565A8:
        {
            size_t pixel_index = (y * m_ScreenWidth + x) * 3;
            uint16_t pixel = *((uint16_t*)(m_RenderBuffer + pixel_index));
            *r = ((pixel >> 11) & 0x1F) << 3;
            *g = ((pixel >> 5) & 0x3F) << 2;
            *b = (pixel & 0x1F) << 3;
            break;
        }
    }
}

DEKI_FAST_ATTR Deki::Color DekiRenderSystem::GetPixel(int32_t x, int32_t y) const
{
    uint8_t r, g, b;
    GetPixel(x, y, &r, &g, &b);
    return Deki::Color(r, g, b);
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
        case DekiColorFormat::RGB565A8:
            return 3;
    }
    return 2;
}

