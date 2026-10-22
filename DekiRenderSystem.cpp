#include "DekiRenderSystem.h"
#include "RenderProfiler.h"
#include "DekiRenderer.h"
#include "PixelFormat.h"
#include <deki/Engine.h>
#include <deki/LogSystem.h>
#include <deki/SceneSystem.h>
#include <deki/providers/Memory.h>
#include <deki/providers/IDisplay.h>
#include "CameraComponent.h"
#include <deki/Object.h>
#include <deki/Scene.h>
#include <deki/ScreenAnchor.h>
#include "RenderingProjectSettings.h"
#include <deki/ProjectSettings.h>
#include <deki/reflection/SettingsRegistry.h>

#include <algorithm>
#include <cstdio>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace DekiRendering
{

namespace
{
// Screenshot requests, counted: each render system takes those it has not
// seen yet (DekiRenderSystem::RequestFrameCapture).
std::atomic<uint32_t> s_CaptureRequests{ 0 };

// Writes one pixel of `format` at p and returns its size in bytes.
inline size_t WritePixel(uint8_t* p, Deki::ColorFormat format, uint8_t r, uint8_t g, uint8_t b)
{
    switch (format)
    {
        case Deki::ColorFormat::RGB565:
            DekiPixel::Store565(p, DekiPixel::PackRGB565(r, g, b));
            return 2;
        case Deki::ColorFormat::RGB888:
            p[0] = r;
            p[1] = g;
            p[2] = b;
            return 3;
        case Deki::ColorFormat::ARGB8888:
        {
            const uint32_t v = (0xFFu << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
            memcpy(p, &v, 4);
            return 4;
        }
        case Deki::ColorFormat::RGB565A8:
            DekiPixel::Store565(p, DekiPixel::PackRGB565(r, g, b));
            p[2] = 0xFF;  // opaque
            return 3;
    }
    return 0;
}

// Fills [x, x+w) x [y, y+h) of a buffer of bufW x bufH pixels, clipped.
void FillRect(uint8_t* buffer, int32_t bufW, int32_t bufH, Deki::ColorFormat format, int32_t x, int32_t y, int32_t w,
              int32_t h, uint8_t r, uint8_t g, uint8_t b)
{
    int32_t x0 = std::max<int32_t>(x, 0), y0 = std::max<int32_t>(y, 0);
    int32_t x1 = std::min<int32_t>(x + w, bufW), y1 = std::min<int32_t>(y + h, bufH);
    if (x1 <= x0 || y1 <= y0)
    {
        return;
    }

    const size_t bpp = static_cast<size_t>(DekiRenderSystem::GetBytesPerPixelOf(format));
    const size_t pitch = static_cast<size_t>(bufW) * bpp;
    const size_t span = static_cast<size_t>(x1 - x0) * bpp;
    uint8_t* row0 = buffer + static_cast<size_t>(y0) * pitch + static_cast<size_t>(x0) * bpp;

    // Write one pixel, double it across the first row, then copy the row down:
    // all memcpy, no per-pixel loop.
    WritePixel(row0, format, r, g, b);
    for (size_t written = bpp; written < span; written *= 2)
    {
        memcpy(row0 + written, row0, std::min(written, span - written));
    }
    for (int32_t yy = y0 + 1; yy < y1; ++yy)
    {
        memcpy(row0 + static_cast<size_t>(yy - y0) * pitch, row0, span);
    }
}
}  // namespace

DekiRenderSystem::DekiRenderSystem()
    : m_RenderBuffer(nullptr),
      m_ScreenWidth(0),
      m_ScreenHeight(0),
      m_ColorFormat(Deki::ColorFormat::RGB565),
      m_CaptureSeen(s_CaptureRequests.load())
{
}

DekiRenderSystem::~DekiRenderSystem()
{
    if (m_RenderBuffer && m_OwnsBuffer)
    {
        Deki::Memory::FreeInternal(m_RenderBuffer);
    }
    m_RenderBuffer = nullptr;
    FreeBand();
}

namespace
{
char s_FrameModeNote[160] = "whole frames";
int32_t s_FrameModeRows = 0;
}  // namespace

DekiRenderSystem::FrameChoice DekiRenderSystem::ChooseFrameMode(FrameMode mode, int32_t rows, size_t frameBytes,
                                                                size_t freeInternal, bool displayTakesPartialFrames)
{
    rows = rows > 0 ? rows : 40;
    switch (mode)
    {
        case FrameMode::WholeFrames: return { 0, FrameChoice::Why::Chosen };
        case FrameMode::Strips: return { rows, FrameChoice::Why::Chosen };
        case FrameMode::Auto: break;
    }
    if (!displayTakesPartialFrames)
    {
        return { 0, FrameChoice::Why::WholeDisplay };
    }
    if (frameBytes + kInternalHeadroom <= freeInternal)
    {
        return { 0, FrameChoice::Why::Fits };
    }
    return { rows, FrameChoice::Why::DoesNotFit };
}

void DekiRenderSystem::NoteFrameChoice(const FrameChoice& choice, size_t frameBytes, size_t freeInternal)
{
    const unsigned frameKb = static_cast<unsigned>((frameBytes + 1023) / 1024);
    const unsigned freeKb = static_cast<unsigned>(freeInternal / 1024);
    char mode[32];
    if (choice.rows > 0)
    {
        std::snprintf(mode, sizeof(mode), "strips of %d rows", static_cast<int>(choice.rows));
    }
    else
    {
        std::snprintf(mode, sizeof(mode), "whole frames");
    }
    switch (choice.why)
    {
        case FrameChoice::Why::Chosen:
            std::snprintf(s_FrameModeNote, sizeof(s_FrameModeNote), "%s: chosen in the project's Rendering settings",
                          mode);
            break;
        case FrameChoice::Why::Fits:
            std::snprintf(s_FrameModeNote, sizeof(s_FrameModeNote),
                          "%s: the %u KB framebuffer fits in internal RAM (%u KB free)", mode, frameKb, freeKb);
            break;
        case FrameChoice::Why::DoesNotFit:
            std::snprintf(s_FrameModeNote, sizeof(s_FrameModeNote),
                          "%s: the %u KB framebuffer does not fit in internal RAM (%u KB free, %u KB kept for the game)",
                          mode, frameKb, freeKb, static_cast<unsigned>(kInternalHeadroom / 1024));
            break;
        case FrameChoice::Why::WholeDisplay:
            std::snprintf(s_FrameModeNote, sizeof(s_FrameModeNote), "%s: the display takes only whole frames", mode);
            break;
        case FrameChoice::Why::Editor:
            std::snprintf(s_FrameModeNote, sizeof(s_FrameModeNote), "%s: the editor always draws whole frames", mode);
            break;
    }
    s_FrameModeRows = choice.rows;
    if (choice.why != FrameChoice::Why::Editor)
    {
        DEKI_LOG_INFO("[Rendering] %s", s_FrameModeNote);
    }
}

const char* DekiRenderSystem::LastFrameModeNote()
{
    return s_FrameModeNote;
}

int32_t DekiRenderSystem::LastFrameModeRows()
{
    return s_FrameModeRows;
}

bool DekiRenderSystem::Setup(int32_t width, int32_t height, Deki::ColorFormat format)
{
    // Project-wide rendering settings. In the editor the registry holds the
    // loaded instance; on device there is no registry, so the values come
    // straight from project_data.bin. Half-width and interlaced are not
    // implemented yet and are only reported.
    m_TrackDirty = false;
    m_DirtyAlign = 32;
    bool halfWidth = false, interlaced = false;
    int32_t bandRows = 40;
    FrameMode frameMode = FrameMode::Auto;
    if (auto* rs = Deki::SettingsRegistry::Instance().Get<RenderingProjectSettings>())
    {
        m_TrackDirty = rs->dirtyTileTracking;
        m_DirtyAlign = rs->dirtyTileSize;
        halfWidth = rs->halfWidthFramebuffer;
        interlaced = rs->interlaced60hz;
        bandRows = rs->bandRows;
        frameMode = rs->frameMode;
    }
    else
    {
        // "Rendering" is RenderingProjectSettings' DEKI_PROJECT_SETTINGS_SECTION.
        bool b = false;
        int32_t a = 0;
        if (Deki::ProjectSettings::ReadPackageSettingBool("Rendering", "dirtyTileTracking", b))
        {
            m_TrackDirty = b;
        }
        if (Deki::ProjectSettings::ReadPackageSettingInt32("Rendering", "dirtyTileSize", a))
        {
            m_DirtyAlign = a;
        }
        if (Deki::ProjectSettings::ReadPackageSettingBool("Rendering", "halfWidthFramebuffer", b))
        {
            halfWidth = b;
        }
        if (Deki::ProjectSettings::ReadPackageSettingBool("Rendering", "interlaced60hz", b))
        {
            interlaced = b;
        }
        if (Deki::ProjectSettings::ReadPackageSettingInt32("Rendering", "bandRows", a))
        {
            bandRows = a;
        }
        if (Deki::ProjectSettings::ReadPackageSettingEnum("Rendering", "frameMode", sizeof(FrameMode), a))
        {
            frameMode = static_cast<FrameMode>(a);
        }
    }
    if (m_DirtyAlign < 1)
    {
        m_DirtyAlign = 1;
    }
    if (halfWidth || interlaced)
    {
        DEKI_LOG(Deki::LogLevel::Info, "[Rendering] settings: half_width=%d interlaced=%d (no implementation yet)",
                 (int)halfWidth, (int)interlaced);
    }
    if (m_TrackDirty)
    {
        DEKI_LOG_INTERNAL("[Rendering] dirty-rect tracking on, alignment %d px", m_DirtyAlign);
    }
    ResetDirtyHistory();

    if (width <= 0 || height <= 0)
    {
        DEKI_LOG_ERROR("DekiRenderSystem::Setup: invalid size %dx%d", width, height);
        return false;
    }

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

    // Whole frames or strips (RenderingProjectSettings::frameMode), said in
    // the log with the reason, and kept for DEKI:STATS.
    const size_t frameBytes = static_cast<size_t>(width) * static_cast<size_t>(height) *
                              static_cast<size_t>(GetBytesPerPixel(format));
#ifdef DEKI_EDITOR
    // The editor reads whole frames back (the Play view, captures, the
    // pixel gates), so it never renders in strips.
    const size_t freeInternal = 0;
    const FrameChoice choice{ 0, FrameChoice::Why::Editor };
#else
    const size_t freeInternal = Deki::Memory::GetAvailableInternal();
    Deki::IDisplay* display = Deki::Engine::GetInstance().GetDisplay();
    const FrameChoice choice =
        ChooseFrameMode(frameMode, bandRows, frameBytes, freeInternal, !display || display->SupportsPartialPresent());
#endif
    NoteFrameChoice(choice, frameBytes, freeInternal);

    // In strips there is no framebuffer at all, which is the point: the
    // whole frame may not fit where drawing is fast.
    FreeBand();
    if (choice.rows > 0)
    {
        if (SetBandRows(choice.rows))
        {
            if (m_BandRows > 0)
            {
                s_FrameModeRows = m_BandRows;  // whole pairs of rows
                return true;
            }
        }
        else
        {
            DEKI_LOG_ERROR("[Rendering] no internal memory for a %d-row strip; drawing whole frames", choice.rows);
            NoteFrameChoice(FrameChoice{ 0, FrameChoice::Why::DoesNotFit }, frameBytes, freeInternal);
        }
    }

    // Prefer a buffer the display provides; it saves a memcpy in Present.
    if (TryAdoptDisplayBuffer())
    {
        return true;
    }

    // No display yet, or its buffer does not match: allocate our own now.
    // Setup must leave a usable buffer or fail; returning true with a null
    // buffer would draw nothing and report no error.
    int bytesPerPixel = GetBytesPerPixel(format);
    size_t bufferSize = (size_t)width * (size_t)height * (size_t)bytesPerPixel;
    m_RenderBuffer = (uint8_t*)Deki::Memory::AllocateInternal(bufferSize);
    if (!m_RenderBuffer)
    {
        DEKI_LOG_ERROR("DekiRenderSystem::Setup: failed to allocate %zu-byte framebuffer (%dx%d)", bufferSize, width,
                       height);
        return false;
    }
    return true;
}

bool DekiRenderSystem::TryAdoptDisplayBuffer()
{
    Deki::IDisplay* display = Deki::Engine::GetInstance().GetDisplay();
    if (!display || display == m_AdoptionCheckedDisplay)
    {
        return false;
    }
    m_AdoptionCheckedDisplay = display;

    int32_t dw = 0, dh = 0;
    uint8_t* directBuf = display->GetRenderBuffer(&dw, &dh);
    if (!directBuf || dw != m_ScreenWidth || dh != m_ScreenHeight)
    {
        return false;
    }

    if (m_RenderBuffer && m_OwnsBuffer)
    {
        Deki::Memory::FreeInternal(m_RenderBuffer);
    }
    m_RenderBuffer = directBuf;
    m_OwnsBuffer = false;
    return true;
}

void DekiRenderSystem::Render(Deki::Scene* currentScene)
{
    if (!currentScene || !m_Renderer)
    {
        return;
    }

    // A display registered after Setup() may offer a direct buffer: adopt it
    // once. When already using the display's buffer, ask for it each frame,
    // since double-buffered displays alternate it in Present. Strips draw
    // into their own buffer.
    if (m_BandRows == 0)
    {
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
                {
                    m_RenderBuffer = buf;
                }
            }
        }

        if (!m_RenderBuffer)
        {
            return;
        }
    }

    // Find the scene's camera every frame rather than caching it: a cached
    // pointer dangles when the camera is removed at runtime or a new scene
    // reuses the old one's address. The walk is cheap next to a frame of blits.
    CameraComponent* camera = nullptr;
    for (Deki::Object* obj : currentScene->GetObjects())
    {
        Deki::Object* holder =
            FindInSubtree(obj, [](Deki::Object* o) { return o->GetComponent<CameraComponent>() != nullptr; });
        if (holder)
        {
            camera = holder->GetComponent<CameraComponent>();
            break;
        }
    }
    if (!camera)
    {
        // Fall back to persistent objects.
        const auto& persistentObjects = Deki::Engine::GetInstance().GetSceneSystem().GetPersistentObjects();
        for (Deki::Object* obj : persistentObjects)
        {
            camera = obj->GetComponent<CameraComponent>();
            if (camera)
            {
                break;
            }
        }
    }

    if (!camera)
    {
        return;
    }

    // Before anything draws, so a HUD pinned to the screen is where this
    // frame's camera puts the screen.
    Deki::ScreenAnchor::PlaceAll(currentScene, *camera, m_ScreenWidth, m_ScreenHeight);

    const bool capture = TakeCaptureRequest();
    if (m_BandRows > 0)
    {
        if (m_Renderer->DrawsInStrips(currentScene))
        {
            if (m_WholeFrames)
            {
                DEKI_LOG_INTERNAL("[Rendering] drawing in strips of %d rows again", m_BandRows);
                m_WholeFrames = false;
                m_RenderBuffer = nullptr;
                m_OwnsBuffer = true;
                ResetDirtyHistory();
            }
            RenderBands(currentScene, camera, capture);
            return;
        }

        // Something in the frame cannot be drawn strip by strip (a 3D mesh,
        // say): draw whole frames, as with bandRows 0, into a frame of our own
        // until it can. It holds what strips kept, so nothing jumps.
        if (!m_WholeFrames)
        {
            uint8_t* whole = EnsureWholeFrame(camera->clearColor);
            if (!whole)
            {
                m_PresentCount = 0;
                return;
            }
            DEKI_LOG_WARNING("[Rendering] the renderer or one of its render passes cannot draw in strips: drawing "
                             "whole frames in %s memory while it cannot",
                             Deki::Memory::IsExternalAvailable() ? "external" : "internal");
            m_WholeFrames = true;
            m_RenderBuffer = whole;
            m_OwnsBuffer = false;
            ResetDirtyHistory();
        }
        m_WholeFrameHolds = true;
    }

    // ---- dirty-rect present -------------------------------------------------
    // Whenever the bookkeeping is unsure (first use of a buffer, a size or
    // format change, a change of the camera's clear colour or clearEveryFrame,
    // a frame the renderer could not describe, MarkAllDirty), the frame is a
    // full clear and a full present, the same as with tracking off.
    const bool tracking = m_TrackDirty;
    BufferHistory* hist = tracking ? &HistoryFor(m_RenderBuffer) : nullptr;
    bool full = !tracking || m_ForceFull || !hist->valid;
    const Deki::Color clear = camera->clearColor;
    if (ClearChanged(*camera))
    {
        full = true;
    }

    // Clear before rendering, unless the camera says the scene paints every
    // pixel itself. With tracking, only what the last frame on this buffer
    // drew needs clearing.
    if (camera->clearEveryFrame)
    {
        if (full || hist->lastDrawn.IsFull())
        {
            const uint64_t clearStart = RenderProfiler::IsEnabled() ? RenderProfiler::Now() : 0;
            ClearBuffer(clear);
            if (RenderProfiler::IsEnabled())
            {
                RenderProfiler::AddContent(RenderProfiler::kClear, RenderProfiler::Now() - clearStart);
            }
        }
        else
        {
            for (const Deki::Rect& r : hist->lastDrawn.Rects())
            {
                ClearRect(r.left, r.top, r.Width(), r.Height(), clear.r, clear.g, clear.b);
            }
        }
    }

    RenderProfiler::CountFrame();
    RenderContext ctx{ camera, m_RenderBuffer, m_ScreenWidth, m_ScreenHeight, m_ColorFormat };
    ctx.trackDirty = tracking;
    m_Renderer->Render(currentScene, ctx);

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
        {
            m_PresentScratch.Union(m_LastDrawn);
        }
        if (m_PresentScratch.IsFull())
        {
            m_PresentCount = -1;
        }
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

void DekiRenderSystem::RenderBands(Deki::Scene* scene, CameraComponent* camera, bool capture)
{
    // Every strip is sent here, so Engine::PresentFrame has nothing left.
    m_PresentCount = 0;

    Deki::IDisplay* display = Deki::Engine::GetInstance().GetDisplay();
    if (!display || !display->SupportsPartialPresent())
    {
        if (!m_BandWarned)
        {
            DEKI_LOG_ERROR("[Rendering] bandRows is set, but %s; nothing is drawn",
                           display ? "the display cannot be sent part of a frame" : "there is no display");
            m_BandWarned = true;
        }
        return;
    }

    if (++m_ContentFrame == 0)
    {
        m_ContentFrame = 1;  // 0 means "not in strips"
    }
    RenderProfiler::CountFrame();

    const Deki::Color clear = camera->clearColor;
    const bool clearChanged = ClearChanged(*camera);

    // A camera that does not clear keeps the last frame, and a screenshot
    // needs this one whole: either way the strips also go to a whole frame.
    const bool keepWanted = !camera->clearEveryFrame;
    uint8_t* whole = (keepWanted || capture) ? EnsureWholeFrame(clear) : nullptr;
    const bool keep = keepWanted && whole;
    if (keepWanted && !whole && !m_WholeFrameWarned)
    {
        DEKI_LOG_ERROR("[Rendering] the camera does not clear, but there is no memory for the frame strips must "
                       "keep for it; every strip is cleared");
        m_WholeFrameWarned = true;
    }

    // Dirty rectangles: send only the rows of a strip that this frame or the
    // last drew in. Whenever that is unknown, the whole strip goes.
    const bool tracking = m_TrackDirty;
    const bool sendAll = !tracking || m_ForceFull || clearChanged || !m_HaveLastDrawn || m_LastDrawn.IsFull();
    if (tracking)
    {
        m_BandFrameDrawn.Reset(m_ScreenWidth, m_ScreenHeight);
    }
    if (clearChanged || keep)
    {
        m_BandHeldValid = false;
    }

    const size_t pitch = static_cast<size_t>(m_ScreenWidth) * GetBytesPerPixel(m_ColorFormat);
    for (int32_t top = 0; top < m_ScreenHeight; top += m_BandRows)
    {
        const int32_t rows = std::min(m_BandRows, m_ScreenHeight - top);
        uint8_t* const wholeRows = whole ? whole + static_cast<size_t>(top) * pitch : nullptr;

        // The strip's starting pixels: the kept frame's rows, or the clear
        // colour. The strip buffer still holds the strip drawn before, so
        // with tracking only what that one drew is cleared.
        const uint64_t clearStart = RenderProfiler::IsEnabled() ? RenderProfiler::Now() : 0;
        if (keep)
        {
            memcpy(m_BandBuffer, wholeRows, static_cast<size_t>(rows) * pitch);
        }
        else if (m_BandHeldValid && !m_BandHeld.IsFull())
        {
            for (const Deki::Rect& r : m_BandHeld.Rects())
            {
                FillRect(m_BandBuffer, m_ScreenWidth, m_BandRows, m_ColorFormat, r.left, r.top, r.Width(), r.Height(),
                         clear.r, clear.g, clear.b);
            }
        }
        else
        {
            FillRect(m_BandBuffer, m_ScreenWidth, m_BandRows, m_ColorFormat, 0, 0, m_ScreenWidth, m_BandRows, clear.r,
                     clear.g, clear.b);
        }
        if (RenderProfiler::IsEnabled())
        {
            RenderProfiler::AddContent(RenderProfiler::kClear, RenderProfiler::Now() - clearStart);
        }

        RenderContext ctx{ camera, m_BandBuffer, m_ScreenWidth, rows, m_ColorFormat };
        ctx.viewTop = top;
        ctx.viewHeight = m_ScreenHeight;
        ctx.contentFrame = m_ContentFrame;
        ctx.trackDirty = tracking;
        m_Renderer->Render(scene, ctx);

        // What this strip drew, in its own rows. Null: unknown, so all of it.
        const DirtyRegion* drawn = tracking ? m_Renderer->GetLastFrameDirty() : nullptr;
        const bool drawnKnown = drawn && !drawn->IsFull();
        if (drawn && !keep)
        {
            m_BandHeld = *drawn;
            m_BandHeldValid = true;
        }
        else
        {
            m_BandHeldValid = false;
        }

        if (whole)
        {
            memcpy(wholeRows, m_BandBuffer, static_cast<size_t>(rows) * pitch);
        }

        const Deki::Rect strip{ 0, top, m_ScreenWidth, top + rows };
        Deki::Rect send = strip;
        if (tracking)
        {
            if (drawnKnown)
            {
                for (const Deki::Rect& r : drawn->Rects())
                {
                    m_BandFrameDrawn.Add(r.left, r.top + top, r.right, r.bottom + top);
                }
            }
            else
            {
                m_BandFrameDrawn.Add(strip);
            }
        }
        if (!sendAll && drawnKnown)
        {
            // The bounds of this frame's draws and last frame's, in the strip.
            send = Deki::Rect{ m_ScreenWidth, strip.bottom, 0, top };
            auto grow = [&send, &strip](int32_t l, int32_t t, int32_t r, int32_t b)
            {
                t = std::max(t, strip.top);
                b = std::min(b, strip.bottom);
                if (r <= l || b <= t)
                {
                    return;
                }
                send.left = std::min(send.left, l);
                send.top = std::min(send.top, t);
                send.right = std::max(send.right, r);
                send.bottom = std::max(send.bottom, b);
            };
            for (const Deki::Rect& r : drawn->Rects())
            {
                grow(r.left, r.top + top, r.right, r.bottom + top);
            }
            for (const Deki::Rect& r : m_LastDrawn.Rects())
            {
                grow(r.left, r.top, r.right, r.bottom);
            }
        }

        // PresentRegions reads the rectangle's rows out of a whole frame. The
        // strip is exactly those rows, so it is handed over as a frame whose
        // row `top` starts at the strip: the display reads nothing else.
        if (!send.Empty())
        {
            const auto* frameBase = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(m_BandBuffer) -
                                                                     static_cast<uintptr_t>(top) * pitch);
            display->PresentRegions(frameBase, m_ScreenWidth, m_ScreenHeight, m_ColorFormat, &send, 1);
        }
    }

    if (tracking)
    {
        std::swap(m_LastDrawn, m_BandFrameDrawn);
        m_HaveLastDrawn = true;
        m_ForceFull = false;
    }
    else
    {
        m_HaveLastDrawn = false;
    }

    // Kept for the next frame or read by a screenshot; otherwise nothing
    // needs it any more.
    m_WholeFrameHolds = whole != nullptr;
    if (!whole)
    {
        FreeWholeFrame();
    }
}

