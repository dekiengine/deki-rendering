// Editor-only translation unit (a firmware build compiles nothing from it).
#ifdef DEKI_EDITOR

// The screen effect's shader use (ShaderUses.h): a full-screen shader the
// GL presenter shows the frame through (GLPresenter::Effect). The file's
// Effect(uv) gives each window pixel the frame covers its colour, with
// Frame(uv) to read the frame; uv runs 0 to 1 over the frame, bottom left to
// top right.

#include "ShaderUses.h"

namespace DekiRendering
{
namespace
{

const char* const kEffectHead = R"(
Texture2D uFrame;
SamplerState uFrameSampler;
float4 uDst;         // where the frame lands in the window: x, y, width, height
float2 uResolution;  // the frame's own size, in screen pixels
float uTime;         // seconds
float3 uParams;      // the ScreenEffect's parameters
float4 uColor;       // the ScreenEffect's colour

float4 Frame(float2 uv)
{
    return uFrame.Sample(uFrameSampler, uv);
}
)";

const char* const kEffectTail = R"(
// One triangle over the whole target, from the vertex id alone: no buffers.
float4 VertexMain(uint id : SV_VertexID) : SV_Position
{
    float2 p = float2(float((id & 1u) << 2) - 1.0, float((id & 2u) << 1) - 1.0);
    return float4(p, 0.0, 1.0);
}

float4 FragmentMain(float4 position : SV_Position) : SV_Target0
{
    float2 uv = (position.xy - uDst.xy) / uDst.zw;
#ifdef DEKI_EFFECT
    return float4(Effect(uv).rgb, 1.0);
#else
    return Frame(uv);
#endif
}
)";

struct EffectShaderUseRegistrar
{
    EffectShaderUseRegistrar()
    {
        ShaderUse use;
        use.name = "effect";
        use.hooks = { { "Effect", "DEKI_EFFECT", true } };
        use.head = kEffectHead;
        use.tail = kEffectTail;
        RegisterShaderUse(use);
    }
};
EffectShaderUseRegistrar s_EffectShaderUseRegistrar;

}  // namespace
}  // namespace DekiRendering

#endif  // DEKI_EDITOR
