// The splash screen (Splash.cpp): the scene it makes drops the Deki logo in
// from above the screen, brings it to rest centred on black, at the size the
// screen calls for, and fades it out.

#include <gtest/gtest.h>

#include "CameraComponent.h"
#include "DekiRenderSystem.h"
#include "QuadBlit.h"
#include "Splash.h"
#include "SplashShapes.h"
#include "Standard2DRenderer.h"

#include <deki/BootSplash.h>
#include <deki/Component.h>
#include <deki/Engine.h>
#include <deki/Object.h>
#include <deki/Scene.h>
#include <deki/Time.h>
#include <deki/providers/IDisplay.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

// Tests name the package's types unqualified.
using namespace DekiRendering;

namespace
{

constexpr float kFrameMs = 1000.0f / 30.0f;

// The screen `seconds` after the logo is first on it, `width` x `height`,
// RGB565: the splash scene played frame by frame as a game plays it.
struct SplashFrame
{
    int width = 0;
    int height = 0;
    std::vector<uint16_t> pixels;
    bool made = false;

    SplashFrame(int w, int h, float seconds)
        : width(w),
          height(h),
          pixels(static_cast<size_t>(w) * h, 0xFFFF)
    {
        DekiRenderSystem screen;
        screen.Setup(w, h, Deki::ColorFormat::RGB565);
        Deki::Engine& engine = Deki::Engine::GetInstance();
        Deki::IRenderSystem* saved = engine.GetRenderSystem();
        engine.SetRenderSystem(&screen);
        RegisterSplash();

        const Deki::BootSplash::MakeScene make = Deki::BootSplash::GetMaker();
        std::unique_ptr<Deki::Scene> scene(make ? make() : nullptr);
        if (scene)
        {
            made = true;
            // The first update is before the first frame; time counts from
            // the second.
            const int frames = 1 + static_cast<int>(std::lround(seconds * 1000.0f / kFrameMs));
            for (int i = 0; i < frames; ++i)
            {
                Deki::Time::UpdateF(kFrameMs);
                for (Deki::Component* component : scene->GetActiveComponents())
                {
                    component->Update();
                }
            }
            // Drawn as a game draws a frame: the render system clears to the
            // camera's colour, then the renderer draws.
            Standard2DRenderer renderer;
            screen.SetRenderer(&renderer);
            screen.Render(scene.get());
            QuadBlit::ClearClipStack();
            screen.SetRenderer(nullptr);
            std::memcpy(pixels.data(), screen.GetFrameBuffer(), pixels.size() * sizeof(uint16_t));
        }

        UnregisterSplash();
        engine.SetRenderSystem(saved);
    }

    uint16_t At(int x, int y) const { return pixels[static_cast<size_t>(y) * width + x]; }

    // How blue the pixel is, 0 to 31.
    int Blue(int x, int y) const { return At(x, y) & 31; }
};

// Where the lit (not black) pixels are, from row `fromY` down: the smallest
// box around them.
struct Box
{
    int left = 1 << 30;
    int top = 1 << 30;
    int right = -1;  // past the last lit column
    int bottom = -1;
    int Width() const { return right - left; }
    int Height() const { return bottom - top; }
    bool Empty() const { return right < 0; }
};

Box LitBox(const SplashFrame& frame, int fromY = 0)
{
    Box box;
    for (int y = fromY; y < frame.height; ++y)
    {
        for (int x = 0; x < frame.width; ++x)
        {
            if (frame.At(x, y) != 0)
            {
                box.left = std::min(box.left, x);
                box.top = std::min(box.top, y);
                box.right = std::max(box.right, x + 1);
                box.bottom = std::max(box.bottom, y + 1);
            }
        }
    }
    return box;
}

// At rest, on a 320 x 240 screen, the icon ends above row 136 and the
// letters start below row 150; at the top of their jump they reach row 142.
constexpr int kBelowIcon = 138;
constexpr int kLettersFrom = 150;

}  // namespace

