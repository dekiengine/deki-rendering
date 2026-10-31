#include "DekiRenderSystem.h"
#include "RenderProfiler.h"
#include "DekiRenderer.h"
#include "PixelFormat.h"
#include "QuadBlit.h"
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
        case Deki::ColorFormat::RGB565: DekiPixel::Store565(p, DekiPixel::PackRGB565(r, g, b)); return 2;
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
        Deki::Memory::Free(m_RenderBuffer);
    }
    m_RenderBuffer = nullptr;
    FreeBand();
}

namespace
{
bool IsColumns(ScanOrder order)
{
    return order == ScanOrder::ColumnsRight || order == ScanOrder::ColumnsLeft;
}

bool IsReversed(ScanOrder order)
{
    return order == ScanOrder::RowsUp || order == ScanOrder::ColumnsLeft;
}

// The scan order strips are cut along now: the current display's, or rows
// from the top when it offers none.
IScanOrderedDisplay* CurrentScanDisplay()
{
    return FindScanOrderedDisplay(Deki::Engine::GetInstance().GetDisplay());
}

// Copies a w x h rectangle of pixels between buffers of different pitches.
void CopyPixels(uint8_t* dst, size_t dstPitch, const uint8_t* src, size_t srcPitch, size_t rowBytes, int32_t rows)
{
    if (dstPitch == rowBytes && srcPitch == rowBytes)
    {
        memcpy(dst, src, rowBytes * static_cast<size_t>(rows));
        return;
    }
    for (int32_t y = 0; y < rows; ++y)
    {
        memcpy(dst + static_cast<size_t>(y) * dstPitch, src + static_cast<size_t>(y) * srcPitch, rowBytes);
    }
}
}  // namespace

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
            std::snprintf(
                s_FrameModeNote, sizeof(s_FrameModeNote),
                "%s: the %u KB framebuffer does not fit in internal RAM (%u KB free, %u KB kept for the game)", mode,
                frameKb, freeKb, static_cast<unsigned>(kInternalHeadroom / 1024));
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
    // straight from project_data.bin.
    m_RedrawChanges = false;
    int32_t bandRows = 40;
    FrameMode frameMode = FrameMode::Auto;
    if (auto* rs = Deki::SettingsRegistry::Instance().Get<RenderingProjectSettings>())
    {
        bandRows = rs->bandRows;
        frameMode = rs->frameMode;
        m_RedrawChanges = rs->redrawChanges;
    }
    else
    {
        // "Rendering" is RenderingProjectSettings' DEKI_PROJECT_SETTINGS_SECTION.
        bool b = false;
        int32_t a = 0;
        if (Deki::ProjectSettings::ReadPackageSettingInt32("Rendering", "bandRows", a))
        {
            bandRows = a;
        }
        if (Deki::ProjectSettings::ReadPackageSettingEnum("Rendering", "frameMode", sizeof(FrameMode), a))
        {
            frameMode = static_cast<FrameMode>(a);
        }
        if (Deki::ProjectSettings::ReadPackageSettingBool("Rendering", "redrawChanges", b))
        {
            m_RedrawChanges = b;
        }
    }
    if (m_RedrawChanges)
    {
        DEKI_LOG_INFO("[Rendering] redrawing only what changed");
    }
    ResetHistory();

    if (width <= 0 || height <= 0)
    {
        DEKI_LOG_ERROR("DekiRenderSystem::Setup: invalid size %dx%d", width, height);
        return false;
    }

    if (m_RenderBuffer && m_OwnsBuffer)
    {
        Deki::Memory::Free(m_RenderBuffer);
    }
    m_RenderBuffer = nullptr;
    m_OwnsBuffer = true;
    m_AdoptionCheckedDisplay = nullptr;

    m_ScreenWidth = width;
    m_ScreenHeight = height;
    m_ColorFormat = format;

    // Whole frames or strips (RenderingProjectSettings::frameMode), said in
    // the log with the reason, and kept for DEKI:STATS.
    const size_t frameBytes =
        static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(GetBytesPerPixel(format));
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
    m_RenderBuffer = (uint8_t*)Deki::Memory::Allocate(bufferSize, Deki::Memory::Internal);
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
        Deki::Memory::Free(m_RenderBuffer);
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
                ResetHistory();
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
            ResetHistory();
        }
        m_WholeFrameHolds = true;
    }

    if (RenderChanges(currentScene, camera))
    {
        return;
    }

    // The whole frame. A renderer that can tell its first draw covers it
    // clears lazily: then often never. A camera that does not clear keeps
    // the last frame and draws over it.
    ClearChanged(*camera);
    m_ClearColor = camera->clearColor;
    m_ClearRects = nullptr;
    const DeferredClear deferred{ &DekiRenderSystem::ClearFrame, this };
    const bool lazily = camera->clearEveryFrame && m_Renderer->ClearsLazily();
    if (camera->clearEveryFrame && !lazily)
    {
        ClearFrame();
    }

    RenderProfiler::CountFrame();
    QuadBlit::NextFrame();
    RenderContext ctx{ camera, m_RenderBuffer, m_ScreenWidth, m_ScreenHeight, m_ColorFormat };
    ctx.clear = lazily ? &deferred : nullptr;
    m_Renderer->Render(currentScene, ctx);
    m_PresentCount = -1;
    m_ForceFull = false;
}

