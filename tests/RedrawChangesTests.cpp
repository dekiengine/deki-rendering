// Redrawing only what changed (DekiRenderSystem::SetRedrawChanges). Each test
// changes a scene frame after frame and checks that what reaches the display
// is exactly a frame drawn whole, every time, while less is drawn and sent
// when little changed:
// - whole frames, from one buffer or from two a display alternates;
// - strips of rows sent as regions, and strips of columns a display lends;
// - moves, see-through overlaps, a swap of draw order, content changed in
//   place, objects that go, a component that cannot say when it changes, a
//   pass that cannot say where it draws, a moving camera.

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include <deki/Engine.h>
#include <deki/Object.h>
#include <deki/Scene.h>
#include <deki/providers/IDisplay.h>
#include "CameraComponent.h"
#include "DekiRenderSystem.h"
#include "DirtyRegion.h"
#include "PixelFormat.h"
#include "RendererComponent.h"
#include "RenderProfiler.h"
#include "RenderPass.h"
#include "ScopedArtDensity.h"
#include "Standard2DRenderer.h"

// Tests name the package's types unqualified.
using namespace DekiRendering;

namespace
{

constexpr int kW = 64, kH = 48;
const Deki::Color kClear(0, 0, 64);

// A 6x6 square (0.375 m at 16 px/m) with a soft edge, in one colour. Says
// when its pixels change in place (GetContentVersion) unless `knowsVersion`
// is off.
class Square : public RendererComponent
{
public:
    // Hand-written, not reflected: the class sets its own type info.
    static const Deki::ComponentTypeInfo kTypeInfo;
    Square() { SetTypeInfo(&kTypeInfo); }

    bool knowsVersion = true;
    uint8_t alpha = 255;  // the tint's alpha

    void SetColour(uint16_t c)
    {
        m_Colour = c;
        ++m_Version;
        m_Revision = QuadBlit::NextRevision();
    }

    // A ring this wide around it left fully transparent: its frame is bigger
    // than what it shows, as an animation frame often is.
    void SetMargin(int m)
    {
        m_Margin = m;
        ++m_Version;
        m_Revision = QuadBlit::NextRevision();
    }

    // Solid to its edge, its four corner pixels left transparent, as a
    // rounded panel is.
    void SetRounded()
    {
        m_Rounded = true;
        ++m_Version;
        m_Revision = QuadBlit::NextRevision();
    }

    bool GetContentExtents(float& outW, float& outH) const override
    {
        outW = outH = 6.0f / 16.0f;
        return true;
    }

    bool RenderContent(const Deki::Object*, QuadBlit::Source& outSource, float& outPivotX, float& outPivotY,
                       uint8_t& tr, uint8_t& tg, uint8_t& tb, uint8_t& ta) override
    {
        // The same buffer every frame, refilled: a change shows only in the
        // version.
        for (int y = 0; y < 6; ++y)
        {
            for (int x = 0; x < 6; ++x)
            {
                uint8_t* p = &m_Pixels[(y * 6 + x) * 3];
                DekiPixel::Store565(p, m_Colour);
                const int m = m_Margin;
                const bool outside = x < m || y < m || x > 5 - m || y > 5 - m;
                const bool edge = x == m || y == m || x == 5 - m || y == 5 - m;
                p[2] = outside ? 0 : (edge ? 128 : 255);
                if (m_Rounded)
                {
                    // Solid to its edge but for the corners.
                    p[2] = (x == 0 || x == 5) && (y == 0 || y == 5) ? 0 : 255;
                }
            }
        }
        outSource = QuadBlit::MakeSource(m_Pixels, 6, 6, QuadBlit::PixelLayout::RGB565A8(), false, nullptr);
        outSource.pixelsPerMeter = 16.0f;
        outSource.revision = knowsVersion ? m_Revision : 0;
        outPivotX = outPivotY = 0.5f;
        tr = tg = tb = 255;
        ta = alpha;
        return true;
    }

    bool GetContentVersion(uint32_t& outVersion) const override
    {
        outVersion = m_Version;
        return knowsVersion;
    }

private:
    uint16_t m_Colour = 0xF800;
    uint32_t m_Version = 1;
    uint32_t m_Revision = QuadBlit::NextRevision();
    int m_Margin = 0;
    bool m_Rounded = false;
    uint8_t m_Pixels[36 * 3] = {};
};
const Deki::ComponentTypeInfo Square::kTypeInfo =
    Deki::MakeHandWrittenTypeInfo<Square, RendererComponent>(/*declaresUpdate=*/false);

// Keeps the frame it is sent and counts the pixels. Offered as an
// IScanOrderedDisplay, it is sent strips along `order`; it can lend strip
// buffers, and alternate two whole buffers for the render system to draw in.
class FrameDisplay : public Deki::IDisplay, public IScanOrderedDisplay
{
public:
    ScanOrder order = ScanOrder::RowsDown;
    bool lend = false;
    bool alternate = false;  // GetRenderBuffer hands out two buffers in turn
    std::vector<uint8_t> lent[2];
    int nextLent = 0;
    int lentOut = -1;
    std::vector<uint8_t> buffers[2];
    int nextBuffer = 0;
    std::vector<uint8_t> frame;
    int64_t pixelsSent = 0;

