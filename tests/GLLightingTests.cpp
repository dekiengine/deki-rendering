// The 2D light on the GPU: GLLight2DShader.inl is what the editor compiles
// editor/Light2DShader.cpp's HLSL to, and a frame drawn on the GPU (the
// presenter and its blit sink) is lit as the CPU lights it (Lighting2D). The
// CPU works the light out per block and blends between blocks; the GPU works
// it out at every pixel, so the two agree to within a few steps. On a machine
// with no OpenGL the drawing tests skip.

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

#include "deki-rendering/GLApi.h"
#include "deki-rendering/GLPresenter.h"
#include "deki-rendering/Light.h"
#include "deki-rendering/Lighting2D.h"
#include "deki-rendering/PixelFormat.h"
#include "deki-rendering/QuadBlit.h"
#include "deki-rendering/editor/Light2DShader.h"

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

// The clear colour lit on the GPU: the frame read back, top row first, RGBA8.
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

#endif  // DEKI_CAPABILITY_OPENGL