uint8_t* DekiRenderSystem::EnsureWholeFrame(const Deki::Color& clear)
{
    if (m_WholeFrame)
    {
        return m_WholeFrame;
    }
    const size_t bytes = Deki::FrameBufferBytes(m_ColorFormat, m_ScreenWidth, m_ScreenHeight);
    const bool external = Deki::Memory::IsExternalAvailable();
    m_WholeFrame = static_cast<uint8_t*>(
        Deki::Memory::Allocate(bytes, external ? Deki::Memory::External : Deki::Memory::Internal));
    if (!m_WholeFrame)
    {
        if (!m_WholeFrameWarned)
        {
            DEKI_LOG_ERROR("[Rendering] no %s memory for a whole %dx%d frame (%u bytes)",
                           external ? "external" : "internal", static_cast<int>(m_ScreenWidth),
                           static_cast<int>(m_ScreenHeight), static_cast<unsigned>(bytes));
            m_WholeFrameWarned = true;
        }
        return nullptr;
    }
    // Nothing was kept before it: it starts as a cleared frame.
    FillRect(m_WholeFrame, m_ScreenWidth, m_ScreenHeight, m_ColorFormat, 0, 0, m_ScreenWidth, m_ScreenHeight, clear.r,
             clear.g, clear.b);
    m_WholeFrameHolds = false;
    return m_WholeFrame;
}

