// The 2D light on the GPU: GLLight2DShader.inl is what the editor compiles
// editor/Light2DShader.cpp's HLSL to, and a frame drawn on the GPU (the
// presenter and its blit sink) is lit as the CPU lights it (Lighting2D). The
// CPU works the light out per block and blends between blocks; the GPU works
// it out at every pixel, so the two agree to within a few steps. And what a
// frame draws is lit, never the colour it was cleared to. On a machine with
// no OpenGL the drawing tests skip.

#ifdef DEKI_CAPABILITY_OPENGL

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <deki/Object.h>
#include <deki/Scene.h>
#include <deki-editor/ShaderCompiler.h>

#include "deki-rendering/CameraComponent.h"
#include "deki-rendering/DekiRenderer.h"
#include "deki-rendering/GLApi.h"
#include "deki-rendering/GLPresenter.h"
#include "deki-rendering/Light.h"
#include "deki-rendering/Lighting2D.h"
#include "deki-rendering/PixelFormat.h"
#include "deki-rendering/QuadBlit.h"
#include "deki-rendering/RendererComponent.h"
#include "deki-rendering/Standard2DRenderer.h"
#include "deki-rendering/editor/Light2DShader.h"
#include "ScopedArtDensity.h"

using namespace DekiGL;
using DekiRendering::Light;
using DekiRendering::Lighting2D;
using DekiRendering::LightType;

namespace
{

constexpr int kW = 64;
constexpr int kH = 48;
constexpr Deki::ColorFormat kFormat = Deki::ColorFormat::RGB565A8;
constexpr uint8_t kClear[3] = { 200, 180, 160 };

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

#define REQUIRE_PRESENTER(p)                                                                                           \
    std::unique_ptr<GLPresenter> p = MakePresenter();                                                                  \
    if (!p)                                                                                                            \
    {                                                                                                                  \
        GTEST_SKIP() << "no OpenGL on this machine";                                                                   \
    }                                                                                                                  \
    ASSERT_TRUE(p->Draws2D()) << "the 2D shader does not build"

DekiRendering::FrameCamera Camera()
{
    DekiRendering::FrameCamera cam;
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

// A frame of one colour lit whole on the GPU: read back, top row first, RGBA8.
std::vector<uint8_t> LitOnGpu(GLPresenter& presenter, const Lighting2D& lighting)
{
    std::vector<uint8_t> buffer(Deki::FrameBufferBytes(kFormat, kW, kH), 0);
    QuadBlit::ClearClipStack();
    EXPECT_TRUE(QuadBlit::BeginSinkFrame(buffer.data(), kW, kH, kFormat, kClear[0], kClear[1], kClear[2]));
    EXPECT_TRUE(QuadBlit::LightSinkFrame(lighting));
    QuadBlit::EndSinkFrame();
    presenter.PushLayer(buffer.data(), kW, kH, kFormat);
    std::vector<uint8_t> frame;
    EXPECT_TRUE(presenter.ReadFrame(frame));
    return frame;
}

// The same on the CPU, as RGBA8.
std::vector<uint8_t> LitOnCpu(Lighting2D& lighting)
{
    std::vector<uint8_t> buffer(Deki::FrameBufferBytes(kFormat, kW, kH));
    for (size_t i = 0; i < static_cast<size_t>(kW) * kH; ++i)
    {
        DekiPixel::WriteDstPixel<kFormat>(buffer.data(), i, kClear[0], kClear[1], kClear[2], 255);
    }
    lighting.Apply(buffer.data(), kW, kH, kFormat, Deki::Rect{}, 0, 0);
    std::vector<uint8_t> rgba(static_cast<size_t>(kW) * kH * 4);
    for (size_t i = 0; i < static_cast<size_t>(kW) * kH; ++i)
    {
        DekiPixel::ReadDstPixel<kFormat>(buffer.data(), i, rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2],
                                         rgba[i * 4 + 3]);
    }
    return rgba;
}

// The largest difference in any channel between the two, over every pixel.
int Furthest(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int& atX, int& atY)
{
    int worst = 0;
    for (int y = 0; y < kH; y++)
    {
        for (int x = 0; x < kW; x++)
        {
            for (int c = 0; c < 3; c++)
            {
                const size_t i = (static_cast<size_t>(y) * kW + x) * 4 + c;
                const int d = std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
                if (d > worst)
                {
                    worst = d;
                    atX = x;
                    atY = y;
                }
            }
        }
    }
    return worst;
}

}  // namespace