TEST(SplashTest, ItComesToRestCentredOnBlack)
{
    const SplashFrame frame(320, 240, 1.25f);
    ASSERT_TRUE(frame.made);
    const Box drawn = LitBox(frame);
    ASSERT_FALSE(drawn.Empty());
    // The logo's box, give or take the letters' side and the soft edges.
    EXPECT_NEAR(drawn.Width(), SplashShapes::kWidth, 3);
    EXPECT_NEAR(drawn.Height(), SplashShapes::kHeight, 3);
    EXPECT_LE(std::abs(drawn.left + drawn.right - 320), 3);
    EXPECT_LE(std::abs(drawn.top + drawn.bottom - 240), 3);
}

TEST(SplashTest, AScreenTwiceAsBigDrawsItTwiceAsBig)
{
    const SplashFrame small(320, 240, 1.25f);
    const SplashFrame big(640, 480, 1.25f);
    ASSERT_TRUE(big.made);
    const Box one = LitBox(small);
    const Box two = LitBox(big);
    EXPECT_NEAR(two.Width(), one.Width() * 2, 2);
    EXPECT_NEAR(two.Height(), one.Height() * 2, 2);
    EXPECT_LE(std::abs(two.left + two.right - 640), 4);
}

TEST(SplashTest, ItStartsAboveTheScreen)
{
    const SplashFrame frame(320, 240, 0.0f);
    ASSERT_TRUE(frame.made);
    EXPECT_TRUE(LitBox(frame).Empty());
}

TEST(SplashTest, TheLettersWaitForTheIconToLand)
{
    const SplashFrame falling(320, 240, 0.3f);
    ASSERT_TRUE(falling.made);
    EXPECT_FALSE(LitBox(falling).Empty());               // the icon is on its way down
    EXPECT_TRUE(LitBox(falling, kLettersFrom).Empty());  // no letters yet
    const SplashFrame landed(320, 240, 1.25f);
    EXPECT_FALSE(LitBox(landed, kLettersFrom).Empty());
}

TEST(SplashTest, TheLandingMakesTheLettersJump)
{
    // E and K are at the top of their jump about a quarter of a second after
    // the landing.
    const SplashFrame jumping(320, 240, 0.57f);
    const SplashFrame resting(320, 240, 1.25f);
    const Box up = LitBox(jumping, kBelowIcon);
    const Box down = LitBox(resting, kBelowIcon);
    ASSERT_FALSE(up.Empty());
    EXPECT_LE(up.top, down.top - 8);
}

TEST(SplashTest, ItBlinksOnce)
{
    // A point in the left eye, above its middle: lit while the eye is open,
    // dark while it is shut.
    const SplashFrame open(320, 240, 1.25f);
    const SplashFrame shut(320, 240, 1.125f);
    const Box drawn = LitBox(open);
    const int x = drawn.left + 37;
    const int y = drawn.top + 34;
    EXPECT_GT(open.Blue(x, y), 24);
    EXPECT_LT(shut.Blue(x, y), 8);
}

TEST(SplashTest, ItFadesToBlackBeforeTheGameStarts)
{
    const auto light = [](const SplashFrame& frame)
    {
        long sum = 0;
        for (uint16_t pixel : frame.pixels)
        {
            sum += (pixel >> 11) + ((pixel >> 5) & 63) / 2 + (pixel & 31);
        }
        return sum;
    };
    const SplashFrame bright(320, 240, 1.25f);
    const SplashFrame fading(320, 240, 1.5f);
    const SplashFrame gone(320, 240, 1.68f);
    EXPECT_GT(light(bright), 0);
    EXPECT_LT(light(fading), light(bright) / 2);
    EXPECT_GT(light(fading), 0);
    EXPECT_EQ(light(gone), 0);
}

TEST(SplashTest, AScreenSmallerThanTheLogoShowsAllOfIt)
{
    const SplashFrame frame(80, 64, 1.25f);
    ASSERT_TRUE(frame.made);
    const Box drawn = LitBox(frame);
    ASSERT_GT(drawn.Width(), 0);
    // Black all round the edge: nothing of the logo is cut off.
    EXPECT_GT(drawn.left, 0);
    EXPECT_GT(drawn.top, 0);
    EXPECT_LT(drawn.right, 80);
    EXPECT_LT(drawn.bottom, 64);
}

