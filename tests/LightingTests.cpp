// The 2D light (Lighting2D, from the scene's Light components): what each
// kind of light adds where, in every frame format; that a strip or a part of
// the frame is lit exactly as the whole frame; and where in the draw order
// the renderer lays it (the camera's litUpToSortOrder, renderers that light
// themselves); and that it lights what is drawn, never the camera's clear
// colour.

#include <gtest/gtest.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

#include <deki/Object.h>
#include <deki/Scene.h>
#include "CameraComponent.h"
#include "DekiRenderSystem.h"
#include "DekiRenderer.h"
#include "Light.h"
#include "Lighting2D.h"
#include "PixelFormat.h"
#include "QuadBlit.h"
#include "RenderPass.h"
#include "RendererComponent.h"
#include "ScopedArtDensity.h"
#include "Standard2DRenderer.h"

using namespace DekiRendering;

namespace
{

constexpr int kW = 64, kH = 48;

// 16 px a metre, the view's centre at world (0, 0).
FrameCamera Camera()
{
    FrameCamera cam;
    cam.ppm = 16.0f;
    cam.halfW = kW / 2.0f;
    cam.halfH = kH / 2.0f;
    cam.valid = true;
    return cam;
}

Light* AddLight(Deki::Scene& scene, LightType type, float x = 0.0f, float y = 0.0f)
{
    auto* o = new Deki::Object("Light");
    o->SetX(x);
    o->SetY(y);
    auto* light = o->AddComponent<Light>();
    light->type = type;
    scene.AddObject(o);
    return light;
}

std::vector<uint8_t> Filled(Deki::ColorFormat format, uint8_t r, uint8_t g, uint8_t b)
{
    const size_t bpp = Deki::FrameBufferBytes(format, 1, 1);
    std::vector<uint8_t> px(static_cast<size_t>(kW) * kH * bpp);
    for (size_t i = 0; i < static_cast<size_t>(kW) * kH; i++)
    {
        switch (format)
        {
            case Deki::ColorFormat::RGB565:
                DekiPixel::WriteDstPixel<Deki::ColorFormat::RGB565>(px.data(), i, r, g, b, 255);
                break;
            case Deki::ColorFormat::RGB565A8:
                DekiPixel::WriteDstPixel<Deki::ColorFormat::RGB565A8>(px.data(), i, r, g, b, 200);
                break;
            case Deki::ColorFormat::RGB888:
                DekiPixel::WriteDstPixel<Deki::ColorFormat::RGB888>(px.data(), i, r, g, b, 255);
                break;
            case Deki::ColorFormat::ARGB8888:
                DekiPixel::WriteDstPixel<Deki::ColorFormat::ARGB8888>(px.data(), i, r, g, b, 255);
                break;
        }
    }
    return px;
}

struct Rgba
{
    uint8_t r, g, b, a;
};

Rgba At(const std::vector<uint8_t>& px, Deki::ColorFormat format, int x, int y)
{
    Rgba c{};
    const size_t i = static_cast<size_t>(y) * kW + x;
    switch (format)
    {
        case Deki::ColorFormat::RGB565:
            DekiPixel::ReadDstPixel<Deki::ColorFormat::RGB565>(px.data(), i, c.r, c.g, c.b, c.a);
            break;
        case Deki::ColorFormat::RGB565A8:
            DekiPixel::ReadDstPixel<Deki::ColorFormat::RGB565A8>(px.data(), i, c.r, c.g, c.b, c.a);
            break;
        case Deki::ColorFormat::RGB888:
            DekiPixel::ReadDstPixel<Deki::ColorFormat::RGB888>(px.data(), i, c.r, c.g, c.b, c.a);
            break;
        case Deki::ColorFormat::ARGB8888:
            DekiPixel::ReadDstPixel<Deki::ColorFormat::ARGB8888>(px.data(), i, c.r, c.g, c.b, c.a);
            break;
    }
    return c;
}

const Deki::ColorFormat kFormats[] = { Deki::ColorFormat::RGB565, Deki::ColorFormat::RGB565A8,
                                       Deki::ColorFormat::RGB888, Deki::ColorFormat::ARGB8888 };

void LightAll(Lighting2D& lighting, std::vector<uint8_t>& px, Deki::ColorFormat format)
{
    lighting.Apply(px.data(), kW, kH, format, Deki::Rect{}, 0, 0);
}

}  // namespace

TEST(Lighting2D, ASceneWithoutLightsIsNotLit)
{
    Deki::Scene scene;
    Lighting2D lighting;
    lighting.Build(&scene, Camera(), kW, kH, 0);
    EXPECT_FALSE(lighting.On());
    std::vector<uint8_t> px = Filled(Deki::ColorFormat::RGB565, 200, 100, 50);
    const std::vector<uint8_t> before = px;
    LightAll(lighting, px, Deki::ColorFormat::RGB565);
    EXPECT_EQ(px, before);
}

