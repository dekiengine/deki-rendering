// DekiRenderSystem drawing in strips (bandRows): each strip reaches the
// display as its own rows of the frame, the whole frame arrives, there is no
// framebuffer to read back, and a display that cannot take part of a frame
// is sent nothing.

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <ostream>
#include <vector>

#include <deki/Engine.h>
#include <deki/Object.h>
#include <deki/Scene.h>
#include "CameraComponent.h"
#include "DekiRenderer.h"
#include "DekiRenderSystem.h"
#include <deki/providers/IDisplay.h>

// Tests name the package's types unqualified.
using namespace DekiRendering;

namespace
{

constexpr int kW = 16, kH = 24;

// Writes each pixel as its row in the whole view, so a strip that lands on the
// wrong rows shows.
class RowRenderer : public DekiRenderer
{
public:
    std::vector<int32_t> viewTops;

    uint32_t GetRendererType() const override { return 0x524F5753; }
    bool DrawsInStrips(Deki::Scene*) const override { return stripsOk; }
    bool stripsOk = true;
    void Render(Deki::Scene*, const RenderContext& ctx) override
    {
        viewTops.push_back(ctx.viewTop);
        EXPECT_EQ(ctx.viewHeight > 0 ? ctx.viewHeight : ctx.height, kH);
        auto* px = reinterpret_cast<uint16_t*>(ctx.buffer);
        for (int32_t y = 0; y < ctx.height; ++y)
        {
            for (int32_t x = 0; x < ctx.width; ++x)
            {
                px[y * ctx.width + x] = static_cast<uint16_t>(ctx.viewTop + y);
            }
        }
    }
};

// Copies the rows of every region it is sent into its own frame.
class StripDisplay : public Deki::IDisplay
{
public:
    bool partial = true;
    std::vector<uint16_t> frame = std::vector<uint16_t>(kW * kH, 0xFFFF);
    int regionCalls = 0;
    int fullPresents = 0;

