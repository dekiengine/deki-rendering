// The splash screen (Splash.cpp): the scene it makes draws the Deki logo,
// centred on black, at the size the screen calls for.

#include <gtest/gtest.h>

#include "CameraComponent.h"
#include "DekiRenderSystem.h"
#include "QuadBlit.h"
#include "Splash.h"
#include "SplashImage.h"
#include "Standard2DRenderer.h"

#include <deki/BootSplash.h>
#include <deki/Engine.h>
#include <deki/Object.h>
#include <deki/Scene.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

// Tests name the package's types unqualified.
using namespace DekiRendering;

namespace
{

// A screen of `width` x `height` with the splash scene drawn on it, RGB565.
struct SplashFrame
{
    int width = 0;
    int height = 0;
    std::vector<uint16_t> pixels;
    bool made = false;

    SplashFrame(int w, int h)
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
            // Drawn as a game draws a frame: the render system clears to the
            // camera's colour, then the renderer draws.
            made = true;
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
};

// The logo's own pixel at (x, y).
uint16_t Logo(int x, int y)
{
    return SplashImage::kPixels[y * SplashImage::kWidth + x];
}

// Where the lit (not black) pixels are: the smallest box around them.
struct Box
{
    int left = 1 << 30;
    int top = 1 << 30;
    int right = -1;  // past the last lit column
    int bottom = -1;
    int Width() const { return right - left; }
    int Height() const { return bottom - top; }
};

template <typename Pixel>
Box LitBox(int width, int height, Pixel pixel)
{
    Box box;
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            if (pixel(x, y) != 0)
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

Box FrameBox(const SplashFrame& frame)
{
    return LitBox(frame.width, frame.height, [&](int x, int y) { return frame.At(x, y); });
}

Box LogoBox()
{
    return LitBox(SplashImage::kWidth, SplashImage::kHeight, [](int x, int y) { return Logo(x, y); });
}

}  // namespace

TEST(SplashTest, TheLogoIsDrawnOneToOneAndCentredOnBlack)
{
    const SplashFrame frame(320, 240);
    ASSERT_TRUE(frame.made);
    const Box drawn = FrameBox(frame);
    const Box logo = LogoBox();
    ASSERT_EQ(drawn.Width(), logo.Width());
    ASSERT_EQ(drawn.Height(), logo.Height());

    // Centred to within the half pixel an odd size leaves.
    const int left = drawn.left - logo.left;
    const int top = drawn.top - logo.top;
    EXPECT_LE(std::abs(left * 2 + SplashImage::kWidth - 320), 1);
    EXPECT_LE(std::abs(top * 2 + SplashImage::kHeight - 240), 1);

    int differ = 0;
    for (int y = 0; y < SplashImage::kHeight; ++y)
    {
        for (int x = 0; x < SplashImage::kWidth; ++x)
        {
            differ += frame.At(left + x, top + y) != Logo(x, y) ? 1 : 0;
        }
    }
    EXPECT_EQ(differ, 0);
}

TEST(SplashTest, AScreenTwiceAsBigDrawsItTwiceAsBig)
{
    const SplashFrame frame(640, 480);
    ASSERT_TRUE(frame.made);
    const Box drawn = FrameBox(frame);
    const Box logo = LogoBox();
    EXPECT_EQ(drawn.Width(), logo.Width() * 2);
    EXPECT_EQ(drawn.Height(), logo.Height() * 2);
    EXPECT_LE(std::abs(drawn.left + drawn.right - 640), 2);
}

TEST(SplashTest, AScreenSmallerThanTheLogoShowsAllOfIt)
{
    const SplashFrame frame(80, 64);
    ASSERT_TRUE(frame.made);
    const Box drawn = FrameBox(frame);
    ASSERT_GT(drawn.Width(), 0);
    // Black all round the edge: nothing of the logo is cut off.
    EXPECT_GT(drawn.left, 0);
    EXPECT_GT(drawn.top, 0);
    EXPECT_LT(drawn.right, 80);
    EXPECT_LT(drawn.bottom, 64);
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
