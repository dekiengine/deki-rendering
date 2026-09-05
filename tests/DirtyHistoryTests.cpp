/**
 * @file DirtyHistoryTests.cpp
 * @brief DekiRenderSystem's dirty-rect present: first frame full, steady state
 *        partial, a vanished object still presents its old rectangle, partial
 *        clears leave the framebuffer identical to a full clear, and a display
 *        that hands out alternating buffers gets per-buffer history.
 */

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <vector>

#include "DekiEngine.h"
#include "DekiObject.h"
#include "Scene.h"
#include "CameraComponent.h"
#include "DekiRenderer.h"
#include "DekiRenderSystem.h"
#include "DirtyRegion.h"
#include "providers/IDekiDisplay.h"

namespace
{

constexpr int kW = 64, kH = 48;

// Paints solid white rectangles wherever it is told and reports them.
class ScriptedRenderer : public DekiRenderer
{
public:
    std::vector<Deki::Rect> draws;
    bool unknown = false;   // report nothing (like a renderer that cannot track)
    bool sawTracking = false;

    uint32_t GetRendererType() const override { return 0x54455354; }
    void Render(Deki::Scene*, const RenderContext& ctx) override
    {
        sawTracking = ctx.trackDirty;
        m_Dirty.Reset(ctx.width, ctx.height);
        for (const Deki::Rect& r : draws)
        {
            for (int32_t y = std::max<int32_t>(r.top, 0); y < std::min<int32_t>(r.bottom, ctx.height); ++y)
                for (int32_t x = std::max<int32_t>(r.left, 0); x < std::min<int32_t>(r.right, ctx.width); ++x)
                    reinterpret_cast<uint16_t*>(ctx.buffer)[y * ctx.width + x] = 0xFFFF;
            m_Dirty.Add(r);
        }
    }
    const DirtyRegion* GetLastFrameDirty() const override { return (sawTracking && !unknown) ? &m_Dirty : nullptr; }

private:
    DirtyRegion m_Dirty;
};

// A display with two framebuffers handed out in turn, like a double-buffered LCD.
class TwoBufferDisplay : public Deki::IDisplay
{
public:
    std::vector<uint8_t> bufs[2] = { std::vector<uint8_t>(kW * kH * 2, 0), std::vector<uint8_t>(kW * kH * 2, 0) };
    int index = 0;
    void Flip() { index = 1 - index; }

    bool Initialize(int32_t, int32_t) override { return true; }
    void Shutdown() override {}
    void Present(const uint8_t*, int, int, int) override {}
    void GetDisplaySize(int32_t* w, int32_t* h) const override { if (w) *w = kW; if (h) *h = kH; }
    bool IsInitialized() const override { return true; }
    void RequestFullRefresh() override {}
    bool ProcessEvents() override { return true; }
    void* CreateUIOverlay(int32_t, int32_t) override { return nullptr; }
    bool UpdateUIOverlay(void*, int32_t, int32_t, int32_t, int32_t, const uint32_t*) override { return false; }
    bool UpdateUIOverlayRGB565A8(void*, int32_t, int32_t, int32_t, int32_t, const uint8_t*) override { return false; }
    void DestroyUIOverlay(void*) override {}
    void SetActiveUIOverlay(void*) override {}
    void ClearActiveUIOverlay() override {}
    uint8_t* GetRenderBuffer(int32_t* w, int32_t* h) override
    {
        if (w) *w = kW;
        if (h) *h = kH;
        return bufs[index].data();
    }
};

struct Fixture
{
    Deki::Scene scene;
    CameraComponent* camera = nullptr;
    ScriptedRenderer renderer;   // tracked system under test
    ScriptedRenderer reference;  // same draws, tracking off
    DekiRenderSystem tracked;
    DekiRenderSystem plain;

    Fixture(int alignment = 8)
    {
        auto* camObj = new Deki::Object("Camera");
        camera = camObj->AddComponent<CameraComponent>();
        scene.AddObject(camObj);
        EXPECT_TRUE(tracked.Setup(kW, kH, Deki::ColorFormat::RGB565));
        tracked.SetRenderer(&renderer);
        tracked.SetDirtyTracking(true, alignment);
        EXPECT_TRUE(plain.Setup(kW, kH, Deki::ColorFormat::RGB565));
        plain.SetRenderer(&reference);
        plain.SetDirtyTracking(false, 1);
    }