    bool Initialize(int32_t, int32_t) override { return true; }
    void Shutdown() override {}
    void Present(const uint8_t*, int, int, Deki::ColorFormat) override { ++fullPresents; }
    bool SupportsPartialPresent() const override { return partial; }
    void PresentRegions(const uint8_t* framebuffer, int width, int, Deki::ColorFormat, const Deki::Rect* rects,
                        int32_t count) override
    {
        for (int32_t i = 0; i < count; ++i)
        {
            ++regionCalls;
            for (int32_t y = rects[i].top; y < rects[i].bottom; ++y)
            {
                std::memcpy(&frame[static_cast<size_t>(y) * kW], framebuffer + static_cast<size_t>(y) * width * 2,
                            static_cast<size_t>(kW) * 2);
            }
        }
    }
    void GetDisplaySize(int32_t* w, int32_t* h) const override
    {
        if (w)
        {
            *w = kW;
        }
        if (h)
        {
            *h = kH;
        }
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

struct Fixture
{
    StripDisplay display;
    Deki::IDisplay* previous = nullptr;
    Deki::Scene scene;
    RowRenderer renderer;
    DekiRenderSystem system;

    Fixture()
    {
        Deki::Engine& engine = Deki::Engine::GetInstance();
        previous = engine.GetDisplay();
        engine.SetDisplay(&display, "strips");
        auto* camObj = new Deki::Object("Camera");
        camObj->AddComponent<CameraComponent>();
        scene.AddObject(camObj);
        EXPECT_TRUE(system.Setup(kW, kH, Deki::ColorFormat::RGB565));
        system.SetRenderer(&renderer);
    }
    ~Fixture() { Deki::Engine::GetInstance().SetDisplay(previous, previous ? "restored" : ""); }
};

}  // namespace

TEST(BandRender, EveryStripReachesItsOwnRows)
{
    Fixture f;
    ASSERT_TRUE(f.system.SetBandRows(8));
    f.system.Render(&f.scene);

    EXPECT_EQ(f.renderer.viewTops, (std::vector<int32_t>{ 0, 8, 16 }));
    EXPECT_EQ(f.display.regionCalls, 3);
    for (int y = 0; y < kH; ++y)
    {
        EXPECT_EQ(f.display.frame[static_cast<size_t>(y) * kW], y) << "row " << y;
        EXPECT_EQ(f.display.frame[static_cast<size_t>(y) * kW + kW - 1], y) << "row " << y;
    }
}

TEST(BandRender, LastStripIsShorter)
{
    Fixture f;
    ASSERT_TRUE(f.system.SetBandRows(10));
    f.system.Render(&f.scene);
    EXPECT_EQ(f.renderer.viewTops, (std::vector<int32_t>{ 0, 10, 20 }));
    EXPECT_EQ(f.display.frame[static_cast<size_t>(kH - 1) * kW], kH - 1);
}

TEST(BandRender, StripsAreWholePairsOfRows)
{
    Fixture f;
    ASSERT_TRUE(f.system.SetBandRows(5));
    EXPECT_EQ(f.system.GetBandRows(), 6);
}

TEST(BandRender, NothingIsLeftForTheEngineToPresent)
{
    Fixture f;
    ASSERT_TRUE(f.system.SetBandRows(8));
    f.system.Render(&f.scene);
    EXPECT_EQ(f.system.GetFrameBuffer(), nullptr) << "no whole frame exists to read back";
    int32_t count = -2;
    f.system.GetPresentRects(&count);
    EXPECT_EQ(count, 0);
    EXPECT_EQ(f.display.fullPresents, 0);
}

TEST(BandRender, StripsAsTallAsTheScreenAreWholeFrames)
{
    Fixture f;
    ASSERT_TRUE(f.system.SetBandRows(kH));
    EXPECT_EQ(f.system.GetBandRows(), 0);
    EXPECT_NE(f.system.GetFrameBuffer(), nullptr);
}

TEST(BandRender, DisplayWithoutPartialPresentIsSentNothing)
{
    Fixture f;
    f.display.partial = false;
    ASSERT_TRUE(f.system.SetBandRows(8));
    f.system.Render(&f.scene);
    EXPECT_TRUE(f.renderer.viewTops.empty());
    EXPECT_EQ(f.display.regionCalls, 0);
    EXPECT_EQ(f.display.fullPresents, 0);
}

// A renderer that does not say it draws in strips gets whole frames, with
// the log line, rather than strips it would draw wrong.
TEST(BandRender, RendererThatKnowsNothingOfStripsGetsWholeFrames)
{
    Fixture f;
    f.renderer.stripsOk = false;
    ASSERT_TRUE(f.system.SetBandRows(8));
    f.system.Render(&f.scene);
    EXPECT_TRUE(f.system.IsDrawingWholeFrames());
    EXPECT_EQ(f.renderer.viewTops, (std::vector<int32_t>{ 0 })) << "one whole frame";
    EXPECT_EQ(f.display.regionCalls, 0);
    int32_t count = 0;
    f.system.GetPresentRects(&count);
    EXPECT_EQ(count, -1) << "the engine presents it";
    ASSERT_NE(f.system.GetFrameBuffer(), nullptr);
    EXPECT_EQ(reinterpret_cast<const uint16_t*>(f.system.GetFrameBuffer())[(kH - 1) * kW], kH - 1);
}

// frameMode Auto: whole frames when the framebuffer fits in internal RAM with
// the headroom kept for the game, strips otherwise, whole frames for a
// display that cannot take part of a frame; WholeFrames and Strips as set.
TEST(BandRender, AutoChoosesByFreeInternalRam)
{
    using Why = DekiRenderSystem::FrameChoice::Why;
    constexpr size_t kTDeck = 320 * 240 * 2;   // 150 KB
    constexpr size_t kAmoled = 536 * 240 * 2;  // 251 KB
    constexpr size_t kFree = 270 * 1024;       // what an ESP32-S3 board has left at that point

    auto c = DekiRenderSystem::ChooseFrameMode(FrameMode::Auto, 40, kTDeck, kFree, true);
    EXPECT_EQ(c.rows, 0);
    EXPECT_EQ(c.why, Why::Fits);

    c = DekiRenderSystem::ChooseFrameMode(FrameMode::Auto, 40, kAmoled, kFree, true);
    EXPECT_EQ(c.rows, 40);
    EXPECT_EQ(c.why, Why::DoesNotFit);

    // Exactly the headroom to spare still fits; a byte less does not.
    c = DekiRenderSystem::ChooseFrameMode(FrameMode::Auto, 40, kFree - DekiRenderSystem::kInternalHeadroom, kFree,
                                          true);
    EXPECT_EQ(c.rows, 0);
    c = DekiRenderSystem::ChooseFrameMode(FrameMode::Auto, 40, kFree - DekiRenderSystem::kInternalHeadroom + 1, kFree,
                                          true);
    EXPECT_EQ(c.rows, 40);

    c = DekiRenderSystem::ChooseFrameMode(FrameMode::Auto, 40, kAmoled, kFree, false);
    EXPECT_EQ(c.rows, 0);
    EXPECT_EQ(c.why, Why::WholeDisplay);

    c = DekiRenderSystem::ChooseFrameMode(FrameMode::WholeFrames, 40, kAmoled, kFree, true);
    EXPECT_EQ(c.rows, 0);
    EXPECT_EQ(c.why, Why::Chosen);
    c = DekiRenderSystem::ChooseFrameMode(FrameMode::Strips, 24, kTDeck, kFree, true);
    EXPECT_EQ(c.rows, 24);
    EXPECT_EQ(c.why, Why::Chosen);

    // No strip height set: 40.
    c = DekiRenderSystem::ChooseFrameMode(FrameMode::Strips, 0, kTDeck, kFree, true);
    EXPECT_EQ(c.rows, 40);
}

// ---- Strips along a display's scan order (IScanOrderedDisplay) ----

// Rect has no comparison of its own; the tests compare lists of them.
namespace Deki
{
inline bool operator==(const Rect& a, const Rect& b)
{
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
inline std::ostream& operator<<(std::ostream& os, const Rect& r)
{
    return os << "{" << r.left << "," << r.top << "," << r.right << "," << r.bottom << "}";
}
}  // namespace Deki

namespace
{

// Writes each pixel as its place in the whole view, row << 8 | column, so a
// strip that lands anywhere else shows.
class PlaceRenderer : public DekiRenderer
{
public:
    std::vector<Deki::Rect> strips;  // each Render()'s place in the view