TEST(SplashTest, ItsShapesTakeUnderAKilobyte)
{
    EXPECT_LT(sizeof(SplashShapes::kLetterBits) + sizeof(SplashShapes::kLetters), 1024u);
}

TEST(SplashTest, RedrawingOnlyChangesShowsEveryFrameAsAWholeOne)
{
    // A board that redraws only what changed must show exactly what a whole
    // frame shows, frame after frame, while the logo falls, squashes, blinks
    // and the letters jump: nothing left behind where something was.
    DekiRenderSystem changes;
    changes.Setup(320, 240, Deki::ColorFormat::RGB565);
    changes.SetRedrawChanges(true);
    DekiRenderSystem whole;
    whole.Setup(320, 240, Deki::ColorFormat::RGB565);
    Deki::Engine& engine = Deki::Engine::GetInstance();
    Deki::IRenderSystem* saved = engine.GetRenderSystem();
    engine.SetRenderSystem(&changes);
    RegisterSplash();
    std::unique_ptr<Deki::Scene> scene(Deki::BootSplash::GetMaker()());
    ASSERT_TRUE(scene);
    Standard2DRenderer changesRenderer;
    Standard2DRenderer wholeRenderer;
    changes.SetRenderer(&changesRenderer);
    whole.SetRenderer(&wholeRenderer);

    const size_t pixels = static_cast<size_t>(320) * 240;
    int firstWrong = -1;
    int worst = 0;
    for (int frame = 0; frame < 50; ++frame)
    {
        Deki::Time::UpdateF(kFrameMs);
        for (Deki::Component* component : scene->GetActiveComponents())
        {
            component->Update();
        }
        changes.Render(scene.get());
        QuadBlit::ClearClipStack();
        whole.Render(scene.get());
        QuadBlit::ClearClipStack();
        const auto* a = reinterpret_cast<const uint16_t*>(changes.GetFrameBuffer());
        const auto* b = reinterpret_cast<const uint16_t*>(whole.GetFrameBuffer());
        int differ = 0;
        for (size_t i = 0; i < pixels; ++i)
        {
            differ += a[i] != b[i] ? 1 : 0;
        }
        if (differ > 0 && firstWrong < 0)
        {
            firstWrong = frame;
        }
        worst = std::max(worst, differ);
    }
    EXPECT_EQ(firstWrong, -1) << "the most pixels wrong in a frame: " << worst;

    changes.SetRenderer(nullptr);
    whole.SetRenderer(nullptr);
    UnregisterSplash();
    engine.SetRenderSystem(saved);
}

namespace
{

// A 320 x 320 screen, as the PicoCalc's, that keeps what it is sent. Like
// its panel (deki-display's PanelDisplay), it sends whole rows: a window on
// the panel spans the rows of what it is told.
class StripScreen : public Deki::IDisplay
{
public:
    static constexpr int kSize = 320;
    std::vector<uint8_t> frame = std::vector<uint8_t>(static_cast<size_t>(kSize) * kSize * 2, 0x5A);

