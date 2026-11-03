// The GPU's 2D light: one triangle over the frame, each pixel's light worked
// out from the frame's Lights exactly as Lighting2D does per block (ambient,
// then each point and spot light by how near it is, in rings when it has
// bands), at the pixel itself. Its colour is half the light, and the frame is
// blended with it as DST_COLOR, SRC_COLOR, which comes to the frame times the
// light, up to twice the colour; its alpha, a half, leaves the frame's alpha
// as it was.

#ifdef DEKI_EDITOR

#include "Light2DShader.h"

namespace DekiRendering
{

const char* Light2DShaderHlsl()
{
    return R"hlsl(
// Each light: x, y (view pixels, top down), radius, bands; then red, green,
// blue (times its intensity) and 1 for a spot; then a spot's facing and the
// cosines of its cone's edge and of where it starts to fade.
uniform float4 uLightA[16];
uniform float4 uLightB[16];
uniform float4 uLightC[16];
uniform int uCount;
uniform float3 uAmbient;
uniform float uHeight;

float4 VertexMain(uint id : SV_VulkanVertexID) : SV_Position
{
    const float2 p = float2(float((id & 1) << 2) - 1.0, float((id & 2) << 1) - 1.0);
    return float4(p, 0.0, 1.0);
}

float4 FragmentMain(float4 position : SV_Position) : SV_Target
{
    // The pixel's centre in view pixels: GL counts rows from the bottom.
    const float2 at = float2(position.x, uHeight - position.y);
    float3 light = uAmbient;
    for (int i = 0; i < uCount; i++)
    {
        const float4 a = uLightA[i];
        const float4 b = uLightB[i];
        const float2 d = at - a.xy;
        const float d2 = dot(d, d);
        float t = 1.0 - d2 / (a.z * a.z);
        if (t <= 0.0)
        {
            continue;
        }
        float f = a.w > 0.0 ? ceil(t * a.w) / a.w : t * t;
        if (b.w > 0.0 && d2 > 0.0)
        {
            const float4 c = uLightC[i];
            const float cosine = dot(d, c.xy) / sqrt(d2);
            const float s = saturate((cosine - c.z) / (c.w - c.z));
            f *= s * s * (3.0 - 2.0 * s);
        }
        light += b.rgb * f;
    }
    return float4(min(light, 2.0) * 0.5, 0.5);
}
)hlsl";
}

}  // namespace DekiRendering

#endif  // DEKI_EDITOR
