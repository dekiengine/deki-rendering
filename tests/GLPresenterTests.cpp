// Tests that the presenter composes 2D layers the way the 2D blitter leaves
// them, premultiplied in a cleared RGB565A8 buffer, in the order they are
// pushed, and that a frame starts over once presented. On a machine with no
// OpenGL the tests skip.

// Only where the platform has OpenGL (DEKI_CAPABILITY_OPENGL); a build
// without it, as for a board, has none of the code under test.
#ifdef DEKI_CAPABILITY_OPENGL

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "deki-rendering/GLApi.h"
#include "deki-rendering/GLPresenter.h"
#include "deki-rendering/PixelFormat.h"
#include "deki-rendering/ShaderAsset.h"

using namespace DekiGL;

namespace
{

constexpr int kW = 32;
constexpr int kH = 24;

std::unique_ptr<GLPresenter> MakePresenter()
{
    std::shared_ptr<GLContext> context = SharedContext();
    if (!context)
    {
        return nullptr;
    }
    GLPresenter::Window window{ [](void*) {},
                                [](void*, int* w, int* h)
                                {
                                    *w = kW;
                                    *h = kH;
                                },
                                nullptr };
    return GLPresenter::Create(context, window);
}

#define REQUIRE_PRESENTER(p)                     \
    std::unique_ptr<GLPresenter> p = MakePresenter(); \
    if (!p)                                      \
    {                                            \
        GTEST_SKIP() << "no OpenGL on this machine"; \
    }

/// An RGB565A8 buffer, every pixel (r, g, b) at coverage a, premultiplied as
/// the caller gives it.
std::vector<uint8_t> Filled(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    std::vector<uint8_t> buffer(Deki::FrameBufferBytes(Deki::ColorFormat::RGB565A8, kW, kH));
    for (size_t i = 0; i < static_cast<size_t>(kW) * kH; ++i)
    {
        DekiPixel::WriteDstPixel<Deki::ColorFormat::RGB565A8>(buffer.data(), i, r, g, b, a);
    }
    return buffer;
}

void Set(std::vector<uint8_t>& buffer, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    DekiPixel::WriteDstPixel<Deki::ColorFormat::RGB565A8>(buffer.data(), static_cast<size_t>(y) * kW + x, r, g, b, a);
}

void ExpectPixel(const std::vector<uint8_t>& rgba, int x, int y, int r, int g, int b)
{
    const uint8_t* p = &rgba[(static_cast<size_t>(y) * kW + x) * 4];
    EXPECT_NEAR(p[0], r, 4) << "red at " << x << "," << y;
    EXPECT_NEAR(p[1], g, 4) << "green at " << x << "," << y;
    EXPECT_NEAR(p[2], b, 4) << "blue at " << x << "," << y;
}

}  // namespace

TEST(GLPresenter, LayersComposeInOrderPremultiplied)
{
    REQUIRE_PRESENTER(presenter);
    // Opaque red below; above it, transparent but for a half-covering green
    // (premultiplied: 128 of green at coverage 128) and an opaque blue.
    std::vector<uint8_t> below = Filled(200, 0, 0, 255);
    std::vector<uint8_t> above = Filled(0, 0, 0, 0);
    Set(above, 5, 5, 0, 128, 0, 128);
    Set(above, 20, 10, 0, 0, 248, 255);

    presenter->PushLayer(below.data(), kW, kH, Deki::ColorFormat::RGB565A8);
    presenter->PushLayer(above.data(), kW, kH, Deki::ColorFormat::RGB565A8);
    std::vector<uint8_t> frame;
    ASSERT_TRUE(presenter->ReadFrame(frame));
    ExpectPixel(frame, 0, 0, 200, 0, 0);    // only the layer below
    ExpectPixel(frame, 5, 5, 100, 128, 0);  // half of each
    ExpectPixel(frame, 20, 10, 0, 0, 248);  // the layer above covers
    ExpectPixel(frame, kW - 1, kH - 1, 200, 0, 0);
}

TEST(GLPresenter, ATransparentLayerChangesNothing)
{
    REQUIRE_PRESENTER(presenter);
    std::vector<uint8_t> below = Filled(40, 80, 160, 255);
    std::vector<uint8_t> nothing = Filled(0, 0, 0, 0);
    presenter->PushLayer(below.data(), kW, kH, Deki::ColorFormat::RGB565A8);
    std::vector<uint8_t> before, after;
    ASSERT_TRUE(presenter->ReadFrame(before));
    presenter->PushLayer(nothing.data(), kW, kH, Deki::ColorFormat::RGB565A8);
    ASSERT_TRUE(presenter->ReadFrame(after));
    EXPECT_EQ(before, after);
}

