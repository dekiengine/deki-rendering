// Strips (bandRows) keep every feature whole frames have. Each test runs the
// same scene, frame after frame, through a render system drawing whole
// frames and one drawing in strips, and the display fed by the strips must
// show exactly the whole frame every time:
// - dirty-rect tracking: only changed rows are sent, a strip with nothing
//   changed is not sent at all;
// - screenshots: a whole frame on request;
// - a camera that does not clear: the last frame is kept;
// - a renderer or pass that cannot draw in strips: whole frames instead.

#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include <deki/Engine.h>
#include <deki/Object.h>
#include <deki/Scene.h>
#include <deki/providers/IDisplay.h>
#include "CameraComponent.h"
#include "DekiRenderSystem.h"
#include "PixelFormat.h"
#include "RendererComponent.h"
#include "RenderPass.h"
#include "ScopedArtDensity.h"
#include "Standard2DRenderer.h"

// Tests name the package's types unqualified.
using namespace DekiRendering;

namespace
{

constexpr int kW = 64, kH = 48;

const Deki::ColorFormat kFormats[] = { Deki::ColorFormat::RGB565, Deki::ColorFormat::RGB888,
                                       Deki::ColorFormat::ARGB8888, Deki::ColorFormat::RGB565A8 };

// A solid 4x4 sprite (0.25 m at 16 px/m).
class StripSprite : public RendererComponent
{
public:
    // Hand-written, not reflected: the class sets its own type info.
    static const Deki::ComponentTypeInfo kTypeInfo;
    StripSprite() { SetTypeInfo(&kTypeInfo); }

    uint16_t colour = 0xF800;
    float sourcePpm = 16.0f;

    bool GetContentExtents(float& outW, float& outH) const override
    {
        outW = outH = 4.0f / sourcePpm;
        return true;
    }

    bool RenderContent(const Deki::Object*, QuadBlit::Source& outSource, float& outPivotX, float& outPivotY,
                       uint8_t& tr, uint8_t& tg, uint8_t& tb, uint8_t& ta) override
    {
        for (int i = 0; i < 16; ++i)
        {
            DekiPixel::Store565(&m_Pixels[i * 2], colour);
        }
        outSource = QuadBlit::MakeSource(m_Pixels, 4, 4, QuadBlit::PixelLayout::RGB565(), false, nullptr);
        outSource.pixelsPerMeter = sourcePpm;
        outPivotX = outPivotY = 0.5f;
        tr = tg = tb = ta = 255;
        return true;
    }

private:
    uint8_t m_Pixels[16 * 2] = {};
};
const Deki::ComponentTypeInfo StripSprite::kTypeInfo =
    Deki::MakeHandWrittenTypeInfo<StripSprite, RendererComponent>(/*declaresUpdate=*/false);

// Keeps the frame it is sent, rectangle by rectangle and pixel by pixel, and
// counts what it was sent.
class FrameDisplay : public Deki::IDisplay
{
public:
    Deki::ColorFormat format = Deki::ColorFormat::RGB565;
    std::vector<uint8_t> frame;
    int regionCalls = 0;
    int64_t pixelsSent = 0;
    int fullPresents = 0;

    void Reset(Deki::ColorFormat f)
    {
        format = f;
        frame.assign(Deki::FrameBufferBytes(f, kW, kH), 0x5A);  // nothing a frame draws
        Count();
    }
    void Count()
    {
        regionCalls = 0;
        pixelsSent = 0;
        fullPresents = 0;
    }