    uint32_t GetRendererType() const override { return 0x504C4345; }
    bool DrawsInStrips(Deki::Scene*) const override { return true; }
    void Render(Deki::Scene*, const RenderContext& ctx) override
    {
        EXPECT_EQ(ctx.viewWidth > 0 ? ctx.viewWidth : ctx.width, kW);
        EXPECT_EQ(ctx.viewHeight > 0 ? ctx.viewHeight : ctx.height, kH);
        strips.push_back(Deki::Rect{ ctx.viewLeft, ctx.viewTop, ctx.viewLeft + ctx.width, ctx.viewTop + ctx.height });
        auto* px = reinterpret_cast<uint16_t*>(ctx.buffer);
        for (int32_t y = 0; y < ctx.height; ++y)
        {
            for (int32_t x = 0; x < ctx.width; ++x)
            {
                px[y * ctx.width + x] = static_cast<uint16_t>(((ctx.viewTop + y) << 8) | (ctx.viewLeft + x));
            }
        }
    }
};

// Offers a scan order and copies what PresentStrip sends into its frame.
class ScanDisplay : public IScanOrderedDisplay
{
public:
    ScanOrder order = ScanOrder::ColumnsRight;
    std::vector<uint16_t> frame = std::vector<uint16_t>(kW * kH, 0xFFFF);
    std::vector<Deki::Rect> sent;
    std::vector<bool> firsts;

    ScanOrder GetScanOrder() const override { return order; }
    void PresentStrip(const ScanStrip& s) override
    {
        sent.push_back(s.changed);
        firsts.push_back(s.firstOfFrame);
        const auto* src = reinterpret_cast<const uint16_t*>(s.pixels);
        for (int32_t y = s.changed.top; y < s.changed.bottom; ++y)
        {
            for (int32_t x = s.changed.left; x < s.changed.right; ++x)
            {
                frame[static_cast<size_t>(y) * kW + x] =
                    src[static_cast<size_t>(y - s.strip.top) * s.stride + (x - s.strip.left)];
            }
        }
    }
};

struct ScanFixture
{
    StripDisplay display;
    ScanDisplay scan;
    Deki::IDisplay* previous = nullptr;
    Deki::Scene scene;
    PlaceRenderer renderer;
    DekiRenderSystem system;

    explicit ScanFixture(ScanOrder order)
    {
        scan.order = order;
        Deki::Engine& engine = Deki::Engine::GetInstance();
        previous = engine.GetDisplay();
        engine.SetDisplay(&display, "scan");
        RegisterScanOrderedDisplay(&display, &scan);
        auto* camObj = new Deki::Object("Camera");
        camObj->AddComponent<CameraComponent>();
        scene.AddObject(camObj);
        EXPECT_TRUE(system.Setup(kW, kH, Deki::ColorFormat::RGB565));
        system.SetRenderer(&renderer);
    }
    ~ScanFixture()
    {
        RegisterScanOrderedDisplay(&display, nullptr);
        Deki::Engine::GetInstance().SetDisplay(previous, previous ? "restored" : "");
    }

