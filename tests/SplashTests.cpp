// The splash screen (Splash.cpp): the scene it makes drops the Deki logo in
// from above the screen and comes to rest centred on black, at the size the
// screen calls for.

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
    const SplashFrame frame(320, 240, 1.4f);
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
    const SplashFrame small(320, 240, 1.4f);
    const SplashFrame big(640, 480, 1.4f);
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
    const SplashFrame landed(320, 240, 1.4f);
    EXPECT_FALSE(LitBox(landed, kLettersFrom).Empty());
}

TEST(SplashTest, TheLandingMakesTheLettersJump)
{
    // E and K are at the top of their jump about a quarter of a second after
    // the landing.
    const SplashFrame jumping(320, 240, 0.57f);
    const SplashFrame resting(320, 240, 1.4f);
    const Box up = LitBox(jumping, kBelowIcon);
    const Box down = LitBox(resting, kBelowIcon);
    ASSERT_FALSE(up.Empty());
    EXPECT_LE(up.top, down.top - 8);
}

TEST(SplashTest, ItBlinksOnce)
{
    // A point in the left eye, above its middle: lit while the eye is open,
    // dark while it is shut.
    const SplashFrame open(320, 240, 1.4f);
    const SplashFrame shut(320, 240, 1.125f);
    const Box drawn = LitBox(open);
    const int x = drawn.left + 37;
    const int y = drawn.top + 34;
    EXPECT_GT(open.Blue(x, y), 24);
    EXPECT_LT(shut.Blue(x, y), 8);
}

TEST(SplashTest, AScreenSmallerThanTheLogoShowsAllOfIt)
{
    const SplashFrame frame(80, 64, 1.4f);
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