    FrameDisplay()
    {
        frame.assign(static_cast<size_t>(kW) * kH * 2, 0x5A);  // nothing a frame draws
        buffers[0].assign(frame.size(), 0x33);
        buffers[1].assign(frame.size(), 0x66);
    }

    bool Initialize(int32_t, int32_t) override { return true; }
    void Shutdown() override {}
    uint8_t* GetRenderBuffer(int32_t* width, int32_t* height) override
    {
        if (!alternate)
        {
            return nullptr;
        }
        *width = kW;
        *height = kH;
        uint8_t* b = buffers[nextBuffer].data();
        nextBuffer ^= 1;
        return b;
    }
    void Present(const uint8_t* fb, int, int, Deki::ColorFormat) override
    {
        std::memcpy(frame.data(), fb, frame.size());
        pixelsSent += static_cast<int64_t>(kW) * kH;
    }
    bool SupportsPartialPresent() const override { return true; }
    void PresentRegions(const uint8_t* fb, int width, int, Deki::ColorFormat, const Deki::Rect* rects,
                        int32_t count) override
    {
        for (int32_t i = 0; i < count; ++i)
        {
            const Deki::Rect& r = rects[i];
            pixelsSent += static_cast<int64_t>(r.Width()) * r.Height();
            for (int32_t y = r.top; y < r.bottom; ++y)
            {
                const size_t at = (static_cast<size_t>(y) * width + r.left) * 2;
                std::memcpy(&frame[at], fb + at, static_cast<size_t>(r.Width()) * 2);
            }
        }
    }
    ScanOrder GetScanOrder() const override { return order; }
    int32_t GetMaxStripPixels() const override { return 0; }
    bool LendsStripBuffers() const override { return lend; }
    uint8_t* GetStripBuffer(size_t bytes) override
    {
        lentOut = nextLent;
        nextLent ^= 1;
        lent[lentOut].resize(std::max(lent[lentOut].size(), bytes), 0x77);
        return lent[lentOut].data();
    }
    void PresentStrip(const ScanStrip& s) override
    {
        lentOut = -1;
        // A turned panel sends whole panel rows: here, the strip's whole
        // length across what changed, from the strip as it is.
        Deki::Rect r = s.changed;
        if (r.Empty())
        {
            return;
        }
        if (order == ScanOrder::ColumnsRight || order == ScanOrder::ColumnsLeft)
        {
            r.top = s.strip.top;
            r.bottom = s.strip.bottom;
        }
        pixelsSent += static_cast<int64_t>(r.Width()) * r.Height();
        for (int32_t y = r.top; y < r.bottom; ++y)
        {
            std::memcpy(&frame[(static_cast<size_t>(y) * kW + r.left) * 2],
                        s.pixels + (static_cast<size_t>(y - s.strip.top) * s.stride + (r.left - s.strip.left)) * 2,
                        static_cast<size_t>(r.Width()) * 2);
        }
    }
    void GetDisplaySize(int32_t* w, int32_t* h) const override
    {
        *w = kW;
        *h = kH;
    }
    Deki::ColorFormat GetColorFormat() const override { return Deki::ColorFormat::RGB565; }
    bool IsInitialized() const override { return true; }
    void RequestFullRefresh() override {}
    bool ProcessEvents() override { return true; }
    void* CreateUIOverlay(int32_t, int32_t) override { return nullptr; }
    bool UpdateUIOverlay(void*, int32_t, int32_t, int32_t, int32_t, const uint32_t*) override { return false; }
    bool UpdateUIOverlayRGB565A8(void*, int32_t, int32_t, int32_t, int32_t, const uint8_t*) override { return false; }
    void DestroyUIOverlay(void*) override {}
    void SetActiveUIOverlay(void*) override {}
    void ClearActiveUIOverlay() override {}
};

// Darkens a pixel per object it is given, drawn straight into the buffer: it
// cannot say where it draws.
class PlainPass : public RenderPass
{
public:
    uint32_t HookMask() const override { return RenderPassHooks::Execute; }
    void Execute(Deki::Object*, RenderContext& ctx) override
    {
        if (ctx.viewTop == 0 && ctx.viewLeft == 0 && ctx.buffer)
        {
            ctx.buffer[0] = 0;
            ctx.buffer[1] = 0;
        }
    }
};

enum class Mode
{
    WholeFrames,
    AlternatingBuffers,
    RowStrips,
    ColumnStripsLent,
};

// One scene drawn two ways: `reference`, whole frames into a buffer of our
// own, cleared every time; and `system`, redrawing only what changed, whose
// frames reach `display`.
struct Fixture
{
    ScopedArtDensity art;
    FrameDisplay display;
    Deki::IDisplay* previous = nullptr;
    Deki::Scene scene;
    CameraComponent* camera = nullptr;
    Standard2DRenderer referenceRenderer;
    Standard2DRenderer renderer;
    DekiRenderSystem system;
    Mode mode;
    std::vector<uint8_t> reference;

