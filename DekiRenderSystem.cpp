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
#include "ProjectSettings.h"
#include "reflection/SettingsRegistry.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>

DekiRenderSystem::DekiRenderSystem()
: m_RenderBuffer(nullptr)
, m_ScreenWidth(0)
, m_ScreenHeight(0)
, m_ColorFormat(Deki::ColorFormat::RGB565)
{
}

DekiRenderSystem::~DekiRenderSystem()
{
    if (m_RenderBuffer && m_OwnsBuffer)
    {
        Deki::Memory::FreeInternal(m_RenderBuffer);
    }
    m_RenderBuffer = nullptr;
}

bool DekiRenderSystem::Setup(int32_t width, int32_t height, Deki::ColorFormat format)
{
    // Project-wide rendering settings. In the editor the registry holds the
    // hydrated instance; on device there is no registry, so the values come
    // straight out of the loaded dproject.bin. Half-width and interlaced have
    // no implementation yet and are only reported.
    m_TrackDirty = false;
    m_DirtyAlign = 32;
    bool halfWidth = false, interlaced = false;
    if (auto* rs = Deki::SettingsRegistry::Instance().Get<RenderingProjectSettings>())
    {
        m_TrackDirty = rs->dirtyTileTracking;
        m_DirtyAlign = rs->dirtyTileSize;
        halfWidth = rs->halfWidthFramebuffer;
        interlaced = rs->interlaced60hz;
    }
    else
    {
        // "Rendering" is RenderingProjectSettings' DEKI_PROJECT_SETTINGS_SECTION.
        bool b = false;
        int32_t a = 0;
        if (Deki::ProjectSettings::ReadPackageSettingBool("Rendering", "dirtyTileTracking", b)) m_TrackDirty = b;
        if (Deki::ProjectSettings::ReadPackageSettingInt32("Rendering", "dirtyTileSize", a)) m_DirtyAlign = a;
        if (Deki::ProjectSettings::ReadPackageSettingBool("Rendering", "halfWidthFramebuffer", b)) halfWidth = b;
        if (Deki::ProjectSettings::ReadPackageSettingBool("Rendering", "interlaced60hz", b)) interlaced = b;
    }
    if (m_DirtyAlign < 1) m_DirtyAlign = 1;
    if (halfWidth || interlaced)
    {
        DEKI_LOG(Deki::LogLevel::Info, "[Rendering] settings: half_width=%d interlaced=%d (no implementation yet)",
                 (int)halfWidth, (int)interlaced);
    }
    if (m_TrackDirty)
        DEKI_LOG_INTERNAL("[Rendering] dirty-rect tracking on, alignment %d px", m_DirtyAlign);
    ResetDirtyHistory();

    if (width <= 0 || height <= 0)
    {
        DEKI_LOG_ERROR("DekiRenderSystem::Setup: invalid size %dx%d", width, height);
        return false;
    }

    // Clean up existing buffers if any
    if (m_RenderBuffer && m_OwnsBuffer)
    {
        Deki::Memory::FreeInternal(m_RenderBuffer);
    }
    m_RenderBuffer = nullptr;
    m_OwnsBuffer = true;
    m_AdoptionCheckedDisplay = nullptr;

    m_ScreenWidth = width;
    m_ScreenHeight = height;
    m_ColorFormat = format;

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
    m_RenderBuffer = (uint8_t*)Deki::Memory::AllocateInternal(buffer_size, "DekiRenderSystem::Setup-framebuffer");
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
    Deki::IDisplay* display = Deki::Engine::GetInstance().GetDisplay();
    if (!display || display == m_AdoptionCheckedDisplay)
        return false;
    m_AdoptionCheckedDisplay = display;

    int32_t dw = 0, dh = 0;
    uint8_t* directBuf = display->GetRenderBuffer(&dw, &dh);
    if (!directBuf || dw != m_ScreenWidth || dh != m_ScreenHeight)
        return false;

    if (m_RenderBuffer && m_OwnsBuffer)
        Deki::Memory::FreeInternal(m_RenderBuffer);
    m_RenderBuffer = directBuf;
    m_OwnsBuffer = false;
    return true;
}