TEST(Lighting2D, ALightDimmedToNothingLeavesTheDark)
{
    Deki::Scene scene;
    AddLight(scene, LightType::Point)->intensity = 0.0f;
    Lighting2D lighting;
    lighting.Build(&scene, Camera(), kW, kH, 0);
    ASSERT_TRUE(lighting.On());
    std::vector<uint8_t> px = Filled(Deki::ColorFormat::RGB565, 200, 100, 50);
    LightAll(lighting, px, Deki::ColorFormat::RGB565);
    const Rgba c = At(px, Deki::ColorFormat::RGB565, 32, 24);
    EXPECT_EQ(c.r + c.g + c.b, 0);
}

TEST(Lighting2D, AmbientLightsEverythingTheSameInEveryFormat)
{
    Deki::Scene scene;
    Light* ambient = AddLight(scene, LightType::Ambient);
    ambient->intensity = 0.5f;
    ambient->color = Deki::Color(255, 255, 255);
    Lighting2D lighting;
    lighting.Build(&scene, Camera(), kW, kH, 0);
    for (Deki::ColorFormat format : kFormats)
    {
        std::vector<uint8_t> px = Filled(format, 200, 120, 64);
        const Rgba was = At(px, format, 0, 0);
        LightAll(lighting, px, format);
        for (int y : { 0, 23, 47 })
        {
            for (int x : { 0, 31, 63 })
            {
                const Rgba c = At(px, format, x, y);
                EXPECT_NEAR(c.r, was.r / 2, 9) << static_cast<int>(format);
                EXPECT_NEAR(c.g, was.g / 2, 5) << static_cast<int>(format);
                EXPECT_NEAR(c.b, was.b / 2, 9) << static_cast<int>(format);
                EXPECT_EQ(c.a, was.a) << "the alpha a frame keeps is left alone";
            }
        }
    }
}

TEST(Lighting2D, AColouredAmbientTintsEachChannel)
{
    Deki::Scene scene;
    Light* ambient = AddLight(scene, LightType::Ambient);
    ambient->color = Deki::Color(255, 128, 0);
    Lighting2D lighting;
    lighting.Build(&scene, Camera(), kW, kH, 0);
    std::vector<uint8_t> px = Filled(Deki::ColorFormat::ARGB8888, 200, 200, 200);
    LightAll(lighting, px, Deki::ColorFormat::ARGB8888);
    const Rgba c = At(px, Deki::ColorFormat::ARGB8888, 10, 10);
    EXPECT_NEAR(c.r, 200, 2);
    EXPECT_NEAR(c.g, 100, 4);
    EXPECT_EQ(c.b, 0);
}

TEST(Lighting2D, APointLightsMostAtItsCentreAndNothingPastItsRange)
{
    Deki::Scene scene;
    Light* point = AddLight(scene, LightType::Point);  // at the view's centre
    point->range = 1.0f;                               // 16 px
    Lighting2D lighting;
    lighting.Build(&scene, Camera(), kW, kH, 0);
    std::vector<uint8_t> px = Filled(Deki::ColorFormat::ARGB8888, 160, 160, 160);
    LightAll(lighting, px, Deki::ColorFormat::ARGB8888);
    const int centre = At(px, Deki::ColorFormat::ARGB8888, 32, 24).g;
    const int half = At(px, Deki::ColorFormat::ARGB8888, 32 + 8, 24).g;
    const int edge = At(px, Deki::ColorFormat::ARGB8888, 32 + 14, 24).g;
    EXPECT_GT(centre, 140);
    EXPECT_LT(half, centre);
    EXPECT_LT(edge, half);
    EXPECT_EQ(At(px, Deki::ColorFormat::ARGB8888, 32 + 20, 24).g, 0) << "past the range, no ambient: dark";
    EXPECT_EQ(At(px, Deki::ColorFormat::ARGB8888, 0, 0).g, 0);
}

TEST(Lighting2D, ASpotLightsOnlyAlongItsFacing)
{
    Deki::Scene scene;
    Light* spot = AddLight(scene, LightType::Spot);  // rotation 0: facing +x
    spot->range = 1.5f;
    spot->spotAngle = 60.0f;
    Lighting2D lighting;
    lighting.Build(&scene, Camera(), kW, kH, 0);
    std::vector<uint8_t> px = Filled(Deki::ColorFormat::ARGB8888, 160, 160, 160);
    LightAll(lighting, px, Deki::ColorFormat::ARGB8888);
    EXPECT_GT(At(px, Deki::ColorFormat::ARGB8888, 32 + 10, 24).g, 40) << "ahead";
    EXPECT_EQ(At(px, Deki::ColorFormat::ARGB8888, 32 - 10, 24).g, 0) << "behind";
    EXPECT_EQ(At(px, Deki::ColorFormat::ARGB8888, 32, 24 - 12).g, 0) << "beside";
}