    explicit Fixture(Mode m)
        : mode(m)
    {
        display.alternate = m == Mode::AlternatingBuffers;
        display.lend = m == Mode::ColumnStripsLent;
        display.order = m == Mode::ColumnStripsLent ? ScanOrder::ColumnsRight : ScanOrder::RowsDown;
        Deki::Engine& engine = Deki::Engine::GetInstance();
        previous = engine.GetDisplay();
        engine.SetDisplay(&display, "redraw changes");
        if (m == Mode::ColumnStripsLent)
        {
            RegisterScanOrderedDisplay(&display, &display);
        }

        auto* camObj = new Deki::Object("Camera");
        camera = camObj->AddComponent<CameraComponent>();
        camera->orthoHeight = 3.0f;  // 16 px/m on 64 x 48
        camera->clearColor = kClear;
        scene.AddObject(camObj);

        EXPECT_TRUE(system.Setup(kW, kH, Deki::ColorFormat::RGB565));
        system.SetRenderer(&renderer);
        if (m == Mode::RowStrips || m == Mode::ColumnStripsLent)
        {
            EXPECT_TRUE(system.SetBandRows(8));
        }
        system.SetRedrawChanges(true);
        system.SetPartCostPixels(0);  // parts kept apart, so they are tested apart
    }
    ~Fixture()
    {
        RegisterScanOrderedDisplay(&display, nullptr);
        Deki::Engine::GetInstance().SetDisplay(previous, previous ? "restored" : "");
    }

    Square* Add(const char* name, float x, float y, uint16_t colour, int order = 0)
    {
        auto* o = new Deki::Object(name);
        o->SetX(x);
        o->SetY(y);
        auto* s = o->AddComponent<Square>();
        s->SetColour(colour);
        s->sortingOrder = order;
        scene.AddObject(o);
        return s;
    }

    // Draws a frame both ways and sends `system`'s as Engine::PresentFrame
    // would (in strips it has nothing left to send). Returns the pixels sent.
    int64_t Frame()
    {
        reference.assign(static_cast<size_t>(kW) * kH * 2, 0);
        const uint16_t clear = DekiPixel::PackRGB565(camera->clearColor.r, camera->clearColor.g, camera->clearColor.b);
        for (size_t i = 0; i < reference.size(); i += 2)
        {
            DekiPixel::Store565(&reference[i], clear);
        }
        RenderContext ctx{ camera, reference.data(), kW, kH, Deki::ColorFormat::RGB565 };
        referenceRenderer.Render(&scene, ctx);

        display.pixelsSent = 0;
        system.Render(&scene);
        int32_t count = -1;
        const Deki::Rect* rects = system.GetPresentRects(&count);
        const uint8_t* fb = system.GetFrameBuffer();
        if (count < 0)
        {
            display.Present(fb, kW, kH, Deki::ColorFormat::RGB565);
        }
        else if (count > 0)
        {
            display.PresentRegions(fb, kW, kH, Deki::ColorFormat::RGB565, rects, count);
        }
        return display.pixelsSent;
    }

    bool Shows() const { return display.frame == reference; }
};

const Mode kModes[] = { Mode::WholeFrames, Mode::AlternatingBuffers, Mode::RowStrips, Mode::ColumnStripsLent };

const char* Name(Mode m)
{
    switch (m)
    {
        case Mode::WholeFrames: return "whole frames";
        case Mode::AlternatingBuffers: return "alternating buffers";
        case Mode::RowStrips: return "strips of rows";
        case Mode::ColumnStripsLent: return "lent strips of columns";
    }
    return "?";
}

}  // namespace

// A square moves over a still one; a see-through one sits on both. Every
// frame matches a whole frame, small moves send a fraction of the screen,
// and a frame where nothing moved sends nothing.
TEST(RedrawChanges, MovesMatchWholeFramesAndSendLittle)
{
    for (Mode mode : kModes)
    {
        Fixture f(mode);
        Square* still = f.Add("still", 0.2f, -0.1f, 0x07E0);
        Square* glass = f.Add("glass", 0.0f, 0.0f, 0xFFE0, 5);
        glass->alpha = 120;
        Square* mover = f.Add("mover", -1.5f, 0.6f, 0xF800, 2);
        (void)still;

        f.Frame();
        EXPECT_TRUE(f.Shows()) << Name(mode) << ": the first frame";
        for (int i = 1; i < 15; ++i)
        {
            mover->GetOwner()->SetX(-1.5f + 0.21f * i);
            mover->GetOwner()->SetY(0.6f - 0.08f * i);
            const int64_t sent = f.Frame();
            EXPECT_TRUE(f.Shows()) << Name(mode) << ", frame " << i;
            EXPECT_GT(sent, 0) << Name(mode) << ", frame " << i;
            EXPECT_LT(sent, kW * kH / 2) << Name(mode) << ", frame " << i;
        }
        // Twice, so a buffer drawn the frame before is up to date too.
        f.Frame();
        EXPECT_EQ(f.Frame(), 0) << Name(mode) << ": nothing moved";
        EXPECT_TRUE(f.Shows()) << Name(mode);
    }
}

