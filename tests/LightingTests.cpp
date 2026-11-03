// The 2D light (Lighting2D, from the scene's Light components): what each
// kind of light adds where, in every frame format; that a strip or a part of
// the frame is lit exactly as the whole frame; and where in the draw order
// the renderer lays it (the camera's litUpToSortOrder, renderers that light
// themselves).

#include <gtest/gtest.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

#include <deki/Object.h>
#include <deki/Scene.h>
#include "CameraComponent.h"
#include "Light.h"
#include "Lighting2D.h"
#include "PixelFormat.h"
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

    bool GetContentExtents(float& outW, float& outH) const override
    {
        outW = outH = 8.0f / 16.0f;
        return true;
    }
    bool RenderContent(const Deki::Object*, QuadBlit::Source& outSource, float& outPivotX, float& outPivotY,
                       uint8_t& tr, uint8_t& tg, uint8_t& tb, uint8_t& ta) override
    {
        for (int i = 0; i < 64; i++)
        {
            DekiPixel::Store565(&m_Pixels[i * 2], DekiPixel::PackRGB565(200, 200, 200));
        }
        outSource = QuadBlit::MakeSource(m_Pixels, 8, 8, QuadBlit::PixelLayout::RGB565(), false, nullptr);
        outSource.pixelsPerMeter = 16.0f;
        outPivotX = outPivotY = 0.5f;
        tr = tg = tb = ta = 255;
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
    Standard2DRenderer renderer;
    std::vector<uint8_t> px;

    Frame()
    {
        auto* camObj = new Deki::Object("Camera");
        camera = camObj->AddComponent<CameraComponent>();
        camera->orthoHeight = 3.0f;  // 16 px/m on 64 x 48
        camera->clearColor = Deki::Color(200, 200, 200);
        scene.AddObject(camObj);
        AddLight(scene, LightType::Ambient)->intensity = 0.5f;
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
    f.Add(-1.0f, 0);   // the world
    f.Add(1.0f, 10);   // the UI
    f.Draw();
    EXPECT_NEAR(f.GreenAt(-1.0f), 100, 6);
    EXPECT_NEAR(f.GreenAt(1.0f), 200, 3);
    EXPECT_NEAR(f.GreenAt(0.0f), 100, 6) << "the clear colour is lit too";
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