void DekiRenderSystem::Render(Deki::Scene* current_scene)
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
        Deki::IDisplay* display = Deki::Engine::GetInstance().GetDisplay();
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

    // Find the scene's camera. Every frame, not cached: the cache used to be
    // keyed on the Scene pointer, which a new scene at the same address (a
    // tool rendering scenes in a loop) or a CameraComponent removed at
    // runtime turned into a dangling component. The walk is a few hundred
    // component-list checks against a frame of blits.
    CameraComponent* camera = nullptr;
    for (Deki::Object* obj : current_scene->GetObjects())
    {
        Deki::Object* holder = FindInSubtree(obj, [](Deki::Object* o)
                                           { return o->GetComponent<CameraComponent>() != nullptr; });
        if (holder)
        {
            camera = holder->GetComponent<CameraComponent>();
            break;
        }
    }
    if (!camera)
    {
        // Fall back to Persistent objects
        const auto& persistentObjects = Deki::Engine::GetInstance().GetSceneSystem().GetPersistentObjects();
        for (Deki::Object* obj : persistentObjects)
        {
            camera = obj->GetComponent<CameraComponent>();
            if (camera) break;
        }
    }

    // No camera = nothing to render
    if (!camera)
    {
        return;
    }


    // ---- dirty-rect present -------------------------------------------------
    // Anything the bookkeeping cannot vouch for (first use of a buffer, a
    // size/format change, a clear-colour change, a frame the renderer could
    // not describe, MarkAllDirty) is a full clear and a full present, so
    // "off" and "unsure" both behave exactly as before.
    const bool tracking = m_TrackDirty;
    BufferHistory* hist = tracking ? &HistoryFor(m_RenderBuffer) : nullptr;
    bool full = !tracking || m_ForceFull || !hist->valid;
    const Deki::Color clear = camera->clearColor;
    if (!m_HaveClearColor || clear.r != m_LastClearColor.r || clear.g != m_LastClearColor.g ||
        clear.b != m_LastClearColor.b)
    {
        full = true;
        m_LastClearColor = clear;
        m_HaveClearColor = true;
    }

    // Clear before rendering, unless the camera says the scene paints every
    // pixel itself. With tracking, only what the last frame on this buffer
    // drew needs clearing.
    if (camera->clearEveryFrame)
    {
        if (full || hist->lastDrawn.IsFull())
            ClearBuffer(clear);
        else
            for (const Deki::Rect& r : hist->lastDrawn.Rects())
                ClearRect(r.left, r.top, r.Width(), r.Height(), clear.r, clear.g, clear.b);
    }

    // Delegate to the active renderer
    RenderContext ctx{camera, m_RenderBuffer, m_ScreenWidth, m_ScreenHeight, m_ColorFormat};
    ctx.trackDirty = tracking;
    m_Renderer->Render(current_scene, ctx);

    if (!tracking)
    {
        m_PresentCount = -1;
        return;
    }

    // What this frame drew, aligned so a small movement reuses its rectangle.
    const DirtyRegion* drawn = m_Renderer->GetLastFrameDirty();
    DirtyRegion& frame = m_DrawnScratch;
    if (drawn)
    {
        frame = *drawn;
        frame.Align(m_DirtyAlign);
    }
    else
    {
        frame.Reset(m_ScreenWidth, m_ScreenHeight);
        frame.SetFull();
        full = true;
    }

    // Present set: this frame's draws plus the previous frame's, whatever
    // buffer that was rendered into.
    if (full || frame.IsFull() || (m_HaveLastDrawn && m_LastDrawn.IsFull()))
    {
        m_PresentCount = -1;
    }
    else
    {
        m_PresentScratch = frame;
        if (m_HaveLastDrawn)
            m_PresentScratch.Union(m_LastDrawn);
        if (m_PresentScratch.IsFull())
            m_PresentCount = -1;
        else
        {
            m_PresentRects = m_PresentScratch.Rects();
            m_PresentCount = static_cast<int32_t>(m_PresentRects.size());
        }
    }

    // History: what this buffer holds now, and what the screen is about to show.
    hist->lastDrawn = frame;
    hist->valid = true;
    m_LastDrawn = frame;
    m_HaveLastDrawn = true;
    m_ForceFull = false;
}

DekiRenderSystem::BufferHistory& DekiRenderSystem::HistoryFor(const uint8_t* buffer)
{
    for (BufferHistory& h : m_History)
        if (h.buffer == buffer) return h;
    m_History.push_back(BufferHistory{ buffer, DirtyRegion{}, false });
    return m_History.back();
}

void DekiRenderSystem::ResetDirtyHistory()
{
    m_History.clear();
    m_HaveLastDrawn = false;
    m_HaveClearColor = false;
    m_ForceFull = true;
    m_PresentCount = -1;
}

void DekiRenderSystem::SetDirtyTracking(bool enabled, int32_t alignment)
{
    m_TrackDirty = enabled;
    m_DirtyAlign = alignment < 1 ? 1 : alignment;
    ResetDirtyHistory();
}

const Deki::Rect* DekiRenderSystem::GetPresentRects(int32_t* count) const
{
    if (count) *count = m_PresentCount;
    return m_PresentCount > 0 ? m_PresentRects.data() : nullptr;
}

void DekiRenderSystem::RenderToBuffer(Deki::Scene* scene, Deki::ICamera* camera,
                                       uint8_t* buffer, int32_t width, int32_t height,
                                       Deki::ColorFormat format)
{
    RenderToBufferStatic(scene, camera, buffer, width, height, format);
}

