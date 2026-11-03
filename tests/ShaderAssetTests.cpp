// Tests the .shader asset: the compiled form the editor writes and a device
// loads, how the editor finds the hooks a file defines, and compiling a file
// for each use, with every problem said (file and line).

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "../ShaderAsset.h"
#include "../editor/ShaderUses.h"

#include <deki-editor/ShaderCompiler.h>

using DekiRendering::ShaderAsset;
using DekiRendering::ShaderProgram;

namespace
{

ShaderProgram MakeProgram(const char* use, const char* tag)
{
    ShaderProgram p;
    p.use = use;
    p.es = { std::string("es vertex ") + tag, std::string("es fragment ") + tag };
    p.gl = { std::string("gl vertex ") + tag, std::string("gl fragment ") + tag };
    return p;
}

bool HaveCompiler()
{
    return DekiEditor::ShaderCompiler::IsAvailable();
}

}  // namespace

TEST(ShaderAsset, LoadsWhatTheEditorWrote)
{
    const std::vector<uint8_t> blob = ShaderAsset::Encode({ MakeProgram("mesh", "a"), MakeProgram("effect", "b") });
    ShaderAsset s;
    ASSERT_TRUE(s.Parse(blob.data(), blob.size()));
    ASSERT_EQ(s.Programs().size(), 2u);
    ASSERT_NE(s.Program("mesh", true), nullptr);
    EXPECT_EQ(s.Program("mesh", true)->vertex, "es vertex a");
    EXPECT_EQ(s.Program("mesh", false)->fragment, "gl fragment a");
    EXPECT_EQ(s.Program("effect", true)->fragment, "es fragment b");
    EXPECT_EQ(s.Program("particles", true), nullptr);
}

TEST(ShaderAsset, RefusesWhatIsNotACompiledShader)
{
    ShaderAsset s;
    EXPECT_FALSE(s.Parse(nullptr, 0));
    const std::string text = "float4 Shade(Fragment f) { return f.base; }";
    EXPECT_FALSE(s.Parse(reinterpret_cast<const uint8_t*>(text.data()), text.size()));

    std::vector<uint8_t> blob = ShaderAsset::Encode({ MakeProgram("mesh", "a") });
    blob.resize(blob.size() - 3);  // cut short
    EXPECT_FALSE(s.Parse(blob.data(), blob.size()));
    EXPECT_TRUE(s.Programs().empty());
}

TEST(ShaderAsset, EveryLoadIsANewRevision)
{
    const std::vector<uint8_t> blob = ShaderAsset::Encode({ MakeProgram("mesh", "a") });
    ShaderAsset a, b;
    ASSERT_TRUE(a.Parse(blob.data(), blob.size()));
    ASSERT_TRUE(b.Parse(blob.data(), blob.size()));
    EXPECT_NE(a.Revision(), 0u);
    EXPECT_NE(a.Revision(), b.Revision());
}

TEST(ShaderHooks, ADefinitionCountsAndNothingElse)
{
    using DekiRendering::DefinesFunction;
    EXPECT_TRUE(DefinesFunction("float4 Shade(Fragment f) { return f.base; }", "Shade"));
    EXPECT_TRUE(DefinesFunction("float4 Shade(Fragment f)\n{\n}", "Shade"));
    EXPECT_TRUE(DefinesFunction("float4 Effect(float2 uv) : SV_Target { return 0; }", "Effect"));
    EXPECT_FALSE(DefinesFunction("float4 Shade(Fragment f);", "Shade")) << "a declaration";
    EXPECT_FALSE(DefinesFunction("float4 x = Shade(f);", "Shade")) << "a call";
    EXPECT_FALSE(DefinesFunction("float4 MyShade(Fragment f) { }", "Shade")) << "another name";
    EXPECT_FALSE(DefinesFunction("float4 ShadeMore(Fragment f) { }", "Shade")) << "another name";
    EXPECT_FALSE(DefinesFunction("// float4 Shade(Fragment f) { }", "Shade")) << "a comment";
    EXPECT_FALSE(DefinesFunction("/* float4 Shade(Fragment f) { } */", "Shade")) << "a comment";
}