TEST(GLPresenter, AFormatWithoutAlphaIsOpaque)
{
    REQUIRE_PRESENTER(presenter);
    std::vector<uint8_t> rgb(Deki::FrameBufferBytes(Deki::ColorFormat::RGB565, kW, kH));
    for (size_t i = 0; i < static_cast<size_t>(kW) * kH; ++i)
    {
        DekiPixel::WriteDstPixel<Deki::ColorFormat::RGB565>(rgb.data(), i, 0, 252, 0, 255);
    }
    EXPECT_FALSE(presenter->TakesLayers(kW, kH, Deki::ColorFormat::RGB565));
    presenter->PushLayer(rgb.data(), kW, kH, Deki::ColorFormat::RGB565);
    std::vector<uint8_t> frame;
    ASSERT_TRUE(presenter->ReadFrame(frame));
    ExpectPixel(frame, 3, 3, 0, 252, 0);
}

TEST(GLPresenter, PresentingEndsTheFrame)
{
    REQUIRE_PRESENTER(presenter);
    EXPECT_EQ(GLPresenter::Active(), presenter.get());
    std::vector<uint8_t> below = Filled(10, 20, 30, 255);
    presenter->Present(below.data(), kW, kH, Deki::ColorFormat::RGB565A8);
    std::vector<uint8_t> frame;
    EXPECT_FALSE(presenter->ReadFrame(frame)) << "a presented frame is over";

    // Gone, it leaves no active presenter behind.
    presenter.reset();
    EXPECT_EQ(GLPresenter::Active(), nullptr);
}

// A screen effect draws the frame into the window through its Effect(): here
// one that inverts, presented into a texture to read back. The corner checks
// that uv runs from the bottom left, the way the effect is told.
TEST(GLPresenter, AScreenEffectShowsTheFrameThroughItsShader)
{
    std::shared_ptr<GLContext> context = SharedContext();
    if (!context || !context->MakeCurrent())
    {
        GTEST_SKIP() << "no OpenGL on this machine";
    }
    const GL& gl = context->Functions();
    GLuint target = 0, texture = 0;
    gl.GenTextures(1, &texture);
    gl.BindTexture(gl::TEXTURE_2D, texture);
    gl.TexImage2D(gl::TEXTURE_2D, 0, static_cast<GLint>(gl::RGBA8), kW, kH, 0, gl::RGBA, gl::UNSIGNED_BYTE, nullptr);
    gl.GenFramebuffers(1, &target);
    gl.BindFramebuffer(gl::FRAMEBUFFER, target);
    gl.FramebufferTexture2D(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT0, gl::TEXTURE_2D, texture, 0);
    ASSERT_EQ(gl.CheckFramebufferStatus(gl::FRAMEBUFFER), gl::FRAMEBUFFER_COMPLETE);

    GLPresenter::Window window{ [](void*) {},
                                [](void*, int* w, int* h)
                                {
                                    *w = kW;
                                    *h = kH;
                                },
                                nullptr, target };
    std::unique_ptr<GLPresenter> presenter = GLPresenter::Create(context, window);
    ASSERT_NE(presenter, nullptr);

    const char* text = "// @fragment\nvec4 Effect(vec2 uv)\n{\n    return vec4(1.0) - Frame(uv);\n}\n";
    DekiRendering::ShaderAsset invert;
    ASSERT_TRUE(invert.Parse(text, std::strlen(text)));
    GLPresenter::Effect effect;
    effect.shader = &invert;

    // Blue, with the top-left pixel red.
    std::vector<uint8_t> frame = Filled(0, 0, 248, 255);
    Set(frame, 0, 0, 248, 0, 0, 255);
    presenter->Present(frame.data(), kW, kH, Deki::ColorFormat::RGB565A8, effect);

    std::vector<uint8_t> shown(static_cast<size_t>(kW) * kH * 4);
    gl.BindFramebuffer(gl::FRAMEBUFFER, target);
    gl.ReadPixels(0, 0, kW, kH, gl::RGBA, gl::UNSIGNED_BYTE, shown.data());
    auto at = [&](int x, int y) { return &shown[(static_cast<size_t>(kH - 1 - y) * kW + x) * 4]; };  // top-down
    EXPECT_NEAR(at(5, 5)[0], 255, 4);  // blue inverted: yellow
    EXPECT_NEAR(at(5, 5)[1], 255, 4);
    EXPECT_NEAR(at(5, 5)[2], 7, 4);
    EXPECT_NEAR(at(0, 0)[0], 7, 4);  // red inverted: cyan, at the top left
    EXPECT_NEAR(at(0, 0)[1], 255, 4);
    EXPECT_NEAR(at(0, 0)[2], 255, 4);

    presenter.reset();
    gl.DeleteFramebuffers(1, &target);
    gl.DeleteTextures(1, &texture);
}

#endif  // DEKI_CAPABILITY_OPENGL