TEST(GLLighting, TheLightShaderIsWhatItsHlslCompilesTo)
{
    if (!DekiEditor::ShaderCompiler::IsAvailable())
    {
        GTEST_SKIP() << "no HLSL compiler (Slang) beside the tests";
    }
    DekiEditor::ShaderStageText vertex, fragment;
    std::string errors;
    DekiEditor::ShaderCompileRequest request;
    request.source = DekiRendering::Light2DShaderHlsl();
    request.fileName = "the 2D light";
    request.stage = DekiEditor::ShaderStage::Vertex;
    request.entry = "VertexMain";
    ASSERT_TRUE(DekiEditor::ShaderCompiler::Compile(request, vertex, errors)) << errors;
    request.stage = DekiEditor::ShaderStage::Fragment;
    request.entry = "FragmentMain";
    ASSERT_TRUE(DekiEditor::ShaderCompiler::Compile(request, fragment, errors)) << errors;

    auto literal = [](const char* name, const std::string& text)
    { return "const char* const " + std::string(name) + " = R\"deki(" + text + ")deki\";\n"; };
    const std::string generated = "// The 2D light program: editor/Light2DShader.cpp, as the editor's compiler\n"
                                  "// writes it. Generated by tests/GLLightingTests.cpp\n"
                                  "// (TheLightShaderIsWhatItsHlslCompilesTo); do not edit.\n\n" +
                                  literal("kLight2DVertexES", vertex.es) + "\n" +
                                  literal("kLight2DFragmentES", fragment.es) + "\n" +
                                  literal("kLight2DVertexGL", vertex.gl) + "\n" +
                                  literal("kLight2DFragmentGL", fragment.gl);

    const std::string path = std::string(DEKI_RENDERING_SOURCE_DIR) + "/GLLight2DShader.inl";
    std::string current;
    if (FILE* in = std::fopen(path.c_str(), "rb"))
    {
        char chunk[4096];
        for (size_t n; (n = std::fread(chunk, 1, sizeof(chunk), in)) > 0;)
        {
            current.append(chunk, n);
        }
        std::fclose(in);
    }
    // A Windows checkout may have given it CRLF line ends.
    current.erase(std::remove(current.begin(), current.end(), '\r'), current.end());
    if (current != generated && std::getenv("DEKI_UPDATE_GENERATED"))
    {
        FILE* out = std::fopen(path.c_str(), "wb");
        ASSERT_NE(out, nullptr) << path;
        std::fwrite(generated.data(), 1, generated.size(), out);
        std::fclose(out);
        current = generated;
    }
    EXPECT_EQ(current, generated) << path
                                  << " is stale: run the deki-rendering tests with DEKI_UPDATE_GENERATED=1";
}

TEST(GLLighting, AmbientLightsTheGpusFrameAsTheCpus)
{
    REQUIRE_PRESENTER(presenter);
    Deki::Scene scene;
    Light* ambient = AddLight(scene, LightType::Ambient);
    ambient->color = Deki::Color(120, 160, 255);
    ambient->intensity = 0.8f;
    Lighting2D lighting;
    lighting.Build(&scene, Camera(), kW, kH, 0);
    const std::vector<uint8_t> gpu = LitOnGpu(*presenter, lighting);
    const std::vector<uint8_t> cpu = LitOnCpu(lighting);
    int x = 0, y = 0;
    EXPECT_LE(Furthest(gpu, cpu, x, y), 10) << "at " << x << "," << y;
}