const DirtyRegion* DekiRenderSystem::FindFrameChanges(Deki::Scene* scene, CameraComponent* camera, bool clearChanged)
{
    // A camera that does not clear draws each frame over the last, so what
    // did not change is drawn again on purpose.
    RenderContext whole{ camera, nullptr, m_ScreenWidth, m_ScreenHeight, m_ColorFormat };
    whole.contentFrame = m_ContentFrame;
    if (!m_RedrawChanges || !camera->clearEveryFrame)
    {
        m_Owed.clear();
        return nullptr;
    }
    const uint64_t start = RenderProfiler::IsEnabled() ? RenderProfiler::Now() : 0;
    const bool known = m_Renderer->FindChanges(scene, whole, m_Changed);
    if (RenderProfiler::IsEnabled())
    {
        RenderProfiler::AddContent(RenderProfiler::kChanges, RenderProfiler::Now() - start, "Changes");
    }
    if (!known)
    {
        m_Owed.clear();
        return nullptr;
    }
    if (clearChanged || m_ForceFull)
    {
        m_Changed.SetFull();
    }
    m_ForceFull = false;
    m_Changed.Align(kRedrawAlign);
    return &m_Changed;
}

bool DekiRenderSystem::MostOfScreen(const DirtyRegion& region) const
{
    return region.IsFull() ||
           region.CoveredArea() * 100 > static_cast<int64_t>(m_ScreenWidth) * m_ScreenHeight * kRedrawWholeAbove;
}

bool DekiRenderSystem::RenderChanges(Deki::Scene* scene, CameraComponent* camera)
{
    if (!m_RedrawChanges)
    {
        return false;
    }
    if (++m_ContentFrame == 0)
    {
        m_ContentFrame = 1;  // 0 means "no frame to replay"
    }
    // Checked first, so the usual path still sees a change of clear colour
    // when there is nothing to compare.
    if (!camera->clearEveryFrame)
    {
        m_Owed.clear();
        return false;
    }
    const DirtyRegion* changed = FindFrameChanges(scene, camera, false);
    if (!changed)
    {
        return false;
    }
    if (ClearChanged(*camera))
    {
        m_Changed.SetFull();
    }

    // Every buffer is owed this frame's changes; the one drawn now is brought
    // up to date. One never drawn into is drawn whole.
    OwedBuffer* mine = nullptr;
    for (OwedBuffer& o : m_Owed)
    {
        o.owed.Union(*changed);
        if (o.buffer == m_RenderBuffer)
        {
            mine = &o;
        }
    }
    DirtyRegion& redraw = m_RedrawScratch;
    if (mine)
    {
        redraw = mine->owed;
    }
    else
    {
        redraw.Reset(m_ScreenWidth, m_ScreenHeight);
        redraw.SetFull();
    }
    const bool whole = MostOfScreen(redraw);

    RenderProfiler::CountFrame();
    if (!whole && redraw.IsEmpty())
    {
        // Nothing moved: nothing to draw or send.
        m_PresentCount = 0;
        return true;
    }
    QuadBlit::NextFrame();
    m_ClearColor = camera->clearColor;
    const DeferredClear deferred{ &DekiRenderSystem::ClearFrame, this };
    const bool lazily = m_Renderer->ClearsLazily();
    RenderContext ctx{ camera, m_RenderBuffer, m_ScreenWidth, m_ScreenHeight, m_ColorFormat };
    ctx.contentFrame = m_ContentFrame;
    if (whole)
    {
        m_ClearRects = nullptr;
        if (!lazily)
        {
            ClearFrame();
        }
        ctx.clear = lazily ? &deferred : nullptr;
        m_Renderer->Render(scene, ctx);
    }
    else
    {
        // Each part cleared and drawn on its own. Parts may overlap; the
        // overlap is drawn twice, the same both times.
        for (const Deki::Rect& part : redraw.Rects())
        {
            m_ClearPart.Reset(m_ScreenWidth, m_ScreenHeight);
            m_ClearPart.Add(part);
            m_ClearRects = &m_ClearPart;
            if (!lazily)
            {
                ClearFrame();
            }
            ctx.redraw = part;
            ctx.clear = lazily ? &deferred : nullptr;
            m_Renderer->Render(scene, ctx);
        }
    }

    // Sent: what this frame changed. The screen shows the rest already.
    if (MostOfScreen(*changed))
    {
        m_PresentCount = -1;
    }
    else
    {
        m_PresentRects = changed->Rects();
        m_PresentCount = static_cast<int32_t>(m_PresentRects.size());
    }

    if (mine)
    {
        mine->owed.Reset(m_ScreenWidth, m_ScreenHeight);
    }
    else
    {
        OwedBuffer fresh;
        fresh.buffer = m_RenderBuffer;
        fresh.owed.Reset(m_ScreenWidth, m_ScreenHeight);
        m_Owed.push_back(fresh);
    }
    return true;
}

