#pragma once

// The GPU's 2D light (editor only): see Light2DShader.cpp.

#ifdef DEKI_EDITOR

namespace DekiRendering
{

/// The HLSL of the full-screen pass GLPresenter lights a 2D frame with. The
/// editor's compiler turns it into GLLight2DShader.inl, which the runtime
/// builds; tests/GLLightingTests.cpp keeps the two the same.
const char* Light2DShaderHlsl();

}  // namespace DekiRendering

#endif  // DEKI_EDITOR
