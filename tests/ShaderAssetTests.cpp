// Tests that a .shader file splits into its shared, vertex and fragment
// parts at the markers, whatever order they come in, and that every load is
// a new revision.

#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "../ShaderAsset.h"

using DekiRendering::ShaderAsset;

namespace
{
ShaderAsset Parse(const std::string& text)
{
    ShaderAsset shader;
    EXPECT_TRUE(shader.Parse(text.data(), text.size()));
    return shader;
}
}  // namespace

TEST(ShaderAsset, SplitsAtTheMarkers)
{
    const ShaderAsset s = Parse("float shared;\n// @vertex\nvec3 Displace();\n// @fragment\nvec4 Shade();\n");
    EXPECT_EQ(s.Common(), "float shared;\n");
    EXPECT_EQ(s.Vertex(), "// @vertex\nvec3 Displace();\n");
    EXPECT_EQ(s.Fragment(), "// @fragment\nvec4 Shade();\n");
}

TEST(ShaderAsset, TheStagesMayComeInEitherOrder)
{
    const ShaderAsset s = Parse("// @fragment\nvec4 Shade();\n// @vertex\nvec3 Displace();\n");
    EXPECT_EQ(s.Common(), "");
    EXPECT_EQ(s.Fragment(), "// @fragment\nvec4 Shade();\n");
    EXPECT_EQ(s.Vertex(), "// @vertex\nvec3 Displace();\n");
}

TEST(ShaderAsset, AMarkerCountsOnlyAtTheStartOfALine)
{
    const ShaderAsset s = Parse("float x; // @vertex is not a marker here\n");
    EXPECT_EQ(s.Vertex(), "");
    EXPECT_EQ(s.Common(), "float x; // @vertex is not a marker here\n");
}

TEST(ShaderAsset, EveryLoadIsANewRevision)
{
    const ShaderAsset a = Parse("// @fragment\n");
    const ShaderAsset b = Parse("// @fragment\n");
    EXPECT_NE(a.Revision(), 0u);
    EXPECT_NE(a.Revision(), b.Revision());
}

TEST(ShaderAsset, AnEmptyFileIsNotAShader)
{
    ShaderAsset s;
    EXPECT_FALSE(s.Parse("", 0));
}

TEST(ShaderAsset, KnowsTheLineEachPartStartsOn)
{
    const ShaderAsset s = Parse("float a;\nfloat b;\n// @vertex\nvec3 D();\n// @fragment\nvec4 S();\n");
    EXPECT_EQ(s.VertexLine(), 3);
    EXPECT_EQ(s.FragmentLine(), 5);
}