    bool Initialize(int32_t, int32_t) override { return true; }
    void Shutdown() override {}
    uint8_t* GetRenderBuffer(int32_t*, int32_t*) override { return nullptr; }
    void Present(const uint8_t* pixels, int, int, Deki::ColorFormat) override
    {
        std::memcpy(frame.data(), pixels, frame.size());
    }
    bool SupportsPartialPresent() const override { return true; }
    void PresentRegions(const uint8_t* pixels, int width, int, Deki::ColorFormat, const Deki::Rect* rects,
                        int32_t count) override
    {
        for (int32_t i = 0; i < count; ++i)
        {
            const Deki::Rect& r = rects[i];
            for (int32_t y = r.top; y < r.bottom; ++y)
            {
                const size_t at = static_cast<size_t>(y) * width * 2;
                std::memcpy(&frame[at], pixels + at, static_cast<size_t>(width) * 2);
            }
        }
    }
    void GetDisplaySize(int32_t* w, int32_t* h) const override
    {
        *w = kSize;
        *h = kSize;
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

}  // namespace

TEST(SplashTest, InStripsOfRowsEveryFrameReachesTheScreenWhole)
{
    // As a PicoCalc draws it: 320 x 320 in strips of 40 rows, only what
    // changed, sent to the screen strip by strip.
    constexpr int kSize = StripScreen::kSize;
    StripScreen screen;
    Deki::Engine& engine = Deki::Engine::GetInstance();
    Deki::IDisplay* previousDisplay = engine.GetDisplay();
    engine.SetDisplay(&screen, "splash strips");
    DekiRenderSystem strips;
    ASSERT_TRUE(strips.Setup(kSize, kSize, Deki::ColorFormat::RGB565));
    ASSERT_TRUE(strips.SetBandRows(40));
    strips.SetRedrawChanges(true);
    DekiRenderSystem whole;
    whole.Setup(kSize, kSize, Deki::ColorFormat::RGB565);
    Deki::IRenderSystem* saved = engine.GetRenderSystem();
    engine.SetRenderSystem(&strips);
    RegisterSplash();
    std::unique_ptr<Deki::Scene> scene(Deki::BootSplash::GetMaker()());
    ASSERT_TRUE(scene);
    Standard2DRenderer stripsRenderer;
    Standard2DRenderer wholeRenderer;
    strips.SetRenderer(&stripsRenderer);
    whole.SetRenderer(&wholeRenderer);

    int firstWrong = -1;
    int worst = 0;
    for (int frame = 0; frame < 50; ++frame)
    {
        Deki::Time::UpdateF(kFrameMs);
        for (Deki::Component* component : scene->GetActiveComponents())
        {
            component->Update();
        }
        strips.Render(scene.get());
        QuadBlit::ClearClipStack();
        int32_t count = -1;
        const Deki::Rect* rects = strips.GetPresentRects(&count);
        if (count < 0)
        {
            screen.Present(strips.GetFrameBuffer(), kSize, kSize, Deki::ColorFormat::RGB565);
        }
        else if (count > 0)
        {
            screen.PresentRegions(strips.GetFrameBuffer(), kSize, kSize, Deki::ColorFormat::RGB565, rects, count);
        }
        whole.Render(scene.get());
        QuadBlit::ClearClipStack();
        const auto* shown = reinterpret_cast<const uint16_t*>(screen.frame.data());
        const auto* right = reinterpret_cast<const uint16_t*>(whole.GetFrameBuffer());
        int differ = 0;
        for (size_t i = 0; i < static_cast<size_t>(kSize) * kSize; ++i)
        {
            differ += shown[i] != right[i] ? 1 : 0;
        }
        if (differ > 0 && firstWrong < 0)
        {
            firstWrong = frame;
        }
        worst = std::max(worst, differ);
    }
    EXPECT_EQ(firstWrong, -1) << "the most pixels wrong in a frame: " << worst;

    strips.SetRenderer(nullptr);
    whole.SetRenderer(nullptr);
    UnregisterSplash();
    engine.SetRenderSystem(saved);
    engine.SetDisplay(previousDisplay, previousDisplay ? "restored" : "");
}

TEST(SplashTest, NoScreenMeansNoSplash)
{
    Deki::Engine& engine = Deki::Engine::GetInstance();
    Deki::IRenderSystem* saved = engine.GetRenderSystem();
    engine.SetRenderSystem(nullptr);
    RegisterSplash();
    const Deki::BootSplash::MakeScene make = Deki::BootSplash::GetMaker();
    ASSERT_NE(make, nullptr);
    EXPECT_EQ(make(), nullptr);
    UnregisterSplash();
    EXPECT_EQ(Deki::BootSplash::GetMaker(), nullptr);
    engine.SetRenderSystem(saved);
}