TEST(Lighting2D, ASmoothLightFadesPixelByPixelNotBlockByBlock)
{
    Deki::Scene scene;
    Light* point = AddLight(scene, LightType::Point);
    point->range = 1.5f;  // 24 px
    Lighting2D lighting;
    lighting.Build(&scene, Camera(), kW, kH, 0);
    std::vector<uint8_t> px = Filled(Deki::ColorFormat::ARGB8888, 200, 200, 200);
    LightAll(lighting, px, Deki::ColorFormat::ARGB8888);
    // The four pixels of one block, halfway out: not one light for all.
    std::set<int> inBlock;
    for (int x = 44; x < 48; x++)
    {
        inBlock.insert(At(px, Deki::ColorFormat::ARGB8888, x, 25).g);
    }
    EXPECT_GT(inBlock.size(), 1u);
    // And no jump between two pixels bigger than a few steps of the light.
    for (int x = 33; x < 60; x++)
    {
        const int a = At(px, Deki::ColorFormat::ARGB8888, x - 1, 25).g;
        const int b = At(px, Deki::ColorFormat::ARGB8888, x, 25).g;
        EXPECT_LE(std::abs(a - b), 20) << x;
    }
}

TEST(Lighting2D, BandsLightInHardRings)
{
    Deki::Scene scene;
    Light* point = AddLight(scene, LightType::Point);
    point->range = 1.5f;  // 24 px
    point->bands = 3;
    Lighting2D lighting;
    lighting.Build(&scene, Camera(), kW, kH, 0);
    std::vector<uint8_t> px = Filled(Deki::ColorFormat::ARGB8888, 200, 200, 200);
    LightAll(lighting, px, Deki::ColorFormat::ARGB8888);
    std::set<int> levels;
    for (int x = 32; x < kW; x++)
    {
        levels.insert(At(px, Deki::ColorFormat::ARGB8888, x, 24).g);
    }
    EXPECT_EQ(levels.size(), 4u) << "three rings and the dark";
}

TEST(Lighting2D, LightsAddUpToTwiceAColour)
{
    Deki::Scene scene;
    AddLight(scene, LightType::Ambient)->intensity = 1.5f;
    AddLight(scene, LightType::Ambient)->intensity = 1.5f;
    Lighting2D lighting;
    lighting.Build(&scene, Camera(), kW, kH, 0);
    std::vector<uint8_t> px = Filled(Deki::ColorFormat::ARGB8888, 100, 40, 200);
    LightAll(lighting, px, Deki::ColorFormat::ARGB8888);
    const Rgba c = At(px, Deki::ColorFormat::ARGB8888, 5, 5);
    EXPECT_EQ(c.r, 200);
    EXPECT_EQ(c.g, 80);
    EXPECT_EQ(c.b, 255) << "and no channel past white";
}

TEST(Lighting2D, ADirectionalLightLights2DEvenly)
{
    Deki::Scene scene;
    AddLight(scene, LightType::Directional)->intensity = 0.5f;
    Lighting2D lighting;
    lighting.Build(&scene, Camera(), kW, kH, 0);
    float r, g, b;
    lighting.Ambient(r, g, b);
    EXPECT_FLOAT_EQ(r, 0.5f);
    EXPECT_FLOAT_EQ(g, 0.5f);
    EXPECT_FLOAT_EQ(b, 0.5f);
    EXPECT_TRUE(lighting.Lights().empty());
}

TEST(Lighting2D, AStripIsLitAsTheWholeFrameIs)
{
    Deki::Scene scene;
    AddLight(scene, LightType::Ambient)->intensity = 0.3f;
    Light* point = AddLight(scene, LightType::Point, 0.4f, -0.3f);
    point->range = 1.2f;
    point->color = Deki::Color(255, 180, 90);
    Lighting2D lighting;
    lighting.Build(&scene, Camera(), kW, kH, 0);

    const Deki::ColorFormat f = Deki::ColorFormat::RGB565;
    std::vector<uint8_t> whole = Filled(f, 180, 200, 220);
    std::vector<uint8_t> strips = whole;
    LightAll(lighting, whole, f);
    // Strips of 10 rows: not a whole number of blocks, so a block is cut.
    for (int top = 0; top < kH; top += 10)
    {
        const int rows = std::min(10, kH - top);
        lighting.Apply(strips.data() + static_cast<size_t>(top) * kW * 2, kW, rows, f, Deki::Rect{}, 0, top);
    }
    EXPECT_EQ(strips, whole);
}