void DekiRenderSystem::FreeWholeFrame()
{
    if (m_WholeFrame)
    {
        if (m_RenderBuffer == m_WholeFrame)
        {
            m_RenderBuffer = nullptr;
            m_OwnsBuffer = true;
        }
        Deki::Memory::Free(m_WholeFrame);
        m_WholeFrame = nullptr;
    }
    m_WholeFrameHolds = false;
    m_WholeFrames = false;
}

void DekiRenderSystem::RequestFrameCapture()
{
    s_CaptureRequests.fetch_add(1);
}

bool DekiRenderSystem::TakeCaptureRequest()
{
    const uint32_t requests = s_CaptureRequests.load();
    const bool wanted = requests != m_CaptureSeen;
    m_CaptureSeen = requests;
    return wanted;
}

bool DekiRenderSystem::ClearChanged(const CameraComponent& camera)
{
    const Deki::Color& clear = camera.clearColor;
    if (m_HaveClearColor && clear.r == m_LastClearColor.r && clear.g == m_LastClearColor.g &&
        clear.b == m_LastClearColor.b && camera.clearEveryFrame == m_LastClearEveryFrame)
    {
        return false;
    }
    m_LastClearColor = clear;
    m_LastClearEveryFrame = camera.clearEveryFrame;
    m_HaveClearColor = true;
    return true;
}

