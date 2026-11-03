#pragma once

// Only where the platform has OpenGL: the build defines DEKI_CAPABILITY_OPENGL
// for one whose platform provides "opengl" (the desktop, a Linux handheld) and
// never for a microcontroller, where this file compiles to nothing.
#ifdef DEKI_CAPABILITY_OPENGL

#include <deki/Color.h>
#include <deki/Component.h>
#include <deki/Vector.h>
#include <deki/assets/AssetRef.h>
#include <deki/reflection/Property.h>

#include "DekiRenderingPackage.h"
#include "deki-rendering/ShaderAsset.h"

namespace DekiGL
{

/// A full-screen shader over the finished frame, where the display presents
/// through OpenGL (GLPresenter): scanlines, a vignette, colour grading, a
/// wobble. The frame draws into the window through the shader's Effect()
/// instead of being copied. Put one anywhere in the scene; the first active
/// one is used. Without a GPU presenter (a microcontroller, the editor) the
/// frame shows as it is.
DEKI_CATEGORY("Rendering")
DEKI_DESCRIPTION("A full-screen shader over the finished frame, where the display presents through OpenGL.")
class DEKI_RENDERING_API ScreenEffect : public Deki::Component
{
public:
    ScreenEffect();
    ~ScreenEffect() override;

    DEKI_TOOLTIP("An HLSL .shader that defines float4 Effect(float2 uv): the colour of each window pixel, where "
                 "uv runs 0 to 1 over the frame, bottom left to top right. Frame(uv) reads the frame.")
    DEKI_EXPORT
    Deki::AssetRef<DekiRendering::ShaderAsset> shader;

    DEKI_TOOLTIP("Three numbers the shader reads as uParams: strengths, speeds, whatever it says it wants.")
    DEKI_EXPORT
    Deki::Vector3 params;

    DEKI_TOOLTIP("A colour the shader reads as uColor.")
    DEKI_EXPORT
    Deki::Color color;

    /// The effect to draw this frame: the first whose object is active and
    /// whose shader has loaded, or null.
    static const ScreenEffect* Current();
};

}  // namespace DekiGL

#endif  // DEKI_CAPABILITY_OPENGL