TEST(GLLighting, PoolsLightTheGpusFrameAsTheCpus)
{
    REQUIRE_PRESENTER(presenter);
    Deki::Scene scene;
    AddLight(scene, LightType::Ambient)->intensity = 0.3f;
    Light* pool = AddLight(scene, LightType::Point, -0.8f, 0.3f);
    pool->range = 1.0f;
    pool->color = Deki::Color(255, 190, 110);
    pool->intensity = 1.4f;  // past the colour in its middle
    Light* other = AddLight(scene, LightType::Point, 0.7f, -0.4f);
    other->range = 1.3f;
    other->color = Deki::Color(120, 200, 255);
    Lighting2D lighting;
    lighting.Build(&scene, Camera(), kW, kH, 0);
    const std::vector<uint8_t> gpu = LitOnGpu(*presenter, lighting);
    const std::vector<uint8_t> cpu = LitOnCpu(lighting);
    int x = 0, y = 0;
    // The CPU's light is the blocks' blended and dithered, in steps of 1/32.
    EXPECT_LE(Furthest(gpu, cpu, x, y), 24) << "at " << x << "," << y;
}

TEST(GLLighting, ConesLightTheGpusFrameAsTheCpusButAtTheirEdgesNearTheTip)
{
    REQUIRE_PRESENTER(presenter);
    Deki::Scene scene;
    AddLight(scene, LightType::Ambient)->intensity = 0.3f;
    Light* spot = AddLight(scene, LightType::Spot, 0.9f, -0.5f);
    spot->range = 1.4f;
    spot->spotAngle = 70.0f;
    spot->GetOwner()->SetRotation(1.2f);
    Lighting2D lighting;
    lighting.Build(&scene, Camera(), kW, kH, 0);
    const std::vector<uint8_t> gpu = LitOnGpu(*presenter, lighting);
    const std::vector<uint8_t> cpu = LitOnCpu(lighting);
    // Near its tip a cone's edge crosses a block in a pixel or two; the CPU's
    // blocks blend it over four, the GPU's pixels do not. Further out, and
    // inside the cone, they agree.
    const float tipX = kW / 2.0f + 0.9f * 16.0f, tipY = kH / 2.0f + 0.5f * 16.0f;
    int off = 0;
    for (int py = 0; py < kH; py++)
    {
        for (int px = 0; px < kW; px++)
        {
            const size_t i = (static_cast<size_t>(py) * kW + px) * 4;
            int d = 0;
            for (int c = 0; c < 3; c++)
            {
                d = std::max(d, std::abs(static_cast<int>(gpu[i + c]) - static_cast<int>(cpu[i + c])));
            }
            const float dx = px + 0.5f - tipX, dy = py + 0.5f - tipY;
            if (dx * dx + dy * dy > 16.0f * 16.0f)
            {
                EXPECT_LE(d, 24) << "at " << px << "," << py << ", away from the tip";
            }
            off += d > 24 ? 1 : 0;
        }
    }
    EXPECT_LE(off, kW * kH * 4 / 100) << off << " pixels apart";
}

TEST(GLLighting, AFrameWithoutLightIsLeftAlone)
{
    REQUIRE_PRESENTER(presenter);
    Deki::Scene scene;
    Lighting2D lighting;
    lighting.Build(&scene, Camera(), kW, kH, 0);
    ASSERT_FALSE(lighting.On());
    std::vector<uint8_t> buffer(Deki::FrameBufferBytes(kFormat, kW, kH), 0);
    ASSERT_TRUE(QuadBlit::BeginSinkFrame(buffer.data(), kW, kH, kFormat, kClear[0], kClear[1], kClear[2]));
    QuadBlit::EndSinkFrame();
    presenter->PushLayer(buffer.data(), kW, kH, kFormat);
    std::vector<uint8_t> frame;
    ASSERT_TRUE(presenter->ReadFrame(frame));
    EXPECT_NEAR(frame[0], kClear[0], 4);
    EXPECT_NEAR(frame[1], kClear[1], 4);
    EXPECT_NEAR(frame[2], kClear[2], 4);
}