bool DekiRenderSystem::SetBandRows(int32_t rows)
{
    FreeBand();
    m_BandWarned = false;
    if (rows <= 0 || m_ScreenHeight <= 0 || rows >= m_ScreenHeight)
    {
        return true;  // whole frames
    }
    // Whole pairs of rows: some panels take writes only in pairs of rows
    // (the RM67162 AMOLED), and a strip widened to one would be read past.
    m_BandRows = (rows + 1) & ~1;
    if (!AllocateBand())
    {
        m_BandRows = 0;
        return false;
    }
    return true;
}

bool DekiRenderSystem::AllocateBand()
{
    const size_t bytes = static_cast<size_t>(m_ScreenWidth) * static_cast<size_t>(m_BandRows) *
                         static_cast<size_t>(GetBytesPerPixel(m_ColorFormat));
    m_BandBuffer = static_cast<uint8_t*>(Deki::Memory::AllocateInternal(bytes));
    return m_BandBuffer != nullptr;
}

void DekiRenderSystem::FreeBand()
{
    if (m_BandBuffer)
    {
        Deki::Memory::FreeInternal(m_BandBuffer);
        m_BandBuffer = nullptr;
    }
    m_BandRows = 0;
    m_BandHeldValid = false;
    FreeWholeFrame();
}