// Pixels changed in place, behind the same buffer, are redrawn when the
// version says so; and so is a component that cannot say, every frame.
TEST(RedrawChanges, ContentChangedInPlaceIsRedrawn)
{
    for (Mode mode : kModes)
    {
        Fixture f(mode);
        Square* a = f.Add("a", -0.8f, 0.4f, 0xF800);
        Square* b = f.Add("b", 0.9f, -0.5f, 0x001F);
        b->knowsVersion = false;
        f.Frame();
        f.Frame();

        a->SetColour(0x07FF);
        EXPECT_GT(f.Frame(), 0) << Name(mode);
        EXPECT_TRUE(f.Shows()) << Name(mode);

        // b cannot say, so it is sent every frame, and only it.
        const int64_t sent = f.Frame();
        EXPECT_GT(sent, 0) << Name(mode);
        EXPECT_LT(sent, kW * kH / 2) << Name(mode);
        EXPECT_TRUE(f.Shows()) << Name(mode);
    }
}

// Two see-through squares overlap; swapping their draw order changes the
// overlap's colour, and the swap is drawn.
TEST(RedrawChanges, ASwapOfDrawOrderIsRedrawn)
{
    for (Mode mode : kModes)
    {
        Fixture f(mode);
        Square* a = f.Add("a", 0.0f, 0.0f, 0xF800, 1);
        Square* b = f.Add("b", 0.15f, 0.1f, 0x001F, 2);
        a->alpha = 160;
        b->alpha = 160;
        f.Frame();
        f.Frame();
        a->sortingOrder = 3;
        f.Frame();
        EXPECT_TRUE(f.Shows()) << Name(mode);
        f.Frame();
        EXPECT_TRUE(f.Shows()) << Name(mode);
    }
}

// Objects that go leave nothing behind; a clear colour of another colour and
// MarkAllDirty send everything.
TEST(RedrawChanges, GoneObjectsAndNewClearColours)
{
    for (Mode mode : kModes)
    {
        Fixture f(mode);
        Square* a = f.Add("a", -0.8f, 0.4f, 0xF800);
        Square* b = f.Add("b", 0.9f, -0.5f, 0x001F);
        f.Frame();
        f.Frame();

        a->GetOwner()->SetActive(false);
        EXPECT_GT(f.Frame(), 0) << Name(mode);
        EXPECT_TRUE(f.Shows()) << Name(mode);
        f.scene.RemoveObject(b->GetOwner());
        EXPECT_GT(f.Frame(), 0) << Name(mode);
        EXPECT_TRUE(f.Shows()) << Name(mode);
        f.Frame();
        EXPECT_TRUE(f.Shows()) << Name(mode);

        f.camera->clearColor = Deki::Color(40, 0, 0);
        EXPECT_EQ(f.Frame(), kW * kH) << Name(mode);
        EXPECT_TRUE(f.Shows()) << Name(mode);
        f.system.MarkAllDirty();
        EXPECT_EQ(f.Frame(), kW * kH) << Name(mode);
        EXPECT_TRUE(f.Shows()) << Name(mode);
    }
}

// A moving camera moves everything: whole frames.
TEST(RedrawChanges, AMovingCameraDrawsWholeFrames)
{
    for (Mode mode : kModes)
    {
        Fixture f(mode);
        f.Add("a", -0.8f, 0.4f, 0xF800);
        f.Add("b", 0.9f, -0.5f, 0x001F);
        f.Frame();
        for (int i = 1; i < 4; ++i)
        {
            f.camera->GetOwner()->SetX(0.1f * i);
            EXPECT_EQ(f.Frame(), kW * kH) << Name(mode) << ", frame " << i;
            EXPECT_TRUE(f.Shows()) << Name(mode) << ", frame " << i;
        }
    }
}