TEST(ShaderCompile, AFileWithNoHookOfAnyUseSaysWhichItCouldDefine)
{
    if (!HaveCompiler())
    {
        GTEST_SKIP() << "no HLSL compiler (Slang) beside the tests";
    }
    DekiRendering::ShaderCompileResult result;
    EXPECT_FALSE(DekiRendering::CompileShaderFile("float Helper() { return 1.0; }\n", "nothing.shader", result));
    EXPECT_NE(result.errors.find("nothing.shader"), std::string::npos) << result.errors;
    EXPECT_NE(result.errors.find("Effect()"), std::string::npos) << result.errors;
    EXPECT_TRUE(result.programs.empty());
}

TEST(ShaderCompile, AnEffectCompilesForBothDialects)
{
    if (!HaveCompiler())
    {
        GTEST_SKIP() << "no HLSL compiler (Slang) beside the tests";
    }
    DekiRendering::ShaderCompileResult result;
    ASSERT_TRUE(DekiRendering::CompileShaderFile("float4 Effect(float2 uv) { return Frame(uv) * uColor; }\n",
                                                 "tint.shader", result))
        << result.errors;
    ASSERT_EQ(result.programs.size(), 1u);
    const ShaderProgram& p = result.programs[0];
    EXPECT_EQ(p.use, "effect");
    EXPECT_EQ(p.es.vertex.rfind("#version 300 es", 0), 0u) << p.es.vertex;
    EXPECT_EQ(p.es.fragment.rfind("#version 300 es", 0), 0u) << p.es.fragment;
    EXPECT_EQ(p.gl.fragment.rfind("#version 330", 0), 0u) << p.gl.fragment;
    // The names the presenter looks up (ShaderCompiler.h).
    EXPECT_NE(p.es.fragment.find("uniform highp sampler2D uFrame;"), std::string::npos) << p.es.fragment;
    EXPECT_NE(p.es.fragment.find("u.uColor"), std::string::npos) << p.es.fragment;
    EXPECT_NE(p.es.fragment.find("fragColor"), std::string::npos) << p.es.fragment;
    EXPECT_EQ(p.es.fragment.find("#extension"), std::string::npos) << p.es.fragment;
}

TEST(ShaderCompile, AnErrorNamesTheFileAndTheLine)
{
    if (!HaveCompiler())
    {
        GTEST_SKIP() << "no HLSL compiler (Slang) beside the tests";
    }
    DekiRendering::ShaderCompileResult result;
    EXPECT_FALSE(DekiRendering::CompileShaderFile("// a comment\n\nfloat4 Effect(float2 uv)\n{\n    return oops;\n}\n",
                                                  "shaders/typo.shader", result));
    EXPECT_NE(result.errors.find("shaders/typo.shader:5:"), std::string::npos) << result.errors;
    EXPECT_NE(result.errors.find("oops"), std::string::npos) << result.errors;
}

TEST(ShaderCompile, SomethingOpenGLES3CannotRunIsAnError)
{
    if (!HaveCompiler())
    {
        GTEST_SKIP() << "no HLSL compiler (Slang) beside the tests";
    }
    // A wave operation: Shader Model 6, with no GLSL ES 3.00 equivalent.
    DekiRendering::ShaderCompileResult result;
    EXPECT_FALSE(DekiRendering::CompileShaderFile(
        "float4 Effect(float2 uv) { return Frame(uv) * WaveActiveSum(uv.x); }\n", "wave.shader", result));
    EXPECT_NE(result.errors.find("wave.shader"), std::string::npos) << result.errors;
}

TEST(ShaderCompile, AGlobalThatIsSecretlyAUniformIsAWarning)
{
    if (!HaveCompiler())
    {
        GTEST_SKIP() << "no HLSL compiler (Slang) beside the tests";
    }
    // Without `static` it is a uniform, as in HLSL, and nothing sets it.
    DekiRendering::ShaderCompileResult result;
    ASSERT_TRUE(DekiRendering::CompileShaderFile(
        "float kScale = 0.5;\nfloat4 Effect(float2 uv) { return Frame(uv) * kScale; }\n", "scale.shader", result))
        << result.errors;
    EXPECT_NE(result.warnings.find("kScale"), std::string::npos) << result.warnings;
    EXPECT_NE(result.warnings.find("scale.shader:1:"), std::string::npos) << result.warnings;
}