TEST(Lighting2D, OnlyThePartAskedForIsLit)
{
    Deki::Scene scene;
    AddLight(scene, LightType::Ambient)->intensity = 0.5f;
    Lighting2D lighting;
    lighting.Build(&scene, Camera(), kW, kH, 0);
    const Deki::ColorFormat f = Deki::ColorFormat::RGB565;
    std::vector<uint8_t> px = Filled(f, 200, 200, 200);
    std::vector<uint8_t> whole = px;
    LightAll(lighting, whole, f);
    const Deki::Rect part{ 9, 5, 30, 17 };
    lighting.Apply(px.data(), kW, kH, f, part, 0, 0);
    for (int y = 0; y < kH; y++)
    {
        for (int x = 0; x < kW; x++)
        {
            const bool inside = x >= part.left && x < part.right && y >= part.top && y < part.bottom;
            const Rgba c = At(px, f, x, y);
            const Rgba lit = At(whole, f, x, y);
            if (inside)
            {
                EXPECT_EQ(c.g, lit.g) << x << "," << y;
            }
            else
            {
                EXPECT_GT(c.g, 190) << x << "," << y;
            }
        }
    }
}

TEST(Lighting2D, AMovedLightChangesWhereItWasAndIs)
{
    Deki::Scene scene;
    Light* point = AddLight(scene, LightType::Point, -1.0f, 0.0f);
    point->range = 0.5f;
    Lighting2D lighting, last;
    DirtyRegion changed;
    changed.Reset(kW, kH);
    lighting.Build(&scene, Camera(), kW, kH, 0);
    lighting.AddChanges(last, changed);
    EXPECT_TRUE(changed.IsFull()) << "lighting came on";

    changed.Reset(kW, kH);
    lighting.Build(&scene, Camera(), kW, kH, 0);
    lighting.AddChanges(last, changed);
    EXPECT_TRUE(changed.IsEmpty()) << "nothing moved";

    point->GetOwner()->SetX(1.0f);
    changed.Reset(kW, kH);
    lighting.Build(&scene, Camera(), kW, kH, 0);
    lighting.AddChanges(last, changed);
    EXPECT_FALSE(changed.IsFull());
    EXPECT_TRUE(changed.Contains(16, 24)) << "where it was";
    EXPECT_TRUE(changed.Contains(48, 24)) << "where it is";
    EXPECT_FALSE(changed.Contains(32, 2));
}

namespace
{
// A square of one colour, `sizePx` wide, that may say it lights itself.
class Patch : public RendererComponent
{
public:
    static const Deki::ComponentTypeInfo kTypeInfo;
    Patch() { SetTypeInfo(&kTypeInfo); }

    bool selfLit = false;
    bool draws = true;    // false: a renderer with nothing to show
    uint8_t alpha = 255;  // how much of what is behind it covers

    bool GetContentExtents(float& outW, float& outH) const override
    {
        outW = outH = 8.0f / 16.0f;
        return true;
    }
    bool RenderContent(const Deki::Object*, QuadBlit::Source& outSource, float& outPivotX, float& outPivotY,
                       uint8_t& tr, uint8_t& tg, uint8_t& tb, uint8_t& ta) override
    {
        if (!draws)
        {
            return false;
        }
        for (int i = 0; i < 64; i++)
        {
            DekiPixel::Store565(&m_Pixels[i * 2], DekiPixel::PackRGB565(200, 200, 200));
        }
        outSource = QuadBlit::MakeSource(m_Pixels, 8, 8, QuadBlit::PixelLayout::RGB565(), false, nullptr);
        outSource.pixelsPerMeter = 16.0f;
        outPivotX = outPivotY = 0.5f;
        tr = tg = tb = 255;
        ta = alpha;
        return true;
    }
    bool LightsItself() const override { return selfLit; }

private:
    uint8_t m_Pixels[64 * 2] = {};
};
const Deki::ComponentTypeInfo Patch::kTypeInfo =
    Deki::MakeHandWrittenTypeInfo<Patch, RendererComponent>(/*declaresUpdate=*/false);

struct Frame
{
    ScopedArtDensity art;
    Deki::Scene scene;
    CameraComponent* camera = nullptr;
    Light* ambient = nullptr;
    Standard2DRenderer renderer;
    std::vector<uint8_t> px;

    Frame()
    {
        auto* camObj = new Deki::Object("Camera");
        camera = camObj->AddComponent<CameraComponent>();
        camera->orthoHeight = 3.0f;  // 16 px/m on 64 x 48
        camera->clearColor = Deki::Color(200, 200, 200);
        scene.AddObject(camObj);
        ambient = AddLight(scene, LightType::Ambient);
        ambient->intensity = 0.5f;
    }
    Patch* Add(float x, int order)
    {
        auto* o = new Deki::Object("Patch");
        o->SetX(x);
        auto* p = o->AddComponent<Patch>();
        p->sortingOrder = order;
        scene.AddObject(o);
        return p;
    }
    void Draw()
    {
        px = Filled(Deki::ColorFormat::RGB565, 200, 200, 200);
        RenderContext ctx{ camera, px.data(), kW, kH, Deki::ColorFormat::RGB565 };
        renderer.Render(&scene, ctx);
    }
    int GreenAt(float worldX) const
    {
        const int x = static_cast<int>(kW / 2 + worldX * 16.0f);
        return At(px, Deki::ColorFormat::RGB565, x, kH / 2).g;
    }
};
}  // namespace