    // Render one frame on both systems; returns the tracked present count and
    // checks the tracked framebuffer equals a fresh full-clear render.
    int32_t Frame(const std::vector<Deki::Rect>& draws)
    {
        renderer.draws = draws;
        reference.draws = draws;
        tracked.Render(&scene);
        plain.Render(&scene);
        EXPECT_EQ(std::memcmp(tracked.GetFrameBuffer(), plain.GetFrameBuffer(), kW * kH * 2), 0)
            << "partial clear left the framebuffer different from a full clear";
        int32_t count = -2;
        tracked.GetPresentRects(&count);
        return count;
    }

    DirtyRegion PresentRegion()
    {
        int32_t count = 0;
        const Deki::Rect* rects = tracked.GetPresentRects(&count);
        DirtyRegion r;
        r.Reset(kW, kH);
        r.SetFullCoverageRatio(2.0f);
        if (count < 0) r.SetFull();
        for (int32_t i = 0; i < count; ++i) r.Add(rects[i]);
        return r;
    }
};

bool RegionCovers(const DirtyRegion& r, const Deki::Rect& q)
{
    for (int32_t y = q.top; y < q.bottom; ++y)
        for (int32_t x = q.left; x < q.right; ++x)
            if (!r.Contains(x, y)) return false;
    return true;
}

}  // namespace

TEST(DirtyHistory, FirstFrameIsFullThenPartial)
{
    Fixture f;
    EXPECT_EQ(f.Frame({ { 10, 10, 20, 20 } }), -1);
    const int32_t second = f.Frame({ { 12, 10, 22, 20 } });
    EXPECT_GT(second, 0);
    const DirtyRegion pr = f.PresentRegion();
    EXPECT_TRUE(RegionCovers(pr, { 10, 10, 22, 20 }));  // old and new position
    EXPECT_FALSE(pr.Contains(50, 40));
}

TEST(DirtyHistory, VanishedObjectStillPresentsWhereItWas)
{
    Fixture f;
    f.Frame({ { 30, 30, 40, 40 } });
    f.Frame({ { 30, 30, 40, 40 } });
    const int32_t count = f.Frame({});
    EXPECT_GT(count, 0);
    EXPECT_TRUE(RegionCovers(f.PresentRegion(), { 30, 30, 40, 40 }));
    // ... and once the screen is clean there, nothing at all is left to push.
    EXPECT_EQ(f.Frame({}), 0);
}

TEST(DirtyHistory, RectanglesAreAligned)
{
    Fixture f(16);
    f.Frame({ { 5, 5, 9, 9 } });
    f.Frame({ { 5, 5, 9, 9 } });
    int32_t count = 0;
    const Deki::Rect* rects = f.tracked.GetPresentRects(&count);
    ASSERT_EQ(count, 1);
    EXPECT_EQ(rects[0].left, 0);
    EXPECT_EQ(rects[0].top, 0);
    EXPECT_EQ(rects[0].right, 16);
    EXPECT_EQ(rects[0].bottom, 16);
}

TEST(DirtyHistory, ClearColourChangeForcesAFullPresent)
{
    Fixture f;
    f.Frame({ { 1, 1, 4, 4 } });
    f.Frame({ { 1, 1, 4, 4 } });
    f.camera->clearColor = Deki::Color(1, 2, 3);
    EXPECT_EQ(f.Frame({ { 1, 1, 4, 4 } }), -1);
    EXPECT_GT(f.Frame({ { 1, 1, 4, 4 } }), 0);
}

TEST(DirtyHistory, MarkAllDirtyForcesAFullPresentOnce)
{
    Fixture f;
    f.Frame({ { 1, 1, 4, 4 } });
    f.Frame({ { 1, 1, 4, 4 } });
    f.tracked.MarkAllDirty();
    EXPECT_EQ(f.Frame({ { 1, 1, 4, 4 } }), -1);
    EXPECT_GT(f.Frame({ { 1, 1, 4, 4 } }), 0);
}

