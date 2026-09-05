/**
 * @file Standard2DRendererGoldenTests.cpp
 * @brief Golden-image gate for the scene renderer (Standard2DRenderer): sort
 *        order and stability, transparent containers, inactive subtrees,
 *        nested clips, ignoreClip, renderer and camera pixel snapping, camera
 *        pixels-per-meter, parent rotation/scale, off-screen culling, alpha
 *        blending - on every target format.
 *
 * GoldenBlitTests pins the rasterizer; this pins everything the renderer does
 * around it. The constants were captured before the per-object overhead work
 * (FrameCamera, SortItem caching, hook masks) and must not change through it:
 * a differing hash there is a bug, not a re-pin. DEKI_GOLDEN_PRINT=1 lists
 * every case's hash so two builds can be compared case by case.
 */

#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

#include "DekiEngine.h"
#include "DekiObject.h"
#include "Scene.h"
#include "ComponentInterfaceAdapters.h"
#include "IClipProvider.h"
#include "ISortableProvider.h"
#include "CameraComponent.h"
#include "RendererComponent.h"
#include "Standard2DRenderer.h"
#include "DirtyRegion.h"
#include "QuadBlit.h"

namespace
{

// ---------------------------------------------------------------- test components

// Solid-colour 4x4 sprite, optionally with per-pixel alpha (RGB565A8).
class TestRenderer : public RendererComponent
{
public:
    DECLARE_COMPONENT_TYPE(TestRenderer, RendererComponent)

    uint16_t colour = 0xF800;  // RGB565
    uint8_t alpha = 255;       // used when withAlpha
    bool withAlpha = false;
    float pivotX = 0.5f, pivotY = 0.5f;
    float sourcePpm = 16.0f;   // 4 px == 0.25 m at the default camera scale

    bool GetContentExtents(float& outW, float& outH) const override
    {
        outW = 4.0f / sourcePpm;
        outH = 4.0f / sourcePpm;
        return true;
    }

    bool RenderContent(const Deki::Object*, QuadBlit::Source& outSource, float& outPivotX, float& outPivotY,
                       uint8_t& tr, uint8_t& tg, uint8_t& tb, uint8_t& ta) override
    {
        for (int i = 0; i < 16; ++i)
        {
            m_Pixels[i * 3] = static_cast<uint8_t>(colour & 0xFF);
            m_Pixels[i * 3 + 1] = static_cast<uint8_t>(colour >> 8);
            m_Pixels[i * 3 + 2] = alpha;
        }
        if (withAlpha)
            outSource = QuadBlit::MakeSource(m_Pixels, 4, 4, 3, true, true, false, nullptr);
        else
        {
            for (int i = 0; i < 16; ++i)
            {
                m_Packed[i * 2] = static_cast<uint8_t>(colour & 0xFF);
                m_Packed[i * 2 + 1] = static_cast<uint8_t>(colour >> 8);
            }
            outSource = QuadBlit::MakeSource(m_Packed, 4, 4, 2, false, true, false, nullptr);
        }
        outSource.pixelsPerMeter = sourcePpm;
        outPivotX = pivotX;
        outPivotY = pivotY;
        tr = tg = tb = ta = 255;
        return true;
    }

private:
    uint8_t m_Pixels[16 * 3] = {};
    uint8_t m_Packed[16 * 2] = {};
};

// A clip region that also sorts (like ClipComponent).
class TestClip : public Deki::Component, public Deki::IClipProvider, public Deki::ISortableProvider
{
public:
    DECLARE_COMPONENT_TYPE(TestClip, Deki::Component)
    float width = 1.0f, height = 1.0f;
    int32_t order = 0;
    float GetClipWidth() const override { return width; }
    float GetClipHeight() const override { return height; }
    int32_t GetSortingOrder() const override { return order; }
};

// A sorting group without any drawing (like SortingGroupComponent).
class TestSortGroup : public Deki::Component, public Deki::ISortableProvider
{
public:
    DECLARE_COMPONENT_TYPE(TestSortGroup, Deki::Component)
    int32_t order = 0;
    int32_t GetSortingOrder() const override { return order; }
};

void RegisterTestAdapters()
{
    static bool done = false;
    if (done) return;
    done = true;
    Deki::ComponentInterfaceAdapters::Register(Deki::IClipProvider::InterfaceID, TestClip::StaticType,
                                         [](Deki::Component* c) -> void* { return static_cast<Deki::IClipProvider*>(static_cast<TestClip*>(c)); });
    Deki::ComponentInterfaceAdapters::Register(Deki::ISortableProvider::InterfaceID, TestClip::StaticType,
                                         [](Deki::Component* c) -> void* { return static_cast<Deki::ISortableProvider*>(static_cast<TestClip*>(c)); });
    Deki::ComponentInterfaceAdapters::Register(Deki::ISortableProvider::InterfaceID, TestSortGroup::StaticType,
                                         [](Deki::Component* c) -> void* { return static_cast<Deki::ISortableProvider*>(static_cast<TestSortGroup*>(c)); });
}

// ---------------------------------------------------------------- scene helpers

struct SceneBuilder
{
    Deki::Scene scene;
    CameraComponent* camera = nullptr;