TEST(Lighting2D, WhatDrawsAboveTheCamerasLitSortOrderKeepsItsColours)
{
    Frame f;
    f.camera->litUpToSortOrder = 5;
    f.Add(-1.0f, 0);  // the world
    f.Add(1.0f, 10);  // the UI
    f.Draw();
    EXPECT_NEAR(f.GreenAt(-1.0f), 100, 6);
    EXPECT_NEAR(f.GreenAt(1.0f), 200, 3);
    EXPECT_NEAR(f.GreenAt(0.0f), 200, 3) << "the clear colour is not lit";
}

TEST(Lighting2D, ARendererThatLightsItselfIsNotLitAgain)
{
    Frame f;
    f.Add(-1.0f, 0);
    f.Add(1.0f, 1)->selfLit = true;
    f.Draw();
    EXPECT_NEAR(f.GreenAt(-1.0f), 100, 6);
    EXPECT_NEAR(f.GreenAt(1.0f), 200, 3);
}

namespace
{
// How often a frame's deferred clear ran.
struct ClearCounts
{
    int plain = 0;
    DeferredClear clear;
    ClearCounts()
    {
        clear.owner = this;
        clear.run = [](void* o) { ++static_cast<ClearCounts*>(o)->plain; };
    }
};

void DrawWith(Frame& f, ClearCounts& counts)
{
    f.px = Filled(Deki::ColorFormat::RGB565, 0, 0, 0);
    RenderContext ctx{ f.camera, f.px.data(), kW, kH, Deki::ColorFormat::RGB565 };
    ctx.clear = &counts.clear;
    f.renderer.Render(&f.scene, ctx);
}

// Writes the frame's first pixel itself, as a software rasteriser does,
// saying so as RenderPass::WritesBuffer asks: BeforeWriting, or not at all.
class WritingPass : public RenderPass
{
public:
    bool says = true;
    uint32_t HookMask() const override { return RenderPassHooks::Execute; }
    bool WantsObject(Deki::Object* obj) const override { return obj->GetName() == "Writer"; }
    bool WritesBuffer() const override { return false; }
    void Execute(Deki::Object*, RenderContext& ctx) override
    {
        if (says)
        {
            QuadBlit::BeforeWriting(ctx.buffer);
        }
        DekiPixel::Store565(ctx.buffer, 0x1234);
        QuadBlit::NoteWritten(ctx.buffer);
    }
};
// Writes two pixels of the buffer the frame started with, whatever buffer
// its object is drawn into: a pass that keeps the frame's buffer for the whole
// frame, as a software rasteriser does.
class FrameWritingPass : public RenderPass
{
public:
    static constexpr uint16_t kPixel = 0x1234;
    int firstX = 0, secondX = 0, y = 0;
    uint32_t HookMask() const override { return RenderPassHooks::BeginFrame | RenderPassHooks::Execute; }
    bool WantsObject(Deki::Object* obj) const override { return obj->GetName() == "Writer"; }
    bool WritesBuffer() const override { return false; }
    void BeginFrame(RenderContext& ctx) override { m_Buffer = ctx.buffer; }
    void Execute(Deki::Object* obj, RenderContext&) override
    {
        if (!WantsObject(obj))
        {
            return;  // asked only where a frame is drawn in strips
        }
        QuadBlit::BeforeWriting(m_Buffer);
        DekiPixel::Store565(m_Buffer + (static_cast<size_t>(y) * kW + firstX) * 2, kPixel);
        DekiPixel::Store565(m_Buffer + (static_cast<size_t>(y) * kW + secondX) * 2, kPixel);
        QuadBlit::NoteWritten(m_Buffer);
    }

private:
    uint8_t* m_Buffer = nullptr;
};
// Takes the frame's clear and draws all of the frame in its pixel, as a
// software rasteriser drawing the clear wherever it draws nothing.
class TakingPass : public RenderPass
{
public:
    bool took = false;
    uint32_t HookMask() const override { return RenderPassHooks::Execute; }
    bool WantsObject(Deki::Object* obj) const override { return obj->GetName() == "Writer"; }
    bool WritesBuffer() const override { return false; }
    void Execute(Deki::Object*, RenderContext& ctx) override
    {
        uint8_t pixel[4];
        took = QuadBlit::TakePendingClear(ctx.buffer, pixel);
        if (!took)
        {
            return;
        }
        const size_t bpp = Deki::FrameBufferBytes(ctx.format, 1, 1);
        for (size_t i = 0; i < static_cast<size_t>(ctx.width) * ctx.height; i++)
        {
            std::memcpy(ctx.buffer + i * bpp, pixel, bpp);
        }
        QuadBlit::NoteWritten(ctx.buffer);
    }
};
}  // namespace