void DekiRenderSystem::RenderToBufferStatic(Deki::Scene* scene, Deki::ICamera* camera,
                                             uint8_t* buffer, int32_t width, int32_t height,
                                             Deki::ColorFormat format)
{
    if (!scene || !camera || !buffer)
        return;

    // Get the renderer from the engine's render system
    DekiRenderer* renderer = Deki::Engine::GetInstance().GetRenderSystem()->GetRenderer();
    if (!renderer)
        return;

    // RenderContext uses CameraComponent* internally — safe cast since
    // the rendering package owns CameraComponent and knows the concrete type
    RenderContext ctx{static_cast<CameraComponent*>(camera), buffer, width, height, format};
    renderer->Render(scene, ctx);
}

namespace
{
// One pixel of `format` at p; returns its size in bytes.
inline size_t WritePixel(uint8_t* p, Deki::ColorFormat format, uint8_t r, uint8_t g, uint8_t b)
{
    switch (format)
    {
        case Deki::ColorFormat::RGB565:
        {
            const uint16_t v = static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            memcpy(p, &v, 2);
            return 2;
        }
        case Deki::ColorFormat::RGB888:
            p[0] = r; p[1] = g; p[2] = b;
            return 3;
        case Deki::ColorFormat::ARGB8888:
        {
            const uint32_t v = (0xFFu << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
            memcpy(p, &v, 4);
            return 4;
        }
        case Deki::ColorFormat::RGB565A8:
        {
            const uint16_t v = static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            p[0] = static_cast<uint8_t>(v & 0xFF);
            p[1] = static_cast<uint8_t>(v >> 8);
            p[2] = 0xFF;  // opaque
            return 3;
        }
    }
    return 0;
}
}  // namespace

void DekiRenderSystem::ClearRect(int32_t x, int32_t y, int32_t w, int32_t h, uint8_t r, uint8_t g, uint8_t b)
{
    if (!m_RenderBuffer) return;
    // Clip to the framebuffer.
    int32_t x0 = std::max<int32_t>(x, 0), y0 = std::max<int32_t>(y, 0);
    int32_t x1 = std::min<int32_t>(x + w, m_ScreenWidth), y1 = std::min<int32_t>(y + h, m_ScreenHeight);
    if (x1 <= x0 || y1 <= y0) return;

    const size_t bpp = static_cast<size_t>(GetBytesPerPixel(m_ColorFormat));
    const size_t pitch = static_cast<size_t>(m_ScreenWidth) * bpp;
    const size_t span = static_cast<size_t>(x1 - x0) * bpp;
    uint8_t* row0 = m_RenderBuffer + static_cast<size_t>(y0) * pitch + static_cast<size_t>(x0) * bpp;

    // Seed one pixel, double it across the first row, then copy the row down:
    // memcpy all the way instead of a per-pixel (or per-byte) loop.
    WritePixel(row0, m_ColorFormat, r, g, b);
    for (size_t written = bpp; written < span; written *= 2)
        memcpy(row0 + written, row0, std::min(written, span - written));
    for (int32_t yy = y0 + 1; yy < y1; ++yy)
        memcpy(row0 + static_cast<size_t>(yy - y0) * pitch, row0, span);
}

void DekiRenderSystem::ClearBuffer(uint8_t r, uint8_t g, uint8_t b)
{
    ClearRect(0, 0, m_ScreenWidth, m_ScreenHeight, r, g, b);
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
        case Deki::ColorFormat::RGB565:
        {
            size_t pixel_index = (y * m_ScreenWidth + x) * 2;
            uint16_t pixel = *((uint16_t*)(m_RenderBuffer + pixel_index));
            *r = ((pixel >> 11) & 0x1F) << 3;  // 5 bits -> 8 bits
            *g = ((pixel >> 5) & 0x3F) << 2;  // 6 bits -> 8 bits
            *b = (pixel & 0x1F) << 3;  // 5 bits -> 8 bits
            break;
        }
        case Deki::ColorFormat::RGB888:
        {
            size_t pixel_index = (y * m_ScreenWidth + x) * 3;
            *r = m_RenderBuffer[pixel_index];
            *g = m_RenderBuffer[pixel_index + 1];
            *b = m_RenderBuffer[pixel_index + 2];
            break;
        }
        case Deki::ColorFormat::ARGB8888:
        {
            size_t pixel_index = (y * m_ScreenWidth + x) * 4;
            uint32_t pixel = *((uint32_t*)(m_RenderBuffer + pixel_index));
            *r = (pixel >> 16) & 0xFF;
            *g = (pixel >> 8) & 0xFF;
            *b = pixel & 0xFF;
            break;
        }
        case Deki::ColorFormat::RGB565A8:
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

int DekiRenderSystem::GetBytesPerPixel(Deki::ColorFormat format)
{
    switch (format)
    {
        case Deki::ColorFormat::RGB565:
            return 2;
        case Deki::ColorFormat::RGB888:
            return 3;
        case Deki::ColorFormat::ARGB8888:
            return 4;
        case Deki::ColorFormat::RGB565A8:
            return 3;
    }
    return 2;
}