    SceneBuilder()
    {
        auto* camObj = new Deki::Object("Camera");
        camera = camObj->AddComponent<CameraComponent>();
        scene.AddObject(camObj);
    }

    Deki::Object* Object(const char* name, float x, float y, Deki::Object* parent = nullptr)
    {
        auto* o = new Deki::Object(name);
        o->SetX(x);
        o->SetY(y);
        if (parent) parent->AddChild(o);
        else scene.AddObject(o);
        return o;
    }

    TestRenderer* Sprite(const char* name, float x, float y, uint16_t colour, int order, Deki::Object* parent = nullptr)
    {
        auto* o = Object(name, x, y, parent);
        auto* r = o->AddComponent<TestRenderer>();
        r->colour = colour;
        r->sortingOrder = order;
        return r;
    }
};

uint64_t Fnv(const std::vector<uint8_t>& bytes, uint64_t h)
{
    for (uint8_t c : bytes)
    {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return h;
}

int TargetBpp(Deki::ColorFormat f)
{
    switch (f)
    {
    case Deki::ColorFormat::RGB565: return 2;
    case Deki::ColorFormat::RGB888: return 3;
    case Deki::ColorFormat::ARGB8888: return 4;
    case Deki::ColorFormat::RGB565A8: return 3;
    }
    return 0;
}

constexpr int kW = 64, kH = 48;

struct Case
{
    const char* name;
    std::function<void(SceneBuilder&)> build;
};

// Camera at the origin, default (inherited) pixels-per-meter of 16: the
// target covers 4 m x 3 m, screen centre is world (0, 0).
const Case kCases[] = {
    { "sort order", [](SceneBuilder& b) {
        b.Sprite("back", 0.0f, 0.0f, 0xF800, 10);   // red, drawn last despite insertion order
        b.Sprite("front", 0.05f, 0.05f, 0x07E0, 0);
        b.Sprite("mid", -0.05f, -0.05f, 0x001F, 5); } },
    { "sort stability", [](SceneBuilder& b) {
        b.Sprite("a", 0.0f, 0.0f, 0xF800, 3);
        b.Sprite("b", 0.05f, 0.0f, 0x07E0, 3);
        b.Sprite("c", 0.10f, 0.0f, 0x001F, 3); } },
    { "transparent container", [](SceneBuilder& b) {
        auto* group = b.Object("group", 0.5f, 0.0f);
        b.Sprite("child1", 0.0f, 0.0f, 0xFFE0, 2, group);
        b.Sprite("child2", 0.1f, 0.0f, 0x07FF, 1, group);
        b.Sprite("root", 0.55f, 0.0f, 0xF81F, 0); } },
    { "sort group", [](SceneBuilder& b) {
        auto* g = b.Object("g", -0.5f, 0.0f);
        g->AddComponent<TestSortGroup>()->order = -1;
        b.Sprite("in group", 0.0f, 0.0f, 0xF800, 50, g);
        b.Sprite("root", -0.45f, 0.0f, 0x07E0, 0); } },
    { "inactive subtree", [](SceneBuilder& b) {
        auto* off = b.Object("off", 0.0f, 0.0f);
        off->SetActive(false);
        b.Sprite("hidden", 0.0f, 0.0f, 0xF800, 0, off);
        b.Sprite("shown", 0.3f, 0.0f, 0x07E0, 0); } },
    { "nested clips depth 3", [](SceneBuilder& b) {
        Deki::Object* parent = nullptr;
        for (int i = 0; i < 3; ++i)
        {
            auto* c = b.Object("clip", i == 0 ? 0.0f : 0.05f, 0.0f, parent);
            auto* clip = c->AddComponent<TestClip>();
            clip->width = 0.5f - 0.1f * i;
            clip->height = 0.4f - 0.05f * i;
            parent = c;
        }
        auto* big = b.Sprite("big", 0.0f, 0.0f, 0xF800, 0, parent);
        big->sourcePpm = 4.0f;  // 4 px == 1 m: larger than every clip
    } },
    { "nested clips depth 40", [](SceneBuilder& b) {
        Deki::Object* parent = nullptr;
        for (int i = 0; i < 40; ++i)
        {
            auto* c = b.Object("clip", 0.0f, 0.0f, parent);
            auto* clip = c->AddComponent<TestClip>();
            clip->width = 2.0f - 0.03f * i;
            clip->height = 1.5f - 0.02f * i;
            parent = c;
        }
        auto* big = b.Sprite("big", 0.0f, 0.0f, 0x07E0, 0, parent);
        big->sourcePpm = 2.0f;
    } },
    { "ignoreClip", [](SceneBuilder& b) {
        auto* c = b.Object("clip", 0.0f, 0.0f);
        auto* clip = c->AddComponent<TestClip>();
        clip->width = 0.2f; clip->height = 0.2f;
        auto* big = b.Sprite("big", 0.0f, 0.0f, 0x001F, 0, c);
        big->sourcePpm = 4.0f;
        big->ignoreClip = true;
    } },
    { "renderer pixelSnap off", [](SceneBuilder& b) {
        auto* s = b.Sprite("s", 0.03f, 0.03f, 0xF800, 0);   // 0.48 px
        s->pixelSnap = false; } },
    { "renderer pixelSnap on", [](SceneBuilder& b) {
        auto* s = b.Sprite("s", 0.03f, 0.03f, 0xF800, 0);
        s->pixelSnap = true; } },
    { "camera pixelSnap off", [](SceneBuilder& b) {
        b.camera->pixelSnap = false;
        b.camera->GetOwner()->SetX(0.04f);
        b.camera->GetOwner()->SetY(-0.02f);
        b.Sprite("s", 0.0f, 0.0f, 0x07E0, 0)->pixelSnap = false;  // so the camera's snap is what differs
    } },
    { "camera pixelSnap on", [](SceneBuilder& b) {
        b.camera->pixelSnap = true;
        b.camera->GetOwner()->SetX(0.04f);
        b.camera->GetOwner()->SetY(-0.02f);
        b.Sprite("s", 0.0f, 0.0f, 0x07E0, 0)->pixelSnap = false;  // so the camera's snap is what differs
    } },
    { "camera ppm 24", [](SceneBuilder& b) {
        b.camera->pixelsPerMeter = 24.0f;
        b.Sprite("s", 0.2f, 0.1f, 0xFFE0, 0); } },
    { "parent rotation and scale", [](SceneBuilder& b) {
        auto* p = b.Object("p", 0.1f, 0.0f);
        p->SetRotation(0.7f);
        p->SetScale(1.5f, 0.75f);
        b.Sprite("child", 0.3f, 0.1f, 0xF81F, 0, p);
        auto* p2 = b.Object("p2", -0.5f, -0.3f);
        p2->SetScale(3.0f, 3.0f);
        b.Sprite("child2", 0.0f, 0.0f, 0x07FF, 0, p2); } },
    { "off-screen culled", [](SceneBuilder& b) {
        b.Sprite("far", 100.0f, 0.0f, 0xF800, 0);
        b.Sprite("near", 0.0f, 0.0f, 0x07E0, 0); } },
    { "alpha blend", [](SceneBuilder& b) {
        b.Sprite("under", 0.0f, 0.0f, 0xF800, 0);
        auto* over = b.Sprite("over", 0.06f, 0.06f, 0x001F, 1);
        over->withAlpha = true;
        over->alpha = 128;
        auto* edge = b.Sprite("edge", -1.95f, 1.45f, 0x07E0, 2);   // partly off the corner
        edge->withAlpha = true;
        edge->alpha = 200; } },
};

const Deki::ColorFormat kDstFmts[] = { Deki::ColorFormat::RGB565, Deki::ColorFormat::RGB888,
                                     Deki::ColorFormat::ARGB8888, Deki::ColorFormat::RGB565A8 };

uint64_t RunTarget(Deki::ColorFormat fmt, bool print)
{
    RegisterTestAdapters();
    uint64_t hash = 0xcbf29ce484222325ULL;
    const int bpp = TargetBpp(fmt);
    for (const Case& c : kCases)
    {
        SceneBuilder b;
        c.build(b);
        std::vector<uint8_t> target(static_cast<size_t>(kW) * kH * bpp, 0);
        Standard2DRenderer renderer;
        RenderContext ctx{ b.camera, target.data(), kW, kH, fmt };
        renderer.Render(&b.scene, ctx);
        QuadBlit::ClearClipStack();
        const uint64_t caseHash = Fnv(target, 0xcbf29ce484222325ULL);
        hash = (hash ^ caseHash) * 0x100000001b3ULL;
        if (print)
            std::printf("  %-28s %016llx\n", c.name, static_cast<unsigned long long>(caseHash));
    }
    return hash;
}

// 0 means "not captured yet": the test prints the actual value and fails.
struct Expected { Deki::ColorFormat fmt; const char* name; uint64_t hash; };
const Expected kExpected[] = {
    // Re-pinned 2026-09-03 when the QuadBlit clip stack lost its 16-slot cap: only
    // the "nested clips depth 40" case changed (levels 17..40 used to be clipped
    // by level 16's rect); every other case hash is identical to the first pin.
    { Deki::ColorFormat::RGB565,   "RGB565",   0x1d8eb3cd8824484dULL },
    { Deki::ColorFormat::RGB888,   "RGB888",   0x57549d41cf335c9bULL },
    { Deki::ColorFormat::ARGB8888, "ARGB8888", 0x99d8e38d9ca41e9fULL },
    { Deki::ColorFormat::RGB565A8, "RGB565A8", 0xb9a61fd457e7a6cfULL },
};

}  // namespace

class RendererGoldenTest : public ::testing::TestWithParam<int>
{
};

TEST_P(RendererGoldenTest, TargetFormatMatchesGolden)
{
    const Expected& e = kExpected[GetParam()];
    const uint64_t actual = RunTarget(e.fmt, std::getenv("DEKI_GOLDEN_PRINT") != nullptr);
    std::printf("RENDERER GOLDEN %s = 0x%016llxULL\n", e.name, static_cast<unsigned long long>(actual));
    if (actual != e.hash)
    {
        std::printf("Per-case hashes for %s:\n", e.name);
        RunTarget(e.fmt, true);
    }
    EXPECT_EQ(actual, e.hash) << "Standard2DRenderer output changed for target " << e.name
                              << ". If intended, update kExpected and say so in the commit.";
}

INSTANTIATE_TEST_SUITE_P(Formats, RendererGoldenTest, ::testing::Values(0, 1, 2, 3));

// The renderer must not leave clip state behind, whatever the nesting depth.
TEST(RendererGoldenTest, ClipStackBalancedAfterDeepNesting)
{
    RegisterTestAdapters();
    SceneBuilder b;
    kCases[6].build(b);  // nested clips depth 40
    std::vector<uint8_t> target(static_cast<size_t>(kW) * kH * 2, 0);
    Standard2DRenderer renderer;
    RenderContext ctx{ b.camera, target.data(), kW, kH, Deki::ColorFormat::RGB565 };
    renderer.Render(&b.scene, ctx);
    EXPECT_EQ(QuadBlit::GetClipStackDepth(), 0);
}

// A clip and a renderer on ONE object must render like the clip on a parent
// with the renderer on a child at the same world position: the clip is pushed
// before the object's own content is drawn (one component walk resolves both).
TEST(RendererGoldenTest, ClipAndRendererOnOneObjectMatchesParentChildForm)
{
    RegisterTestAdapters();
    auto render = [](bool sameObject) {
        SceneBuilder b;
        Deki::Object* holder = b.Object("holder", 0.1f, -0.05f);
        auto* clip = holder->AddComponent<TestClip>();
        clip->width = 0.15f;
        clip->height = 0.15f;
        TestRenderer* r = sameObject ? holder->AddComponent<TestRenderer>()
                                     : b.Sprite("child", 0.0f, 0.0f, 0x001F, 0, holder);
        r->colour = 0x001F;
        r->sourcePpm = 4.0f;  // 1 m square: far larger than the clip
        std::vector<uint8_t> target(static_cast<size_t>(kW) * kH * 2, 0);
        Standard2DRenderer renderer;
        RenderContext ctx{ b.camera, target.data(), kW, kH, Deki::ColorFormat::RGB565 };
        renderer.Render(&b.scene, ctx);
        QuadBlit::ClearClipStack();
        return target;
    };
    const std::vector<uint8_t> same = render(true);
    const std::vector<uint8_t> split = render(false);
    EXPECT_EQ(same, split);
    // ... and the clip really applied: the far corner stays background.
    EXPECT_EQ(same[0], 0u);
    EXPECT_EQ(same[1], 0u);
    // ... while the centre is blue.
    const size_t centre = (static_cast<size_t>(kH / 2) * kW + kW / 2 + 1) * 2;
    EXPECT_EQ(same[centre] | (same[centre + 1] << 8), 0x001F);
}

// A component type that gains an IClipProvider adapter AFTER the renderer has
// already classified it (a package loading later) must clip from the next
// frame on: the per-type cache is dropped when the adapter registry changes.
class TestLateClip : public Deki::Component, public Deki::IClipProvider
{
public:
    DECLARE_COMPONENT_TYPE(TestLateClip, Deki::Component)
    float GetClipWidth() const override { return 0.15f; }
    float GetClipHeight() const override { return 0.15f; }
};

TEST(RendererGoldenTest, AdapterRegisteredAfterFirstFrameTakesEffect)
{
    RegisterTestAdapters();
    SceneBuilder b;
    Deki::Object* holder = b.Object("holder", 0.0f, 0.0f);
    holder->AddComponent<TestLateClip>();
    // Like ClipComponent, a real clip is also sortable so the renderer claims
    // it; an unclaimed container's clip is never pushed (containers only
    // float their children up).
    holder->AddComponent<TestSortGroup>();
    auto* big = b.Sprite("big", 0.0f, 0.0f, 0xF800, 0, holder);
    big->sourcePpm = 4.0f;

    Standard2DRenderer renderer;
    std::vector<uint8_t> target(static_cast<size_t>(kW) * kH * 2, 0);
    RenderContext ctx{ b.camera, target.data(), kW, kH, Deki::ColorFormat::RGB565 };
    renderer.Render(&b.scene, ctx);
    QuadBlit::ClearClipStack();
    // 6 px from the centre: inside the 16 px sprite, outside the 2.4 px clip.
    const size_t corner = (static_cast<size_t>(kH / 2 - 6) * kW + kW / 2 - 6) * 2;
    EXPECT_EQ(target[corner] | (target[corner + 1] << 8), 0xF800) << "no adapter yet: unclipped";

    Deki::ComponentInterfaceAdapters::Register(Deki::IClipProvider::InterfaceID, TestLateClip::StaticType,
                                         [](Deki::Component* c) -> void* { return static_cast<Deki::IClipProvider*>(static_cast<TestLateClip*>(c)); });
    std::fill(target.begin(), target.end(), 0);
    renderer.Render(&b.scene, ctx);
    EXPECT_EQ(QuadBlit::GetClipStackDepth(), 0);
    EXPECT_EQ(target[corner] | (target[corner + 1] << 8), 0x0000) << "adapter registered: clipped";
    const size_t centre = (static_cast<size_t>(kH / 2) * kW + kW / 2) * 2;
    EXPECT_EQ(target[centre] | (target[centre + 1] << 8), 0xF800);
}

// With dirty tracking on, every case renders the same pixels, and every pixel
// that differs from the background lies inside the reported region.
TEST(RendererGoldenTest, DirtyTrackingIsPixelIdenticalAndCoversEveryDrawnPixel)
{
    RegisterTestAdapters();
    for (const Case& c : kCases)
    {
        SceneBuilder b;
        c.build(b);
        std::vector<uint8_t> plain(static_cast<size_t>(kW) * kH * 2, 0);
        std::vector<uint8_t> tracked(static_cast<size_t>(kW) * kH * 2, 0);

        Standard2DRenderer r1;
        RenderContext c1{ b.camera, plain.data(), kW, kH, Deki::ColorFormat::RGB565 };
        r1.Render(&b.scene, c1);
        QuadBlit::ClearClipStack();
        EXPECT_EQ(r1.GetLastFrameDirty(), nullptr) << c.name << ": untracked render reports nothing";

        Standard2DRenderer r2;
        RenderContext c2{ b.camera, tracked.data(), kW, kH, Deki::ColorFormat::RGB565 };
        c2.trackDirty = true;
        r2.Render(&b.scene, c2);
        QuadBlit::ClearClipStack();
        EXPECT_EQ(plain, tracked) << c.name;
        EXPECT_EQ(QuadBlit::GetDirtyTrackedTarget(), nullptr) << "tracking left on after the frame";

        const DirtyRegion* d = r2.GetLastFrameDirty();
        ASSERT_NE(d, nullptr) << c.name;
        int uncovered = 0;
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x)
            {
                const size_t i = (static_cast<size_t>(y) * kW + x) * 2;
                if ((tracked[i] | tracked[i + 1]) != 0 && !d->Contains(x, y))
                    ++uncovered;
            }
        EXPECT_EQ(uncovered, 0) << c.name << ": drawn pixels outside the dirty region";
    }
}