    void ExpectWholeFrame() const
    {
        for (int y = 0; y < kH; ++y)
        {
            for (int x = 0; x < kW; ++x)
            {
                ASSERT_EQ(scan.frame[static_cast<size_t>(y) * kW + x], (y << 8) | x) << "at " << x << "," << y;
            }
        }
    }
};

}  // namespace

TEST(ScanOrderStrips, ATurnedPanelIsSentColumnsLeftToRight)
{
    ScanFixture f(ScanOrder::ColumnsRight);
    ASSERT_TRUE(f.system.SetBandRows(8));
    EXPECT_TRUE(f.system.IsBandColumns());
    f.system.Render(&f.scene);

    const std::vector<Deki::Rect> expected{ { 0, 0, 8, kH }, { 8, 0, 16, kH } };
    EXPECT_EQ(f.renderer.strips, expected);
    EXPECT_EQ(f.scan.sent, expected);
    EXPECT_EQ(f.scan.firsts, (std::vector<bool>{ true, false })) << "the frame starts at its first strip";
    EXPECT_EQ(f.display.regionCalls, 0) << "strips go to PresentStrip only";
    f.ExpectWholeFrame();

    f.system.Render(&f.scene);
    EXPECT_EQ(f.scan.firsts, (std::vector<bool>{ true, false, true, false })) << "and every frame again";
}

TEST(ScanOrderStrips, ColumnsLeftStartsAtTheRight)
{
    ScanFixture f(ScanOrder::ColumnsLeft);
    ASSERT_TRUE(f.system.SetBandRows(8));
    f.system.Render(&f.scene);
    EXPECT_EQ(f.scan.sent, (std::vector<Deki::Rect>{ { 8, 0, 16, kH }, { 0, 0, 8, kH } }));
    f.ExpectWholeFrame();
}

TEST(ScanOrderStrips, RowsUpStartsAtTheBottom)
{
    ScanFixture f(ScanOrder::RowsUp);
    ASSERT_TRUE(f.system.SetBandRows(10));
    EXPECT_FALSE(f.system.IsBandColumns());
    f.system.Render(&f.scene);
    // 10 rows, made whole pairs: strips of 10 from the bottom, the top one 4.
    EXPECT_EQ(f.scan.sent, (std::vector<Deki::Rect>{ { 0, 20, kW, 24 }, { 0, 10, kW, 20 }, { 0, 0, kW, 10 } }));
    f.ExpectWholeFrame();
}

// A strip of columns is as many pixels as the strip of rows asked for, in
// whole eights: 16 x 24 asked for 5 rows is 80 pixels, 3.3 columns, so 8.
TEST(ScanOrderStrips, ColumnStripsAreAsBigAsTheRowsAskedForInWholeEights)
{
    ScanFixture f(ScanOrder::ColumnsRight);
    ASSERT_TRUE(f.system.SetBandRows(5));
    EXPECT_EQ(f.system.GetBandRows(), 8);
    ASSERT_TRUE(f.system.SetBandRows(11));  // 7.3 columns
    EXPECT_EQ(f.system.GetBandRows(), 8);
}

TEST(ScanOrderStrips, ColumnsAsWideAsTheScreenAreWholeFrames)
{
    ScanFixture f(ScanOrder::ColumnsRight);
    ASSERT_TRUE(f.system.SetBandRows(24));  // 16 columns: the screen's width
    EXPECT_EQ(f.system.GetBandRows(), 0);
}

// A display that offers its scan order after the strips were cut (it starts
// after the render system) has them cut again on the next frame.
TEST(ScanOrderStrips, AnOrderOfferedLaterCutsTheStripsAgain)
{
    Fixture f;
    ASSERT_TRUE(f.system.SetBandRows(8));
    EXPECT_FALSE(f.system.IsBandColumns());

    ScanDisplay scan;
    RegisterScanOrderedDisplay(&f.display, &scan);
    PlaceRenderer place;
    f.system.SetRenderer(&place);
    f.system.Render(&f.scene);
    EXPECT_TRUE(f.system.IsBandColumns());
    EXPECT_EQ(scan.sent.size(), 2u);
    EXPECT_EQ(f.display.regionCalls, 0);

    // Withdrawn: rows again, through PresentRegions.
    RegisterScanOrderedDisplay(&f.display, nullptr);
    f.system.Render(&f.scene);
    EXPECT_FALSE(f.system.IsBandColumns());
    EXPECT_EQ(f.display.regionCalls, 3);
}

// Only the display that offered an order is asked for it.
TEST(ScanOrderStrips, AnotherDisplaysOrderIsNotUsed)
{
    Fixture f;
    StripDisplay other;
    ScanDisplay scan;
    RegisterScanOrderedDisplay(&other, &scan);
    EXPECT_EQ(FindScanOrderedDisplay(&f.display), nullptr);
    ASSERT_TRUE(f.system.SetBandRows(8));
    EXPECT_FALSE(f.system.IsBandColumns());
    RegisterScanOrderedDisplay(&other, nullptr);
}