void DekiRenderSystem::RenderBands(Deki::Scene* scene, CameraComponent* camera, bool capture)
{
    // Every strip is sent here, so Engine::PresentFrame has nothing left.
    m_PresentCount = 0;

    Deki::IDisplay* display = Deki::Engine::GetInstance().GetDisplay();
    // The display's scan order, asked every frame: strips allocated for
    // another one (the display offered it, or stopped) are cut again.
    IScanOrderedDisplay* scan = FindScanOrderedDisplay(display);
    {
        const ScanOrder order = scan ? scan->GetScanOrder() : ScanOrder::RowsDown;
        const int32_t maxPixels = scan ? scan->GetMaxStripPixels() : 0;
        if (IsColumns(order) != m_BandColumns || IsReversed(order) != m_BandReverse || maxPixels != m_BandMaxPixels)
        {
            const int32_t asked = m_BandAsked;
            if (!SetBandRows(asked) || m_BandRows == 0)
            {
                DEKI_LOG_ERROR("[Rendering] the display's scan order changed and %d-pixel strips along it could not be "
                               "made; nothing is drawn",
                               static_cast<int>(asked));
                return;
            }
        }
    }
    if (!display || (!scan && !display->SupportsPartialPresent()))
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
    QuadBlit::NextFrame();

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

    // Redrawing only what changed: strips with changes are drawn and sent
    // only across them, the others not at all; past most of the screen, every
    // strip whole. Not while the frame is also kept or captured whole.
    const DirtyRegion* changes = (!keepWanted && !capture) ? FindFrameChanges(scene, camera, clearChanged) : nullptr;
    if (keepWanted || capture)
    {
        m_Owed.clear();
    }
    const bool partial = changes && !MostOfScreen(*changes);

    // Strips run along the scan order, in the order the panel refreshes
    // them. A strip's buffer is its own width: the screen's for rows, the
    // strip's for columns.
    const size_t bpp = static_cast<size_t>(GetBytesPerPixel(m_ColorFormat));
    const size_t pitch = static_cast<size_t>(m_ScreenWidth) * bpp;
    const int32_t across = m_BandColumns ? m_ScreenWidth : m_ScreenHeight;
    const int32_t strips = (across + m_BandRows - 1) / m_BandRows;
    bool firstSent = true;  // no strip of this frame sent yet
    for (int32_t s = 0; s < strips; ++s)
    {
        const int32_t start = (m_BandReverse ? strips - 1 - s : s) * m_BandRows;
        const int32_t size = std::min(m_BandRows, across - start);
        const Deki::Rect strip = m_BandColumns ? Deki::Rect{ start, 0, start + size, m_ScreenHeight }
                                               : Deki::Rect{ 0, start, m_ScreenWidth, start + size };
        // The part of the strip to draw and send: all of it, or what changed.
        Deki::Rect part = strip;
        if (partial)
        {
            part = Deki::Rect{ strip.right, strip.bottom, strip.left, strip.top };
            for (const Deki::Rect& r : changes->Rects())
            {
                const int32_t l = std::max(r.left, strip.left), t = std::max(r.top, strip.top);
                const int32_t rr = std::min(r.right, strip.right), b = std::min(r.bottom, strip.bottom);
                if (rr > l && b > t)
                {
                    part.left = std::min(part.left, l);
                    part.top = std::min(part.top, t);
                    part.right = std::max(part.right, rr);
                    part.bottom = std::max(part.bottom, b);
                }
            }
            if (part.Empty())
            {
                continue;
            }
            // The strip's whole length: a display may send more of a strip
            // than it is told (a turned panel sends whole panel rows).
            if (scan && m_BandColumns)
            {
                part.top = strip.top;
                part.bottom = strip.bottom;
            }
            else if (scan)
            {
                part.left = strip.left;
                part.right = strip.right;
            }
        }
        const int32_t sw = strip.Width();
        const int32_t sh = strip.Height();
        const size_t stripPitch = static_cast<size_t>(sw) * bpp;
        uint8_t* const wholeAt =
            whole ? whole + static_cast<size_t>(strip.top) * pitch + static_cast<size_t>(strip.left) * bpp : nullptr;
        // The strip's buffer: one the display lends, which it then sends as
        // it is, or our own.
        uint8_t* buffer = scan && scan->LendsStripBuffers() ? scan->GetStripBuffer(stripPitch * sh) : nullptr;
        const bool lent = buffer != nullptr;
        if (!lent)
        {
            if (!m_BandBuffer && !AllocateBand())
            {
                DEKI_LOG_ERROR("[Rendering] no internal memory for a strip; nothing is drawn");
                return;
            }
            buffer = m_BandBuffer;
        }
        RenderContext ctx{ camera, buffer, sw, sh, m_ColorFormat };
        ctx.viewTop = strip.top;
        ctx.viewHeight = m_ScreenHeight;
        ctx.viewLeft = strip.left;
        ctx.viewWidth = m_ScreenWidth;
        ctx.contentFrame = m_ContentFrame;
        if (partial)
        {
            ctx.redraw = Deki::Rect{ part.left - strip.left, part.top - strip.top, part.right - strip.left,
                                     part.bottom - strip.top };
        }

        // The strip's starting pixels: the kept frame's, or the clear colour
        // across what is drawn, and nothing when the renderer will cover all
        // of it (a full-screen background).
        const uint64_t clearStart = RenderProfiler::IsEnabled() ? RenderProfiler::Now() : 0;
        if (keep)
        {
            CopyPixels(buffer, stripPitch, wholeAt, pitch, stripPitch, sh);
        }
        else if (m_Renderer->CoversView(ctx))
        {
            // Nothing: every pixel is drawn over.
        }
        else if (partial)
        {
            FillRect(buffer, sw, sh, m_ColorFormat, ctx.redraw.left, ctx.redraw.top, ctx.redraw.Width(),
                     ctx.redraw.Height(), clear.r, clear.g, clear.b);
        }
        else
        {
            FillRect(buffer, sw, sh, m_ColorFormat, 0, 0, sw, sh, clear.r, clear.g, clear.b);
        }
        if (RenderProfiler::IsEnabled())
        {
            RenderProfiler::AddContent(RenderProfiler::kClear, RenderProfiler::Now() - clearStart);
        }

        m_Renderer->Render(scene, ctx);

        if (whole)
        {
            CopyPixels(wholeAt, pitch, buffer, stripPitch, stripPitch, sh);
        }

        const Deki::Rect send = part;
        const uint64_t presentStart = RenderProfiler::IsEnabled() ? RenderProfiler::Now() : 0;
        if (scan)
        {
            ScanStrip out;
            out.pixels = buffer;
            out.stride = sw;
            out.strip = strip;
            out.changed = send;
            out.format = m_ColorFormat;
            out.firstOfFrame = firstSent;
            firstSent = false;
            scan->PresentStrip(out);
        }
        else
        {
            // PresentRegions reads the rectangle's rows out of a whole frame.
            // The strip is exactly those rows, so it is handed over as a frame
            // whose row `top` starts at the strip: the display reads nothing
            // else.
            const auto* frameBase = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(buffer) -
                                                                     static_cast<uintptr_t>(strip.top) * pitch);
            display->PresentRegions(frameBase, m_ScreenWidth, m_ScreenHeight, m_ColorFormat, &send, 1);
        }
        if (RenderProfiler::IsEnabled())
        {
            RenderProfiler::AddContent(RenderProfiler::kPresent, RenderProfiler::Now() - presentStart, "Present");
        }
    }

    m_ForceFull = false;

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
    m_BandAsked = rows;
    const IScanOrderedDisplay* scan = CurrentScanDisplay();
    const ScanOrder order = scan ? scan->GetScanOrder() : ScanOrder::RowsDown;
    m_BandColumns = IsColumns(order);
    m_BandReverse = IsReversed(order);
    m_BandMaxPixels = scan ? scan->GetMaxStripPixels() : 0;
    const int32_t across = m_BandColumns ? m_ScreenWidth : m_ScreenHeight;
    if (rows <= 0 || across <= 0 || m_ScreenWidth <= 0 || m_ScreenHeight <= 0)
    {
        return true;  // whole frames
    }
    // Rows in whole pairs: some panels take writes only in pairs of rows (the
    // RM67162 AMOLED), and a strip widened to one would be read past. Columns
    // as many pixels as that many rows would be, or fewer (so no more memory:
    // a display keeps copies of its strips, and as many strips: each strip
    // costs a walk of the scene), in whole eights: a column strip starts the
    // ordered dither's 8x8 tile where the whole frame does only at a multiple
    // of 8. (A panel that is sent columns is sent whole panel rows of them;
    // its pairs are its own.)
    if (m_BandColumns)
    {
        const int64_t cols = static_cast<int64_t>(rows) * m_ScreenWidth / m_ScreenHeight;
        m_BandRows = static_cast<int32_t>(std::min<int64_t>(std::max<int64_t>(cols & ~int64_t{ 7 }, 8), across));
    }
    else
    {
        m_BandRows = (rows + 1) & ~1;
    }
    // No bigger than the display can send at full speed, in the same steps.
    const int32_t along = m_BandColumns ? m_ScreenHeight : m_ScreenWidth;
    const int32_t step = m_BandColumns ? 8 : 2;
    while (m_BandMaxPixels > 0 && m_BandRows > step && m_BandRows * along > m_BandMaxPixels)
    {
        m_BandRows -= step;
    }
    if (m_BandRows >= across)
    {
        m_BandRows = 0;
        return true;
    }
    // A display that lends strip buffers draws them in its own memory; ours
    // is made only if it stops.
    if (!(scan && scan->LendsStripBuffers()) && !AllocateBand())
    {
        m_BandRows = 0;
        return false;
    }
    s_FrameModeRows = m_BandRows;  // the strips as cut now, for DEKI:STATS
    if (m_BandColumns || m_BandReverse)
    {
        DEKI_LOG_INFO("[Rendering] strips of %d %s, %s, along the display's refresh", static_cast<int>(m_BandRows),
                      m_BandColumns ? "columns" : "rows",
                      m_BandColumns ? (m_BandReverse ? "right to left" : "left to right") : "bottom to top");
    }
    return true;
}