// A camera that moves by whole pixels, and sits on whole pixels, moves the
// last frame instead of drawing it again: every frame still matches one drawn
// whole, with a square moving on its own and a see-through one over others.
TEST(RedrawChanges, AWholePixelScrollMovesTheFrame)
{
    for (Mode mode : kModes)
    {
        Fixture f(mode);
        f.Add("still", 0.2f, -0.1f, 0x07E0);
        Square* glass = f.Add("glass", 0.0f, 0.0f, 0xFFE0, 5);
        glass->alpha = 120;
        Square* mover = f.Add("mover", -1.5f, 0.6f, 0xF800, 2);
        f.Add("far", 2.4f, 0.9f, 0x001F);
        f.Add("on the edge", -1.95f, -0.4f, 0xF81F);  // half off the left of the view
        f.Add("on the top", 0.5f, 1.45f, 0x07FF);     // and one off the top
        f.Frame();
        EXPECT_TRUE(f.Shows()) << Name(mode) << ": the first frame";
        // 1/16 m is one pixel at 16 px/m: a few pixels a frame, every way.
        const int steps[][2] = { { 1, 0 }, { 2, 0 }, { 0, 1 }, { -3, 2 }, { 0, -2 }, { 4, -1 }, { -1, -1 }, { 0, 0 } };
        float camX = 0.0f, camY = 0.0f;
        int frame = 0;
        for (const auto& step : steps)
        {
            camX += step[0] / 16.0f;
            camY += step[1] / 16.0f;
            f.camera->GetOwner()->SetX(camX);
            f.camera->GetOwner()->SetY(camY);
            mover->GetOwner()->SetX(-1.5f + 0.13f * ++frame);
            f.Frame();
            EXPECT_TRUE(f.Shows()) << Name(mode) << ", step " << frame;
            if (mode == Mode::WholeFrames && (step[0] != 0 || step[1] != 0))
            {
                int32_t dx = 0, dy = 0;
                EXPECT_TRUE(f.renderer.ScrolledBy(dx, dy)) << "step " << frame;
                EXPECT_EQ(dx, -step[0]) << "step " << frame;
                EXPECT_EQ(dy, step[1]) << "step " << frame;
            }
        }
    }
}

// Squares across the view's left and top edges scroll along with the frame:
// only the edges the move uncovers are drawn, not each square again.
TEST(RedrawChanges, ObjectsAcrossTheEdgesScrollAlong)
{
    ScopedArtDensity art;
    Deki::Scene scene;
    auto* camObj = new Deki::Object("Camera");
    CameraComponent* camera = camObj->AddComponent<CameraComponent>();
    camera->orthoHeight = 3.0f;
    scene.AddObject(camObj);
    for (const float* at : { std::array<float, 2>{ -1.95f, -0.4f }.data(), std::array<float, 2>{ 0.5f, 1.45f }.data() })
    {
        auto* o = new Deki::Object("edge");
        o->AddComponent<Square>();
        o->SetX(at[0]);
        o->SetY(at[1]);
        scene.AddObject(o);
    }

    Standard2DRenderer renderer;
    RenderContext ctx{ camera, nullptr, kW, kH, Deki::ColorFormat::RGB565 };
    ctx.scrollable = true;
    DirtyRegion changed;
    ctx.contentFrame = 1;
    ASSERT_TRUE(renderer.FindChanges(&scene, ctx, changed));

    // One pixel right and one down: the frame moves left and up.
    camObj->SetX(1.0f / 16.0f);
    camObj->SetY(-1.0f / 16.0f);
    ctx.contentFrame = 2;
    ASSERT_TRUE(renderer.FindChanges(&scene, ctx, changed));
    ASSERT_FALSE(changed.IsFull());
    int32_t dx = 0, dy = 0;
    ASSERT_TRUE(renderer.ScrolledBy(dx, dy));
    EXPECT_TRUE(changed.Contains(kW - 1, 10)) << "the right edge, uncovered";
    EXPECT_TRUE(changed.Contains(10, kH - 1)) << "the bottom edge, uncovered";
    EXPECT_FALSE(changed.Contains(1, 29)) << "the square on the left edge";
    EXPECT_FALSE(changed.Contains(39, 1)) << "the square on the top edge";
}

// A frame bigger than what it shows changes only where it shows: moving a
// square with a transparent margin redraws its visible box, not its frame.
TEST(RedrawChanges, OnlyWhatADrawShowsIsItsArea)
{
    ScopedArtDensity art;
    Deki::Scene scene;
    auto* camObj = new Deki::Object("Camera");
    CameraComponent* camera = camObj->AddComponent<CameraComponent>();
    camera->orthoHeight = 3.0f;
    scene.AddObject(camObj);
    auto* o = new Deki::Object("framed");
    Square* square = o->AddComponent<Square>();
    square->SetMargin(2);  // shows only its middle 2x2
    scene.AddObject(o);

    Standard2DRenderer renderer;
    RenderContext ctx{ camera, nullptr, kW, kH, Deki::ColorFormat::RGB565 };
    DirtyRegion changed;
    ctx.contentFrame = 1;
    ASSERT_TRUE(renderer.FindChanges(&scene, ctx, changed));
    o->SetX(4.0f / 16.0f);  // four pixels right
    ctx.contentFrame = 2;
    ASSERT_TRUE(renderer.FindChanges(&scene, ctx, changed));
    ASSERT_FALSE(changed.IsFull());
    // Its frame spans x 29..34 then 33..38, y 21..26; it shows x 31..32 then
    // 35..36, y 23..24.
    EXPECT_TRUE(changed.Contains(31, 23));
    EXPECT_TRUE(changed.Contains(36, 24));
    EXPECT_FALSE(changed.Contains(29, 21)) << "the margin it never writes";
    EXPECT_FALSE(changed.Contains(38, 26));
    EXPECT_EQ(changed.CoveredArea(), 8);
}