TEST(DirtyHistory, UnknownFrameFallsBackToFull)
{
    Fixture f;
    f.Frame({ { 1, 1, 4, 4 } });
    f.renderer.unknown = true;
    EXPECT_EQ(f.Frame({ { 1, 1, 4, 4 } }), -1);
    f.renderer.unknown = false;
    EXPECT_EQ(f.Frame({ { 1, 1, 4, 4 } }), -1) << "the screen holds a frame we could not describe";
    EXPECT_GT(f.Frame({ { 1, 1, 4, 4 } }), 0);
}

TEST(DirtyHistory, TrackingOffReportsUnknown)
{
    Fixture f;
    f.tracked.SetDirtyTracking(false, 1);
    EXPECT_EQ(f.Frame({ { 1, 1, 4, 4 } }), -1);
    EXPECT_EQ(f.Frame({ { 1, 1, 4, 4 } }), -1);
    EXPECT_FALSE(f.renderer.sawTracking);
}

TEST(DirtyHistory, LargeCoverageCollapsesToFull)
{
    Fixture f;
    f.Frame({ { 0, 0, kW, kH } });
    EXPECT_EQ(f.Frame({ { 0, 0, kW, kH } }), -1);
}

TEST(DirtyHistory, DoubleBufferedDisplayKeepsPerBufferHistory)
{
    TwoBufferDisplay display;
    Deki::Engine& engine = Deki::Engine::GetInstance();
    Deki::IDisplay* prev = engine.GetDisplay();
    engine.SetDisplay(&display, "two-buffer");

    Deki::Scene scene;
    auto* camObj = new Deki::Object("Camera");
    CameraComponent* camera = camObj->AddComponent<CameraComponent>();
    scene.AddObject(camObj);
    (void)camera;

    ScriptedRenderer renderer, reference;
    DekiRenderSystem tracked, plain;
    ASSERT_TRUE(tracked.Setup(kW, kH, Deki::ColorFormat::RGB565));  // adopts the display's buffer
    tracked.SetRenderer(&renderer);
    tracked.SetDirtyTracking(true, 8);
    ASSERT_TRUE(plain.Setup(kW, kH, Deki::ColorFormat::RGB565));
    plain.SetRenderer(&reference);
    plain.SetDirtyTracking(false, 1);
    // The reference system must not adopt the display's buffer: give it its own.
    engine.SetDisplay(nullptr, "");
    engine.SetDisplay(&display, "two-buffer");

    auto frame = [&](const std::vector<Deki::Rect>& draws) {
        renderer.draws = draws;
        reference.draws = draws;
        tracked.Render(&scene);
        // The reference renders into its own buffer with a full clear each frame.
        plain.Render(&scene);
        EXPECT_EQ(tracked.GetFrameBuffer(), display.bufs[display.index].data()) << "renders into the display's current buffer";
        EXPECT_EQ(std::memcmp(tracked.GetFrameBuffer(), plain.GetFrameBuffer(), kW * kH * 2), 0);
        int32_t count = -2;
        const Deki::Rect* rects = tracked.GetPresentRects(&count);
        DirtyRegion r;
        r.Reset(kW, kH);
        r.SetFullCoverageRatio(2.0f);
        if (count < 0) r.SetFull();
        for (int32_t i = 0; i < count; ++i) r.Add(rects[i]);
        display.Flip();
        return std::make_pair(count, r);
    };

    EXPECT_EQ(frame({ { 8, 8, 16, 16 } }).first, -1);   // buffer A: first use
    EXPECT_EQ(frame({ { 16, 8, 24, 16 } }).first, -1);  // buffer B: first use
    auto third = frame({ { 24, 8, 32, 16 } });          // buffer A again: partial
    EXPECT_GT(third.first, 0);
    EXPECT_TRUE(RegionCovers(third.second, { 16, 8, 32, 16 }));  // previous frame (on B) and this one
    auto fourth = frame({ { 24, 8, 32, 16 } });         // buffer B: partial, object still
    EXPECT_GT(fourth.first, 0);
    EXPECT_TRUE(RegionCovers(fourth.second, { 24, 8, 32, 16 }));
    EXPECT_FALSE(fourth.second.Contains(8, 8)) << "the first frame's rectangle is long gone";

    engine.SetDisplay(prev, prev ? "restored" : "");
}