namespace
{
constexpr uint8_t kSquare[3] = { 200, 100, 40 };

// An opaque square of kSquare, 8 pixels a side.
struct Square
{
    uint8_t pixels[8 * 8 * 2];
    QuadBlit::Source source;
    Square()
    {
        for (int i = 0; i < 64; i++)
        {
            DekiPixel::Store565(&pixels[i * 2], DekiPixel::PackRGB565(kSquare[0], kSquare[1], kSquare[2]));
        }
        source = QuadBlit::MakeSource(pixels, 8, 8, QuadBlit::PixelLayout::RGB565(), false, nullptr);
    }
};

// What the frame holds at (x, y), top row first.
const uint8_t* PixelAt(const std::vector<uint8_t>& frame, int x, int y)
{
    return &frame[(static_cast<size_t>(y) * kW + x) * 4];
}
}  // namespace

// Set apart from the colour the frame started as (BeginLit), what the GPU
// draws is lit and that colour is not: round it, and through what covers it
// in part.
TEST(GLLighting, TheGpuLightsWhatIsDrawnAndNotTheColourUnderIt)
{
    REQUIRE_PRESENTER(presenter);
    Deki::Scene scene;
    AddLight(scene, LightType::Ambient)->intensity = 0.5f;
    Lighting2D lighting;
    lighting.Build(&scene, Camera(), kW, kH, 0);

    Square square;
    std::vector<uint8_t> buffer(Deki::FrameBufferBytes(kFormat, kW, kH), 0);
    QuadBlit::ClearClipStack();
    ASSERT_TRUE(QuadBlit::BeginSinkFrame(buffer.data(), kW, kH, kFormat, kClear[0], kClear[1], kClear[2]));
    ASSERT_TRUE(QuadBlit::BeginLitSinkFrame());
    QuadBlit::BlitScaled(square.source, buffer.data(), kW, kH, kFormat, 8, 8, 8, 8, 255, 255, 255, 255, false);
    QuadBlit::BlitScaled(square.source, buffer.data(), kW, kH, kFormat, 40, 8, 8, 8, 255, 255, 255, 128, false);
    EXPECT_TRUE(QuadBlit::LightSinkFrame(lighting));
    QuadBlit::EndSinkFrame();
    presenter->PushLayer(buffer.data(), kW, kH, kFormat);
    std::vector<uint8_t> frame;
    ASSERT_TRUE(presenter->ReadFrame(frame));

    for (int c = 0; c < 3; c++)
    {
        EXPECT_NEAR(PixelAt(frame, 0, 0)[c], kClear[c], 1) << "the frame's colour, channel " << c;
        EXPECT_NEAR(PixelAt(frame, 30, 12)[c], kClear[c], 1) << "between the squares, channel " << c;
        EXPECT_NEAR(PixelAt(frame, 12, 12)[c], kSquare[c] / 2, 6) << "the square, lit, channel " << c;
        // Half the square lit, half the frame's colour as it is.
        const int through = (kSquare[c] / 2 * 128 + kClear[c] * 127) / 255;
        EXPECT_NEAR(PixelAt(frame, 44, 12)[c], through, 8) << "the square half covering, channel " << c;
    }
    EXPECT_EQ(PixelAt(frame, 0, 0)[3], 255) << "the frame is opaque again";
    EXPECT_EQ(PixelAt(frame, 44, 12)[3], 255);
}

namespace
{
// A square of kSquare that may say it lights itself, as a lit mesh does.
class GpuPatch : public DekiRendering::RendererComponent
{
public:
    static const Deki::ComponentTypeInfo kTypeInfo;
    GpuPatch() { SetTypeInfo(&kTypeInfo); }

    bool selfLit = false;