bool DekiRenderSystem::AllocateBand()
{
    // A strip of rows is the screen's width; a strip of columns its height.
    const size_t pixels = m_BandColumns ? static_cast<size_t>(m_ScreenHeight) * static_cast<size_t>(m_BandRows)
                                        : static_cast<size_t>(m_ScreenWidth) * static_cast<size_t>(m_BandRows);
    const size_t bytes = pixels * static_cast<size_t>(GetBytesPerPixel(m_ColorFormat));
    m_BandBuffer = static_cast<uint8_t*>(Deki::Memory::Allocate(bytes, Deki::Memory::Internal));
    return m_BandBuffer != nullptr;
}

void DekiRenderSystem::FreeBand()
{
    if (m_BandBuffer)
    {
        Deki::Memory::Free(m_BandBuffer);
        m_BandBuffer = nullptr;
    }
    m_BandRows = 0;
    FreeWholeFrame();
}

void DekiRenderSystem::ResetHistory()
{
    m_Owed.clear();
    m_HaveClearColor = false;
    m_ForceFull = true;
    m_PresentCount = -1;
}

void DekiRenderSystem::SetRedrawChanges(bool enabled)
{
    m_RedrawChanges = enabled;
    ResetHistory();
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

void DekiRenderSystem::ClearFrame()
{
    const uint64_t clearStart = RenderProfiler::IsEnabled() ? RenderProfiler::Now() : 0;
    if (!m_ClearRects)
    {
        ClearBuffer(m_ClearColor);
    }
    else
    {
        for (const Deki::Rect& r : m_ClearRects->Rects())
        {
            ClearRect(r.left, r.top, r.Width(), r.Height(), m_ClearColor.r, m_ClearColor.g, m_ClearColor.b);
        }
    }
    if (RenderProfiler::IsEnabled())
    {
        RenderProfiler::AddContent(RenderProfiler::kClear, RenderProfiler::Now() - clearStart);
    }
}

void DekiRenderSystem::ClearFrame(void* owner)
{
    static_cast<DekiRenderSystem*>(owner)->ClearFrame();
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