DekiRenderSystem::BufferHistory& DekiRenderSystem::HistoryFor(const uint8_t* buffer)
{
    for (BufferHistory& h : m_History)
    {
        if (h.buffer == buffer)
        {
            return h;
        }
    }
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
    if (count)
    {
        *count = m_PresentCount;
    }
    return m_PresentCount > 0 ? m_PresentRects.data() : nullptr;
}

void DekiRenderSystem::RenderToBuffer(Deki::Scene* scene, Deki::ICamera* camera, uint8_t* buffer, int32_t width,
                                      int32_t height, Deki::ColorFormat format)
{
    RenderToBufferStatic(scene, camera, buffer, width, height, format);
}

void DekiRenderSystem::RenderToBufferStatic(Deki::Scene* scene, Deki::ICamera* camera, uint8_t* buffer, int32_t width,
                                            int32_t height, Deki::ColorFormat format)
{
    if (!scene || !camera || !buffer)
    {
        return;
    }

    DekiRenderer* renderer = Deki::Engine::GetInstance().GetRenderSystem()->GetRenderer();
    if (!renderer)
    {
        return;
    }

    // Safe cast: every ICamera in this package is a CameraComponent.
    RenderContext ctx{ static_cast<CameraComponent*>(camera), buffer, width, height, format };
    renderer->Render(scene, ctx);
}

