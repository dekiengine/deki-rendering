/**
 * @file RenderPassHookTests.cpp
 * @brief RenderPass::HookMask: the renderer calls a pass only for the hooks
 *        it declares, the default mask keeps every hook, and the mask is
 *        re-read each frame.
 */

#include <gtest/gtest.h>
#include <cstdint>
#include <vector>

#include "DekiObject.h"
#include "Scene.h"
#include "CameraComponent.h"
#include "RendererComponent.h"
#include "RenderPass.h"
#include "Standard2DRenderer.h"
#include "QuadBlit.h"

namespace
{

class CountingPass : public RenderPass
{
public:
    uint32_t mask = RenderPassHooks::All;
    int begin = 0, pre = 0, exec = 0, post = 0, end = 0;

    uint32_t HookMask() const override { return mask; }
    void BeginFrame(RenderContext&) override { ++begin; }
    void PreExecute(Deki::Object*, RenderContext&) override { ++pre; }
    void Execute(Deki::Object*, RenderContext&) override { ++exec; }
    void PostExecute(Deki::Object*, RenderContext&) override { ++post; }
    void EndFrame(RenderContext&) override { ++end; }
};

// Something the renderer will claim so RenderObject runs (a sort group with
// no drawing would do, but a renderer is the common case).
class DotRenderer : public RendererComponent
{
public:
    DECLARE_COMPONENT_TYPE(DotRenderer, RendererComponent)
    bool RenderContent(const Deki::Object*, QuadBlit::Source& out, float& px, float& py,
                       uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) override
    {
        out = QuadBlit::MakeSource(m_Pixel, 1, 1, 2, false, true, false, nullptr);
        px = py = 0.5f;
        r = g = b = a = 255;
        return true;
    }
private:
    uint8_t m_Pixel[2] = { 0xFF, 0xFF };
};

struct HookScene
{
    Deki::Scene scene;
    CameraComponent* camera;
    std::vector<uint8_t> target;
    static constexpr int kObjects = 5;

    HookScene() : target(32 * 32 * 2, 0)
    {
        auto* camObj = new Deki::Object("Camera");
        camera = camObj->AddComponent<CameraComponent>();
        scene.AddObject(camObj);
        for (int i = 0; i < kObjects; ++i)
        {
            auto* o = new Deki::Object("dot");
            o->AddComponent<DotRenderer>();
            scene.AddObject(o);
        }
    }

    void Render(Standard2DRenderer& renderer)
    {
        RenderContext ctx{ camera, target.data(), 32, 32, Deki::ColorFormat::RGB565 };
        renderer.Render(&scene, ctx);
        QuadBlit::ClearClipStack();
    }
};

}  // namespace

TEST(RenderPassHooks, DefaultMaskReceivesEveryHook)
{
    HookScene s;
    Standard2DRenderer renderer;
    CountingPass pass;
    renderer.AddPass(&pass);
    s.Render(renderer);
    EXPECT_EQ(pass.begin, 1);
    EXPECT_EQ(pass.pre, HookScene::kObjects);
    EXPECT_EQ(pass.exec, HookScene::kObjects);
    EXPECT_EQ(pass.post, HookScene::kObjects);
    EXPECT_EQ(pass.end, 1);
}

TEST(RenderPassHooks, ExecuteOnlyMaskSkipsTheOtherHooks)
{
    HookScene s;
    Standard2DRenderer renderer;
    CountingPass pass;
    pass.mask = RenderPassHooks::Execute;
    renderer.AddPass(&pass);
    s.Render(renderer);
    EXPECT_EQ(pass.begin, 0);
    EXPECT_EQ(pass.pre, 0);
    EXPECT_EQ(pass.exec, HookScene::kObjects);
    EXPECT_EQ(pass.post, 0);
    EXPECT_EQ(pass.end, 0);
}

TEST(RenderPassHooks, MaskIsReadEveryFrame)
{
    HookScene s;
    Standard2DRenderer renderer;
    CountingPass pass;
    pass.mask = RenderPassHooks::BeginFrame | RenderPassHooks::EndFrame;
    renderer.AddPass(&pass);
    s.Render(renderer);
    EXPECT_EQ(pass.begin, 1);
    EXPECT_EQ(pass.exec, 0);

    pass.mask = RenderPassHooks::Execute;
    s.Render(renderer);
    EXPECT_EQ(pass.begin, 1);  // unchanged
    EXPECT_EQ(pass.exec, HookScene::kObjects);
}

TEST(RenderPassHooks, RemovedPassReceivesNothing)
{
    HookScene s;
    Standard2DRenderer renderer;
    CountingPass pass;
    renderer.AddPass(&pass);
    renderer.RemovePass(&pass);
    s.Render(renderer);
    EXPECT_EQ(pass.begin + pass.pre + pass.exec + pass.post + pass.end, 0);
}

TEST(RenderPassHooks, FrameCameraIsCapturedForPasses)
{
    struct CamProbe : RenderPass
    {
        bool valid = false;
        float halfW = 0.0f;
        uint32_t HookMask() const override { return RenderPassHooks::Execute; }
        void Execute(Deki::Object*, RenderContext& ctx) override { valid = ctx.cam.valid; halfW = ctx.cam.halfW; }
    };
    HookScene s;
    Standard2DRenderer renderer;
    CamProbe probe;
    renderer.AddPass(&probe);
    s.Render(renderer);
    EXPECT_TRUE(probe.valid);
    EXPECT_EQ(probe.halfW, 16.0f);
}