// Lights light what is drawn, never the camera's clear colour. A sky of
// #9CCBEA under a sun and an ambient light adding up to 1.65 came out white:
// in a scene of lit meshes the clear colour was all the 2D light fell on.
// With nothing drawn under the light (the one renderer lights itself, as a
// lit mesh does), with something lit beside the bare clear colour, and with
// the camera's litUpToSortOrder under everything, it comes out as it is set.
TEST(Lighting2D, LightsDoNotChangeTheClearColour)
{
    for (int scene = 0; scene < 3; scene++)
    {
        Frame f;
        f.camera->clearColor = Deki::Color(156, 203, 234);
        f.ambient->intensity = 0.55f;
        AddLight(f.scene, LightType::Directional)->intensity = 1.1f;
        f.Add(1.0f, 1)->selfLit = true;
        if (scene == 1)
        {
            f.Add(-1.0f, 0);
        }
        if (scene == 2)
        {
            f.camera->litUpToSortOrder = -1000000;
        }
        DekiRenderSystem system;
        ASSERT_TRUE(system.Setup(kW, kH, Deki::ColorFormat::RGB888));
        system.SetRenderer(&f.renderer);
        system.Render(&f.scene);
        const uint8_t* corner = system.GetFrameBuffer();
        EXPECT_EQ(corner[0], 156) << scene;
        EXPECT_EQ(corner[1], 203) << scene;
        EXPECT_EQ(corner[2], 234) << scene;
        const uint8_t* between = corner + (static_cast<size_t>(kH / 2) * kW + kW / 2) * 3;
        EXPECT_EQ(between[1], 203) << "beside what is drawn, " << scene;
        const uint8_t* patch = corner + (static_cast<size_t>(kH / 2) * kW + kW / 2 - 16) * 3;
        if (scene == 1)
        {
            EXPECT_GE(patch[1], 248) << "the patch is lit, to more than white";
        }
        else
        {
            EXPECT_EQ(patch[1], 203);
        }
    }
}

// The same in every frame format: the frame's own plain clear.
TEST(Lighting2D, AFrameOfOnlyTheClearKeepsItsColourInEveryFormat)
{
    for (Deki::ColorFormat format : kFormats)
    {
        DekiRenderSystem cleared;
        ASSERT_TRUE(cleared.Setup(kW, kH, format));
        cleared.ClearBuffer(200, 200, 200);

        Frame f;
        f.Add(1.0f, 1)->selfLit = true;
        DekiRenderSystem system;
        ASSERT_TRUE(system.Setup(kW, kH, format));
        system.SetRenderer(&f.renderer);
        system.Render(&f.scene);
        // The corner, which holds only the clear: the patch is in the middle.
        const size_t bpp = Deki::FrameBufferBytes(format, 1, 1);
        EXPECT_EQ(std::memcmp(system.GetFrameBuffer(), cleared.GetFrameBuffer(), bpp), 0) << static_cast<int>(format);
    }
}

// And with something lit drawn on it, in every frame format: the patch is
// lit, the clear colour round it is the frame's own plain clear.
TEST(Lighting2D, TheClearColourRoundWhatIsLitKeepsItsColourInEveryFormat)
{
    for (Deki::ColorFormat format : kFormats)
    {
        DekiRenderSystem cleared;
        ASSERT_TRUE(cleared.Setup(kW, kH, format));
        cleared.ClearBuffer(200, 200, 200);

        Frame f;
        f.Add(0.0f, 0);
        DekiRenderSystem system;
        ASSERT_TRUE(system.Setup(kW, kH, format));
        system.SetRenderer(&f.renderer);
        system.Render(&f.scene);
        const size_t bpp = Deki::FrameBufferBytes(format, 1, 1);
        const size_t bytes = static_cast<size_t>(kW) * kH * bpp;
        const std::vector<uint8_t> px(system.GetFrameBuffer(), system.GetFrameBuffer() + bytes);
        EXPECT_NEAR(At(px, format, kW / 2, kH / 2).g, 100, 6) << static_cast<int>(format);
        // The patch is 8 pixels square in the middle: every pixel outside it.
        for (int y = 0; y < kH; y++)
        {
            for (int x = 0; x < kW; x++)
            {
                if (x >= kW / 2 - 4 && x < kW / 2 + 4 && y >= kH / 2 - 4 && y < kH / 2 + 4)
                {
                    continue;
                }
                const size_t at = (static_cast<size_t>(y) * kW + x) * bpp;
                ASSERT_EQ(std::memcmp(px.data() + at, cleared.GetFrameBuffer() + at, bpp), 0)
                    << static_cast<int>(format) << " at " << x << "," << y;
            }
        }
    }
}

// What is lit and lets the clear colour through (a translucent sprite, a
// soft edge) is lit as much as it covers: the clear colour's share of the
// pixel stays as it is.
TEST(Lighting2D, TheClearColourShowsUnlitThroughWhatIsLit)
{
    Frame f;
    f.Add(0.0f, 0)->alpha = 128;
    f.Draw();
    // Half the patch lit (200 x 0.5), half the clear colour (200) as it is.
    EXPECT_NEAR(f.GreenAt(0.0f), 150, 8);
    EXPECT_NEAR(f.GreenAt(1.0f), 200, 3);
}