// The same square, drawn whole each frame and in changes, looks the same.
TEST(RedrawChanges, AFramedSquareMovingDrawsRight)
{
    for (Mode mode : kModes)
    {
        Fixture f(mode);
        Square* framed = f.Add("framed", -0.5f, 0.1f, 0xF800, 5);
        framed->SetMargin(1);
        f.Add("under", -0.3f, 0.1f, 0x07E0, 0);
        f.Frame();
        for (int i = 1; i <= 6; ++i)
        {
            framed->GetOwner()->SetX(-0.5f + 0.0625f * 3 * i);
            f.Frame();
            EXPECT_TRUE(f.Shows()) << Name(mode) << ", step " << i;
        }
    }
}

// A HUD square that stays on screen as the camera scrolls keeps the pixels of
// its opaque middle: only its soft edge and the band it leaves are drawn.
TEST(RedrawChanges, WhatStaysOnScreenKeepsItsOpaqueMiddle)
{
    ScopedArtDensity art;
    Deki::Scene scene;
    auto* camObj = new Deki::Object("Camera");
    CameraComponent* camera = camObj->AddComponent<CameraComponent>();
    camera->orthoHeight = 3.0f;
    scene.AddObject(camObj);
    auto* hud = new Deki::Object("hud");
    hud->AddComponent<Square>()->sortingOrder = 10;
    scene.AddObject(hud);

    Standard2DRenderer renderer;
    RenderContext ctx{ camera, nullptr, kW, kH, Deki::ColorFormat::RGB565 };
    ctx.scrollable = true;
    DirtyRegion changed;
    ctx.contentFrame = 1;
    ASSERT_TRUE(renderer.FindChanges(&scene, ctx, changed));
    ctx.contentFrame = 2;
    ASSERT_TRUE(renderer.FindChanges(&scene, ctx, changed));  // its record, still

    // Three pixels right, the square with the camera: the frame moves left.
    camObj->SetX(3.0f / 16.0f);
    hud->SetX(3.0f / 16.0f);
    ctx.contentFrame = 3;
    ASSERT_TRUE(renderer.FindChanges(&scene, ctx, changed));
    ASSERT_FALSE(changed.IsFull());
    int32_t dx = 0, dy = 0;
    ASSERT_TRUE(renderer.ScrolledBy(dx, dy));
    ASSERT_EQ(renderer.ScrollPins().size(), 1u);
    const Deki::Rect pin = renderer.ScrollPins()[0];
    EXPECT_EQ(pin.Width(), 4);
    EXPECT_EQ(pin.Height(), 4);
    EXPECT_FALSE(changed.Contains(pin.left + 1, pin.top + 1)) << "its opaque middle";
    EXPECT_TRUE(changed.Contains(pin.left - 1, pin.top + 1)) << "its soft edge";
    EXPECT_TRUE(changed.Contains(pin.left - 3, pin.top + 1)) << "where its pixels moved to";
}

// Something staying put in the world, drawn over a pin, moved across it on
// screen: where it was over the pin is drawn again, as is where it is now.
TEST(RedrawChanges, WhatIsDrawnOverAPinRedrawsIt)
{
    ScopedArtDensity art;
    Deki::Scene scene;
    auto* camObj = new Deki::Object("Camera");
    CameraComponent* camera = camObj->AddComponent<CameraComponent>();
    camera->orthoHeight = 3.0f;
    scene.AddObject(camObj);
    auto* hud = new Deki::Object("hud");
    hud->AddComponent<Square>()->sortingOrder = 10;
    scene.AddObject(hud);
    auto* over = new Deki::Object("over");
    Square* overSquare = over->AddComponent<Square>();
    overSquare->sortingOrder = 20;
    overSquare->alpha = 128;  // no pin of its own
    over->SetX(2.0f / 16.0f);  // two pixels right of the HUD, overlapping it
    scene.AddObject(over);

    Standard2DRenderer renderer;
    RenderContext ctx{ camera, nullptr, kW, kH, Deki::ColorFormat::RGB565 };
    ctx.scrollable = true;
    DirtyRegion changed;
    ctx.contentFrame = 1;
    ASSERT_TRUE(renderer.FindChanges(&scene, ctx, changed));
    ctx.contentFrame = 2;
    ASSERT_TRUE(renderer.FindChanges(&scene, ctx, changed));

    camObj->SetX(1.0f / 16.0f);
    hud->SetX(1.0f / 16.0f);
    ctx.contentFrame = 3;
    ASSERT_TRUE(renderer.FindChanges(&scene, ctx, changed));
    ASSERT_EQ(renderer.ScrollPins().size(), 1u);
    const Deki::Rect pin = renderer.ScrollPins()[0];
    // The square over it sat over the pin's right three columns last frame,
    // and over its right two now: those columns are drawn again.
    EXPECT_TRUE(changed.Contains(pin.right - 1, pin.top + 1));
    EXPECT_TRUE(changed.Contains(pin.right - 3, pin.top + 1));
}