    bool Initialize(int32_t, int32_t) override { return true; }
    void Shutdown() override {}
    void Present(const uint8_t* fb, int, int, Deki::ColorFormat) override
    {
        ++fullPresents;
        ASSERT_NE(fb, nullptr);
        std::memcpy(frame.data(), fb, frame.size());
        pixelsSent += static_cast<int64_t>(kW) * kH;
    }
    bool SupportsPartialPresent() const override { return true; }
    void PresentRegions(const uint8_t* fb, int width, int, Deki::ColorFormat, const Deki::Rect* rects,
                        int32_t count) override
    {
        const size_t bpp = Deki::FrameBufferBytes(format, 1, 1);
        for (int32_t i = 0; i < count; ++i)
        {
            const Deki::Rect& r = rects[i];
            ++regionCalls;
            pixelsSent += static_cast<int64_t>(r.Width()) * r.Height();
            for (int32_t y = r.top; y < r.bottom; ++y)
            {
                const size_t at = (static_cast<size_t>(y) * width + r.left) * bpp;
                std::memcpy(&frame[at], fb + at, static_cast<size_t>(r.Width()) * bpp);
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
    Deki::ColorFormat GetColorFormat() const override { return format; }
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

// A pass that composites the whole frame in EndFrame, knowing nothing of
// strips: it darkens the middle row of whatever buffer it is given, which in
// a strip is the wrong row.
class MiddleRowPass : public RenderPass
{
public:
    bool stripsOk = false;  // claims to draw in strips
    uint32_t HookMask() const override { return RenderPassHooks::BeginFrame | RenderPassHooks::EndFrame; }
    bool DrawsInStrips(Deki::Scene*) const override { return stripsOk; }
    void EndFrame(RenderContext& ctx) override
    {
        const size_t bpp = Deki::FrameBufferBytes(ctx.format, 1, 1);
        const int32_t row = ctx.height / 2;
        std::memset(ctx.buffer + static_cast<size_t>(row) * ctx.width * bpp, 0, static_cast<size_t>(ctx.width) * bpp);
        QuadBlit::MarkDirty(0, row, ctx.width, row + 1);
    }
};

// One scene, two render systems: `whole` draws whole frames, `strips` draws
// strips into `display`.
struct Fixture
{
    ScopedArtDensity art;
    FrameDisplay display;
    Deki::IDisplay* previous = nullptr;
    Deki::Scene scene;
    CameraComponent* camera = nullptr;
    Deki::Object* mover = nullptr;
    Deki::Object* still = nullptr;
    Standard2DRenderer wholeRenderer;
    Standard2DRenderer stripRenderer;
    DekiRenderSystem whole;
    DekiRenderSystem strips;
    Deki::ColorFormat format;

    explicit Fixture(Deki::ColorFormat f = Deki::ColorFormat::RGB565, int rows = 8, bool tracking = false)
        : format(f)
    {
        display.Reset(f);
        Deki::Engine& engine = Deki::Engine::GetInstance();
        previous = engine.GetDisplay();
        engine.SetDisplay(&display, "strip features");

        auto* camObj = new Deki::Object("Camera");
        camera = camObj->AddComponent<CameraComponent>();
        camera->orthoHeight = 3.0f;  // 16 px/m on 64 x 48
        camera->clearColor = Deki::Color(0, 0, 64);
        scene.AddObject(camObj);
        mover = Sprite("mover", -1.5f, 0.6f, 0x07E0);
        still = Sprite("still", 0.8f, -0.5f, 0xF800);

        EXPECT_TRUE(whole.Setup(kW, kH, f));
        whole.SetRenderer(&wholeRenderer);
        whole.SetDirtyTracking(false, 1);
        EXPECT_TRUE(strips.Setup(kW, kH, f));
        strips.SetRenderer(&stripRenderer);
        EXPECT_TRUE(strips.SetBandRows(rows));
        strips.SetDirtyTracking(tracking, 8);
    }
    ~Fixture() { Deki::Engine::GetInstance().SetDisplay(previous, previous ? "restored" : ""); }

    Deki::Object* Sprite(const char* name, float x, float y, uint16_t colour)
    {
        auto* o = new Deki::Object(name);
        o->SetX(x);
        o->SetY(y);
        o->AddComponent<StripSprite>()->colour = colour;
        scene.AddObject(o);
        return o;
    }

    // Draws a frame on both systems and presents the strips' one as
    // Engine::PresentFrame would (in strips it has nothing left to send).
    void Frame()
    {
        display.Count();
        whole.Render(&scene);
        strips.Render(&scene);
        int32_t count = -1;
        const Deki::Rect* rects = strips.GetPresentRects(&count);
        if (count < 0)
        {
            display.Present(strips.GetFrameBuffer(), kW, kH, format);
        }
        else
        {
            display.PresentRegions(strips.GetFrameBuffer(), kW, kH, format, rects, count);
        }
    }

    std::vector<uint8_t> WholeFrame() const
    {
        const uint8_t* fb = whole.GetFrameBuffer();
        return std::vector<uint8_t>(fb, fb + Deki::FrameBufferBytes(format, kW, kH));
    }
};

}  // namespace

// Dirty rectangles in strips: what reaches the display equals the whole
// frame every frame, while only the rows that changed are sent, and a frame
// where nothing moved sends nothing.
TEST(StripFeatures, DirtyTrackingSendsOnlyChangedRowsAndMatchesWholeFrames)
{
    for (Deki::ColorFormat fmt : kFormats)
    {
        for (int rows : { 2, 8, 14 })
        {
            Fixture f(fmt, rows, /*tracking=*/true);
            f.Frame();
            EXPECT_EQ(f.display.pixelsSent, kW * kH) << "the first frame is sent whole";
            EXPECT_EQ(f.display.frame, f.WholeFrame());

            for (int i = 0; i < 12; ++i)
            {
                f.mover->SetX(-1.5f + 0.23f * i);
                f.mover->SetY(0.6f - 0.09f * i);
                f.Frame();
                EXPECT_EQ(f.display.frame, f.WholeFrame()) << "frame " << i << ", strips of " << rows;
                EXPECT_LT(f.display.pixelsSent, kW * kH / 2) << "frame " << i << ", strips of " << rows;
                EXPECT_GT(f.display.pixelsSent, 0);
            }

            // Nothing moved: only the strips the sprites are drawn in are
            // sent (drawn again, as with whole frames), the rest are not.
            f.Frame();
            const int strips = (kH + f.strips.GetBandRows() - 1) / f.strips.GetBandRows();
            EXPECT_LT(f.display.regionCalls, strips) << "strips of " << rows;
            EXPECT_EQ(f.display.frame, f.WholeFrame());

            // Gone: their last rectangles are sent once more, cleared, and
            // then nothing is sent at all.
            f.mover->SetActive(false);
            f.still->SetActive(false);
            f.Frame();
            EXPECT_GT(f.display.pixelsSent, 0);
            EXPECT_EQ(f.display.frame, f.WholeFrame());
            f.Frame();
            EXPECT_EQ(f.display.regionCalls, 0) << "strips of " << rows;
            EXPECT_EQ(f.display.frame, f.WholeFrame());
            f.still->SetActive(true);

            // A new clear colour, and MarkAllDirty, send everything.
            f.camera->clearColor = Deki::Color(40, 0, 0);
            f.Frame();
            EXPECT_EQ(f.display.pixelsSent, kW * kH);
            EXPECT_EQ(f.display.frame, f.WholeFrame());
            f.strips.MarkAllDirty();
            f.Frame();
            EXPECT_EQ(f.display.pixelsSent, kW * kH);
            EXPECT_EQ(f.display.frame, f.WholeFrame());
        }
    }
}

// Tracking on or off, strips draw the same pixels.
TEST(StripFeatures, TrackingOffSendsEverything)
{
    Fixture f(Deki::ColorFormat::RGB565, 8, /*tracking=*/false);
    for (int i = 0; i < 3; ++i)
    {
        f.Frame();
        EXPECT_EQ(f.display.pixelsSent, kW * kH);
        EXPECT_EQ(f.display.frame, f.WholeFrame());
    }
}

// A screenshot asked for while drawing in strips gets the next frame whole,
// equal to the whole frame, and only that frame: the frame after has none
// again.
TEST(StripFeatures, ScreenshotGetsTheNextFrameWhole)
{
    for (Deki::ColorFormat fmt : kFormats)
    {
        for (bool tracking : { false, true })
        {
            Fixture f(fmt, 8, tracking);
            f.Frame();
            EXPECT_EQ(f.strips.GetFrameBuffer(), nullptr);

            DekiRenderSystem::RequestFrameCapture();
            f.mover->SetX(0.3f);
            f.Frame();
            ASSERT_NE(f.strips.GetFrameBuffer(), nullptr);
            const uint8_t* shot = f.strips.GetFrameBuffer();
            EXPECT_EQ(std::vector<uint8_t>(shot, shot + Deki::FrameBufferBytes(fmt, kW, kH)), f.WholeFrame());
            EXPECT_EQ(f.display.frame, f.WholeFrame());

            // The whole frame is a copy only: the strips still went out.
            if (tracking)
            {
                EXPECT_LT(f.display.pixelsSent, kW * kH);
            }

            f.Frame();
            EXPECT_EQ(f.strips.GetFrameBuffer(), nullptr) << "the screenshot frame is freed again";
        }
    }
}

// A camera that does not clear keeps the last frame in strips too: a moving
// sprite leaves the same trail.
TEST(StripFeatures, CameraThatDoesNotClearKeepsTheLastFrame)
{
    for (Deki::ColorFormat fmt : kFormats)
    {
        for (bool tracking : { false, true })
        {
            for (int rows : { 2, 10 })
            {
                Fixture f(fmt, rows, tracking);
                f.camera->clearEveryFrame = false;
                // The whole frame starts from the clear colour, as a kept
                // strip frame does.
                f.whole.ClearBuffer(f.camera->clearColor);

                for (int i = 0; i < 10; ++i)
                {
                    f.mover->SetX(-1.5f + 0.31f * i);
                    f.Frame();
                    EXPECT_EQ(f.display.frame, f.WholeFrame())
                        << "frame " << i << ", strips of " << rows << ", tracking " << tracking;
                }
                // What is kept is also what a screenshot reads.
                ASSERT_NE(f.strips.GetFrameBuffer(), nullptr);
                const uint8_t* kept = f.strips.GetFrameBuffer();
                EXPECT_EQ(std::vector<uint8_t>(kept, kept + Deki::FrameBufferBytes(fmt, kW, kH)), f.WholeFrame());

                // Clearing again: back to cleared strips, nothing kept.
                f.camera->clearEveryFrame = true;
                f.Frame();
                EXPECT_EQ(f.display.frame, f.WholeFrame());
                EXPECT_EQ(f.strips.GetFrameBuffer(), nullptr);
            }
        }
    }
}

// A pass that composites the whole frame and does not say it can draw in
// strips gets whole frames, so it draws what it draws in whole frames; one
// that says it can is drawn in strips.
TEST(StripFeatures, PassThatNeedsWholeFramesGetsThem)
{
    for (bool tracking : { false, true })
    {
        Fixture f(Deki::ColorFormat::RGB565, 8, tracking);
        MiddleRowPass wholePass, stripPass;
        f.wholeRenderer.AddPass(&wholePass);
        f.stripRenderer.AddPass(&stripPass);

        for (int i = 0; i < 4; ++i)
        {
            f.mover->SetX(-1.0f + 0.4f * i);
            f.Frame();
            EXPECT_TRUE(f.strips.IsDrawingWholeFrames());
            EXPECT_NE(f.strips.GetFrameBuffer(), nullptr) << "a whole frame is there to read";
            EXPECT_EQ(f.display.frame, f.WholeFrame()) << "frame " << i;
        }

        // Without the pass, strips again.
        f.wholeRenderer.RemovePass(&wholePass);
        f.stripRenderer.RemovePass(&stripPass);
        f.Frame();
        EXPECT_FALSE(f.strips.IsDrawingWholeFrames());
        EXPECT_EQ(f.strips.GetFrameBuffer(), nullptr);
        EXPECT_EQ(f.display.frame, f.WholeFrame());
        EXPECT_EQ(f.display.fullPresents, 0);
    }
}

TEST(StripFeatures, PassThatCanDrawInStripsKeepsStrips)
{
    Fixture f;
    MiddleRowPass pass;
    pass.stripsOk = true;  // wrongly, but it is believed
    f.stripRenderer.AddPass(&pass);
    f.Frame();
    EXPECT_FALSE(f.strips.IsDrawingWholeFrames());
    EXPECT_EQ(f.strips.GetFrameBuffer(), nullptr);
}

// Per-object passes map through ctx.cam and are drawn in strips by default;
// frame passes are not.
TEST(StripFeatures, DefaultPassAnswers)
{
    struct ObjectPass : RenderPass
    {
        uint32_t HookMask() const override { return RenderPassHooks::Execute | RenderPassHooks::PostExecute; }
    } objectPass;
    struct FramePass : RenderPass
    {
        uint32_t HookMask() const override { return RenderPassHooks::BeginFrame | RenderPassHooks::Execute; }
    } framePass;
    RenderPass everything;
    EXPECT_TRUE(objectPass.DrawsInStrips(nullptr));
    EXPECT_FALSE(framePass.DrawsInStrips(nullptr));
    EXPECT_FALSE(everything.DrawsInStrips(nullptr));
}