// A lamp lights what is drawn within its reach, and none of the clear colour
// there.
TEST(Lighting2D, ALampDoesNotLightTheClearColour)
{
    Frame f;
    f.ambient->intensity = 0.25f;
    Light* lamp = AddLight(f.scene, LightType::Point, 0.0f, 0.0f);
    lamp->range = 3.0f;
    f.Add(0.0f, 0);
    f.Draw();
    EXPECT_GT(f.GreenAt(0.0f), 200) << "the patch under the lamp is lit by it";
    EXPECT_NEAR(f.GreenAt(1.0f), 200, 3) << "the clear colour beside it is not";
}

// The first draw under the light covering the whole view: none of the clear
// colour can show, and the frame is lit whole.
TEST(Lighting2D, AFrameCoveredByItsFirstDrawIsLitWhole)
{
    Frame f;
    f.Add(0.0f, 0)->GetOwner()->SetScale(8.0f, 8.0f);  // 64 pixels square
    f.Add(1.0f, 1);
    DekiRenderSystem system;
    ASSERT_TRUE(system.Setup(kW, kH, Deki::ColorFormat::RGB565));
    system.SetRenderer(&f.renderer);
    system.Render(&f.scene);
    const std::vector<uint8_t> px(system.GetFrameBuffer(), system.GetFrameBuffer() + static_cast<size_t>(kW) * kH * 2);
    EXPECT_NEAR(At(px, Deki::ColorFormat::RGB565, 0, 0).g, 100, 6);
    EXPECT_NEAR(At(px, Deki::ColorFormat::RGB565, kW / 2 + 16, kH / 2).g, 100, 6);
}

// A camera that does not clear leaves the frame under what it draws: it is
// lit whole, as before.
TEST(Lighting2D, ACameraThatDoesNotClearLightsTheFrame)
{
    Frame f;
    f.camera->clearEveryFrame = false;
    f.Add(-1.0f, 0);
    f.Draw();
    EXPECT_NEAR(f.GreenAt(-1.0f), 100, 6);
    EXPECT_NEAR(f.GreenAt(0.0f), 100, 6);
}

// Drawn in strips, each with what is lit in a layer of its own, the frame is
// the one drawn whole: a lamp over two patches, one half transparent, on the
// bare clear colour.
TEST(Lighting2D, AFrameInStripsIsLitAsTheWholeFrame)
{
    auto build = [](Frame& f)
    {
        f.ambient->intensity = 0.3f;
        AddLight(f.scene, LightType::Point, 0.3f, 0.2f)->range = 2.0f;
        f.Add(-1.0f, 0);
        f.Add(0.5f, 1)->alpha = 128;
    };
    Frame whole;
    build(whole);
    whole.Draw();

    Frame f;
    build(f);
    constexpr int kStrip = 10;  // not a multiple of the light's blocks
    std::vector<uint8_t> frame;
    for (int top = 0; top < kH; top += kStrip)
    {
        const int rows = std::min(kStrip, kH - top);
        std::vector<uint8_t> strip = Filled(Deki::ColorFormat::RGB565, 200, 200, 200);
        strip.resize(static_cast<size_t>(kW) * rows * 2);
        RenderContext ctx{ f.camera, strip.data(), kW, rows, Deki::ColorFormat::RGB565 };
        ctx.viewTop = top;
        ctx.viewHeight = kH;
        ctx.viewWidth = kW;
        ctx.contentFrame = 7;
        f.renderer.Render(&f.scene, ctx);
        frame.insert(frame.end(), strip.begin(), strip.end());
    }
    EXPECT_EQ(frame, whole.px);
}

// The frame's clear always runs in its own colour, once, wherever the light
// comes.
TEST(Lighting2D, TheClearRunsPlainWhateverIsLit)
{
    {
        Frame f;  // ambient light only, and the one renderer lights itself
        f.Add(1.0f, 1)->selfLit = true;
        ClearCounts counts;
        DrawWith(f, counts);
        EXPECT_EQ(counts.plain, 1);
    }
    {
        Frame f;  // a lamp in view: the light differs from place to place
        AddLight(f.scene, LightType::Point, 0.5f, 0.0f)->range = 1.0f;
        f.Add(1.0f, 1)->selfLit = true;
        ClearCounts counts;
        DrawWith(f, counts);
        EXPECT_EQ(counts.plain, 1);
    }
    {
        Frame f;  // something lit is drawn before the light is due
        f.Add(-1.0f, 0);
        ClearCounts counts;
        DrawWith(f, counts);
        EXPECT_EQ(counts.plain, 1);
    }
    {
        Frame f;  // nothing is drawn at all
        ClearCounts counts;
        DrawWith(f, counts);
        EXPECT_EQ(counts.plain, 1);
    }
}