// The same, frame after frame, with squares passing under and over it, one
// of them staying put in the world: every frame matches one drawn whole.
TEST(RedrawChanges, AHudOverAScrollingWorldDrawsRight)
{
    for (Mode mode : kModes)
    {
        Fixture f(mode);
        Square* hud = f.Add("hud", 0.4f, 0.3f, 0xFFE0, 10);
        Square* under = f.Add("under", -1.0f, 0.3f, 0xF800, 0);
        Square* over = f.Add("over", 1.2f, 0.25f, 0x07E0, 20);
        f.Add("over, still", 0.45f, 0.35f, 0x001F, 30);
        Square* text = f.Add("on it", 0.4f, 0.3f, 0xF81F, 15);  // stays on screen too
        text->alpha = 100;
        f.Frame();
        EXPECT_TRUE(f.Shows()) << Name(mode) << ": the first frame";
        const int steps[][2] = { { 1, 0 }, { 2, 1 }, { 0, -2 }, { -3, 1 }, { -1, -1 }, { 2, 2 }, { 0, 0 }, { 1, 0 } };
        float camX = 0.0f, camY = 0.0f;
        int frame = 0;
        for (const auto& step : steps)
        {
            camX += step[0] / 16.0f;
            camY += step[1] / 16.0f;
            ++frame;
            f.camera->GetOwner()->SetX(camX);
            f.camera->GetOwner()->SetY(camY);
            hud->GetOwner()->SetX(0.4f + camX);
            hud->GetOwner()->SetY(0.3f + camY);
            text->GetOwner()->SetX(0.4f + camX);
            text->GetOwner()->SetY(0.3f + camY);
            under->GetOwner()->SetX(-1.0f + 0.2f * frame);
            over->GetOwner()->SetX(1.2f - 0.15f * frame);
            if (frame == 5)
            {
                hud->SetColour(0x07FF);  // changed in place, on screen
            }
            f.Frame();
            EXPECT_TRUE(f.Shows()) << Name(mode) << ", step " << frame;
            if (mode == Mode::WholeFrames && (step[0] != 0 || step[1] != 0) && frame != 5)
            {
                EXPECT_FALSE(f.renderer.ScrollPins().empty()) << "step " << frame;
            }
        }
    }
}

// A HUD panel with rounded corners over a scrolling world: what its corners
// show of the world moves under it, and every frame matches one drawn whole.
TEST(RedrawChanges, ARoundedHudOverAScrollingWorldDrawsRight)
{
    for (Mode mode : kModes)
    {
        Fixture f(mode);
        Square* hud = f.Add("hud", 0.4f, 0.3f, 0xFFE0, 10);
        hud->SetRounded();
        Square* under = f.Add("under", -1.0f, 0.3f, 0xF800, 0);
        f.Frame();
        EXPECT_TRUE(f.Shows()) << Name(mode) << ": the first frame";
        const int steps[][2] = { { 1, 0 }, { 2, 1 }, { 0, -2 }, { -3, 1 }, { 1, 1 } };
        float camX = 0.0f, camY = 0.0f;
        int frame = 0;
        for (const auto& step : steps)
        {
            camX += step[0] / 16.0f;
            camY += step[1] / 16.0f;
            ++frame;
            f.camera->GetOwner()->SetX(camX);
            f.camera->GetOwner()->SetY(camY);
            hud->GetOwner()->SetX(0.4f + camX);
            hud->GetOwner()->SetY(0.3f + camY);
            under->GetOwner()->SetX(-1.0f + 0.2f * frame);
            f.Frame();
            EXPECT_TRUE(f.Shows()) << Name(mode) << ", step " << frame;
        }
    }
}

// A camera off the pixel grid cannot move the frame by whole pixels: the
// frame is drawn whole, and right.
TEST(RedrawChanges, ACameraBetweenPixelsDrawsWholeFrames)
{
    Fixture f(Mode::WholeFrames);
    f.Add("a", -0.8f, 0.4f, 0xF800);
    f.Add("b", 0.9f, -0.5f, 0x001F);
    f.camera->GetOwner()->SetX(0.03f);
    f.Frame();
    f.camera->GetOwner()->SetX(0.03f + 1.0f / 16.0f);
    EXPECT_EQ(f.Frame(), kW * kH);
    int32_t dx = 0, dy = 0;
    EXPECT_FALSE(f.renderer.ScrolledBy(dx, dy));
    EXPECT_TRUE(f.Shows());
}

// A pass that cannot say where it draws makes every frame whole, and right.
TEST(RedrawChanges, APassThatCannotSayDrawsWholeFrames)
{
    for (Mode mode : { Mode::WholeFrames, Mode::RowStrips })
    {
        Fixture f(mode);
        PlainPass pass;
        f.renderer.AddPass(&pass);
        f.referenceRenderer.AddPass(&pass);
        f.Add("a", -0.8f, 0.4f, 0xF800);
        f.Frame();
        for (int i = 0; i < 3; ++i)
        {
            EXPECT_EQ(f.Frame(), kW * kH) << Name(mode);
            EXPECT_TRUE(f.Shows()) << Name(mode);
        }
    }
}