void DekiRenderSystem::ClearRect(int32_t x, int32_t y, int32_t w, int32_t h, uint8_t r, uint8_t g, uint8_t b)
{
    if (!m_RenderBuffer)
    {
        return;
    }
    FillRect(m_RenderBuffer, m_ScreenWidth, m_ScreenHeight, m_ColorFormat, x, y, w, h, r, g, b);
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
    const uint8_t* frame = GetFrameBuffer();
    if (!frame || !r || !g || !b)
    {
        if (r)
        {
            *r = 0;
        }
        if (g)
        {
            *g = 0;
        }
        if (b)
        {
            *b = 0;
        }
        return;
    }

    if (x < 0 || x >= m_ScreenWidth || y < 0 || y >= m_ScreenHeight)
    {
        *r = *g = *b = 0;
        return;
    }

    switch (m_ColorFormat)
    {
        case Deki::ColorFormat::RGB565:
        {
            size_t pixel_index = (y * m_ScreenWidth + x) * 2;
            DekiPixel::UnpackRGB565(DekiPixel::Load565(frame + pixel_index), *r, *g, *b);
            break;
        }
        case Deki::ColorFormat::RGB888:
        {
            size_t pixel_index = (y * m_ScreenWidth + x) * 3;
            *r = frame[pixel_index];
            *g = frame[pixel_index + 1];
            *b = frame[pixel_index + 2];
            break;
        }
        case Deki::ColorFormat::ARGB8888:
        {
            size_t pixel_index = (y * m_ScreenWidth + x) * 4;
            uint32_t pixel = *((uint32_t*)(frame + pixel_index));
            *r = (pixel >> 16) & 0xFF;
            *g = (pixel >> 8) & 0xFF;
            *b = pixel & 0xFF;
            break;
        }
        case Deki::ColorFormat::RGB565A8:
        {
            size_t pixel_index = (y * m_ScreenWidth + x) * 3;
            DekiPixel::UnpackRGB565(DekiPixel::Load565(frame + pixel_index), *r, *g, *b);
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

int DekiRenderSystem::GetBytesPerPixelOf(Deki::ColorFormat format)
{
    switch (format)
    {
        case Deki::ColorFormat::RGB565: return 2;
        case Deki::ColorFormat::RGB888: return 3;
        case Deki::ColorFormat::ARGB8888: return 4;
        case Deki::ColorFormat::RGB565A8: return 3;
    }
    return 2;
}

}  // namespace DekiRendering