// A pass that writes into the frame itself between two lit renderers: what
// was drawn under the light before it lies under what it writes, what is
// drawn after lies over, as without lights.
TEST(Lighting2D, WhatWritesTheFrameItselfKeepsItsPlaceAmongWhatIsLit)
{
    Frame f;
    f.Add(-1.0f, 0);
    auto* writer = new Deki::Object("Writer");
    Patch* nothing = writer->AddComponent<Patch>();
    nothing->draws = false;
    nothing->sortingOrder = 1;
    f.scene.AddObject(writer);
    f.Add(1.0f, 2);
    FrameWritingPass pass;
    pass.firstX = kW / 2 - 16;   // in the first patch
    pass.secondX = kW / 2 + 16;  // in the last
    pass.y = kH / 2;
    f.renderer.AddPass(&pass);
    f.Draw();
    const size_t row = static_cast<size_t>(kH / 2) * kW;
    EXPECT_EQ(DekiPixel::Load565(f.px.data() + (row + pass.firstX) * 2), FrameWritingPass::kPixel)
        << "written over the first patch";
    EXPECT_NEAR(f.GreenAt(1.0f), 100, 6) << "the last patch, lit, over what was written";
    EXPECT_NEAR(At(f.px, Deki::ColorFormat::RGB565, pass.firstX + 2, pass.y).g, 100, 6) << "the first patch, lit";
    EXPECT_NEAR(f.GreenAt(0.0f), 200, 3) << "the clear colour between them";
}

// A pass that writes into the frame itself and says it does not
// (WritesBuffer) leaves the clear waiting until it calls BeforeWriting.
TEST(Lighting2D, APassThatWritesItselfIsClearedUnderWhenItSaysSo)
{
    for (bool says : { true, false })
    {
        // No lights, and nothing else that draws: only the pass can make the
        // clear before the frame's end.
        ScopedArtDensity art;
        Deki::Scene scene;
        auto* camObj = new Deki::Object("Camera");
        CameraComponent* camera = camObj->AddComponent<CameraComponent>();
        camera->orthoHeight = 3.0f;
        camera->clearColor = Deki::Color(200, 200, 200);
        scene.AddObject(camObj);
        auto* writer = new Deki::Object("Writer");
        writer->AddComponent<Patch>()->draws = false;
        scene.AddObject(writer);
        Standard2DRenderer renderer;
        WritingPass pass;
        pass.says = says;
        renderer.AddPass(&pass);
        DekiRenderSystem system;
        ASSERT_TRUE(system.Setup(kW, kH, Deki::ColorFormat::RGB565));
        system.SetRenderer(&renderer);
        system.Render(&scene);
        const uint16_t first = DekiPixel::Load565(system.GetFrameBuffer());
        if (says)
        {
            EXPECT_EQ(first, 0x1234) << "the clear came before the pass wrote";
        }
        else
        {
            EXPECT_NE(first, 0x1234) << "with nothing said, the clear came after";
        }
    }
}

// A pass that writes every pixel itself can take the frame's clear as one
// pixel, in the clear colour as it is, whatever the lights: the frame is what
// the clear would have made.
TEST(Lighting2D, APassTakesTheClearInItsOwnColour)
{
    for (Deki::ColorFormat format : kFormats)
    {
        Frame plain;  // the clear, made by the render system
        plain.Add(1.0f, 1)->selfLit = true;
        DekiRenderSystem made;
        ASSERT_TRUE(made.Setup(kW, kH, format));
        made.SetRenderer(&plain.renderer);
        made.Render(&plain.scene);

        Frame f;
        auto* writer = new Deki::Object("Writer");
        Patch* nothing = writer->AddComponent<Patch>();
        nothing->draws = false;
        nothing->selfLit = true;  // the light is due before it, with nothing drawn
        nothing->sortingOrder = 1;
        f.scene.AddObject(writer);
        TakingPass pass;
        f.renderer.AddPass(&pass);
        DekiRenderSystem system;
        ASSERT_TRUE(system.Setup(kW, kH, format));
        system.SetRenderer(&f.renderer);
        system.Render(&f.scene);
        EXPECT_TRUE(pass.took);
        // The corners, which hold only the clear in both: `plain`'s patch is
        // in the middle.
        const size_t bpp = Deki::FrameBufferBytes(format, 1, 1);
        EXPECT_EQ(std::memcmp(system.GetFrameBuffer(), made.GetFrameBuffer(), bpp), 0) << static_cast<int>(format);
        EXPECT_EQ(std::memcmp(system.GetFrameBuffer() + (static_cast<size_t>(kW) * kH - 1) * bpp,
                              made.GetFrameBuffer() + (static_cast<size_t>(kW) * kH - 1) * bpp, bpp),
                  0);
        DekiRenderSystem cleared;
        ASSERT_TRUE(cleared.Setup(kW, kH, format));
        cleared.ClearBuffer(200, 200, 200);
        EXPECT_EQ(std::memcmp(system.GetFrameBuffer(), cleared.GetFrameBuffer(), bpp), 0) << "the clear is not lit";
    }
}