    bool GetContentExtents(float& outW, float& outH) const override
    {
        outW = outH = 8.0f / 16.0f;
        return true;
    }
    bool RenderContent(const Deki::Object*, QuadBlit::Source& outSource, float& outPivotX, float& outPivotY,
                       uint8_t& tr, uint8_t& tg, uint8_t& tb, uint8_t& ta) override
    {
        outSource = m_Square.source;
        outSource.pixelsPerMeter = 16.0f;
        outPivotX = outPivotY = 0.5f;
        tr = tg = tb = ta = 255;
        return true;
    }
    bool LightsItself() const override { return selfLit; }

private:
    Square m_Square;
};
const Deki::ComponentTypeInfo GpuPatch::kTypeInfo =
    Deki::MakeHandWrittenTypeInfo<GpuPatch, DekiRendering::RendererComponent>(/*declaresUpdate=*/false);
}  // namespace

// The renderer drawing a frame on the GPU: a sky's colour under a sun and an
// ambient light that add up to 1.65 stays the colour set, with nothing drawn
// under the light (the one renderer lights itself, as a lit mesh does) and
// with something lit beside it.
TEST(GLLighting, TheRendererKeepsTheClearColourOutOfTheGpusLight)
{
    REQUIRE_PRESENTER(presenter);
    for (bool lit2D : { false, true })
    {
        ScopedArtDensity art;
        Deki::Scene scene;
        auto* camObj = new Deki::Object("Camera");
        auto* camera = camObj->AddComponent<DekiRendering::CameraComponent>();
        camera->orthoHeight = 3.0f;  // 16 px/m on 64 x 48
        camera->clearColor = Deki::Color(kClear[0], kClear[1], kClear[2]);
        scene.AddObject(camObj);
        AddLight(scene, LightType::Ambient)->intensity = 0.55f;
        AddLight(scene, LightType::Directional)->intensity = 1.1f;
        auto add = [&scene](float x, int order, bool selfLit)
        {
            auto* o = new Deki::Object("Patch");
            o->SetX(x);
            auto* p = o->AddComponent<GpuPatch>();
            p->sortingOrder = order;
            p->selfLit = selfLit;
            scene.AddObject(o);
        };
        add(1.0f, 1, true);
        if (lit2D)
        {
            add(-1.0f, 0, false);
        }

        DekiRendering::Standard2DRenderer renderer;
        std::vector<uint8_t> buffer(Deki::FrameBufferBytes(kFormat, kW, kH), 0);
        QuadBlit::ClearClipStack();
        ASSERT_TRUE(QuadBlit::BeginSinkFrame(buffer.data(), kW, kH, kFormat, kClear[0], kClear[1], kClear[2]));
        DekiRendering::RenderContext ctx{ camera, buffer.data(), kW, kH, kFormat };
        renderer.Render(&scene, ctx);
        QuadBlit::EndSinkFrame();
        presenter->PushLayer(buffer.data(), kW, kH, kFormat);
        std::vector<uint8_t> frame;
        ASSERT_TRUE(presenter->ReadFrame(frame));

        for (int c = 0; c < 3; c++)
        {
            EXPECT_NEAR(PixelAt(frame, 0, 0)[c], kClear[c], 1) << "the clear colour, channel " << c << ", " << lit2D;
            EXPECT_NEAR(PixelAt(frame, kW / 2, kH / 2)[c], kClear[c], 1) << "between the patches, channel " << c;
            EXPECT_NEAR(PixelAt(frame, kW / 2 + 16, kH / 2)[c], kSquare[c], 6) << "what lights itself, channel " << c;
        }
        if (lit2D)
        {
            // 1.65 times its colour, up to white.
            EXPECT_GE(PixelAt(frame, kW / 2 - 16, kH / 2)[0], 250);
            EXPECT_NEAR(PixelAt(frame, kW / 2 - 16, kH / 2)[1], 165, 8);
        }
    }
}

#endif  // DEKI_CAPABILITY_OPENGL