// The renderer's own answer: nothing on a second look, the old and new
// places of a move, everything for a new camera.
TEST(RedrawChanges, FindChangesReportsOldAndNewPlaces)
{
    ScopedArtDensity art;
    Deki::Scene scene;
    auto* camObj = new Deki::Object("Camera");
    CameraComponent* camera = camObj->AddComponent<CameraComponent>();
    camera->orthoHeight = 3.0f;
    scene.AddObject(camObj);
    auto* o = new Deki::Object("a");
    o->AddComponent<Square>();
    scene.AddObject(o);

    Standard2DRenderer renderer;
    RenderContext ctx{ camera, nullptr, kW, kH, Deki::ColorFormat::RGB565 };
    DirtyRegion changed;
    ctx.contentFrame = 1;
    ASSERT_TRUE(renderer.FindChanges(&scene, ctx, changed));
    EXPECT_TRUE(changed.IsFull()) << "nothing to compare with";

    ctx.contentFrame = 2;
    ASSERT_TRUE(renderer.FindChanges(&scene, ctx, changed));
    EXPECT_TRUE(changed.IsEmpty());

    o->SetX(1.0f);  // 16 px right
    ctx.contentFrame = 3;
    ASSERT_TRUE(renderer.FindChanges(&scene, ctx, changed));
    ASSERT_FALSE(changed.IsFull());
    EXPECT_TRUE(changed.Contains(kW / 2, kH / 2)) << "where it was";
    EXPECT_TRUE(changed.Contains(kW / 2 + 16, kH / 2)) << "where it is";
    EXPECT_FALSE(changed.Contains(2, 2));

    ctx.contentFrame = 0;
    EXPECT_FALSE(renderer.FindChanges(&scene, ctx, changed)) << "a frame to replay is needed";
}

// The profiler's breakdown of a scene: a row for each renderer, naming its
// object, whose drawing adds up to its type's, whose share of finding what
// changed fits inside that stage, and which says why it was drawn again.
// Off, it keeps nothing.
namespace
{
uint64_t s_ProfileTicks = 0;
uint64_t ProfileClock()
{
    return s_ProfileTicks += 3;
}
}  // namespace

TEST(RedrawChanges, TheProfilerBreaksTheFrameDownByObject)
{
    Fixture f(Mode::WholeFrames);
    Square* mover = f.Add("mover", -1.0f, 0.3f, 0xF800, 2);
    f.Add("still", 0.6f, -0.4f, 0x07E0, 1);
    RenderProfiler::Enable(&ProfileClock);
    f.Frame();
    for (int i = 1; i <= 4; ++i)
    {
        mover->GetOwner()->SetX(-1.0f + 0.2f * i);
        f.Frame();
    }
    std::vector<RenderProfiler::ObjectEntry> rows = RenderProfiler::TakeObjects();
    uint32_t frames = 0;
    const std::vector<RenderProfiler::Entry> types = RenderProfiler::Take(frames);
    RenderProfiler::Enable(nullptr);

    const RenderProfiler::ObjectEntry* moverRow = nullptr;
    const RenderProfiler::ObjectEntry* stillRow = nullptr;
    uint64_t objectBlits = 0, objectMeasure = 0;
    for (const auto& r : rows)
    {
        if (r.path == "mover")
        {
            moverRow = &r;
        }
        if (r.path == "still")
        {
            stillRow = &r;
        }
        objectBlits += r.blitUs;
        objectMeasure += r.measureUs;
    }
    ASSERT_NE(moverRow, nullptr);
    ASSERT_NE(stillRow, nullptr);
    EXPECT_EQ(moverRow->order, 2);
    EXPECT_GT(moverRow->draws, stillRow->draws) << "the mover is drawn again each frame, the still one is not";
    EXPECT_EQ(moverRow->redraws[static_cast<int>(RenderProfiler::Redraw::New)], 1u);
    EXPECT_EQ(moverRow->redraws[static_cast<int>(RenderProfiler::Redraw::Changed)], 4u);
    EXPECT_EQ(stillRow->redraws[static_cast<int>(RenderProfiler::Redraw::Changed)], 0u);
    EXPECT_GT(moverRow->measureUs, 0u);

    uint64_t typeBlits = 0, changesUs = 0;
    for (const auto& t : types)
    {
        typeBlits += t.blitUs;
        if (t.typeId == RenderProfiler::kChanges)
        {
            changesUs = t.contentUs;
        }
    }
    EXPECT_EQ(objectBlits, typeBlits) << "every blit is some object's";
    EXPECT_GT(changesUs, 0u);
    EXPECT_LE(objectMeasure, changesUs) << "each object's share of finding what changed is part of that stage";

    // Off: frames leave no rows.
    mover->GetOwner()->SetX(0.5f);
    f.Frame();
    EXPECT_TRUE(RenderProfiler::TakeObjects().empty());
}
