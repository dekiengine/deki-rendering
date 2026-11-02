#pragma once

// A GPU shader's source, as a project asset (.shader).
//
// The text is GLSL ES 3.00, the language of OpenGL ES 3.0 and, with the
// version line changed, desktop OpenGL 3.3. A file has up to three parts:
//
//   (anything before the first marker)  shared: helpers, constants
//   // @vertex                          what the vertex stage adds
//   // @fragment                        what the fragment stage adds
//
// What each part must define is up to what draws with it: deki-3d-gl's
// meshes call Shade() and, if the file has one, Displace(); a screen effect
// calls Effect(). The backend wraps the parts in its own shaders, so a file
// holds only the interesting part.
//
// Loaded as text everywhere, compiled only by a GPU backend when it first
// draws with it. A board without a GPU loads it and draws without it.

#include "DekiRenderingPackage.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace DekiRendering
{

class DEKI_RENDERING_API ShaderAsset
{
public:
    static constexpr const char* kAssetTypeName = "Shader";

    /// Splits `text` at the stage markers. False when it is empty.
    bool Parse(const char* text, size_t size);

    const std::string& Common() const { return m_Common; }
    const std::string& Vertex() const { return m_Vertex; }
    const std::string& Fragment() const { return m_Fragment; }
    /// The file's line each part starts on, counting from 1, so a backend can
    /// tell the compiler (#line) and its errors name the file's lines.
    int VertexLine() const { return m_VertexLine; }
    int FragmentLine() const { return m_FragmentLine; }

    /// Never zero, and new every time a source loads, so a backend can keep
    /// one compiled program per revision.
    uint32_t Revision() const { return m_Revision; }

private:
    std::string m_Common;
    std::string m_Vertex;
    std::string m_Fragment;
    int m_VertexLine = 1;
    int m_FragmentLine = 1;
    uint32_t m_Revision = 0;
};

}  // namespace DekiRendering

/// Registers the Shader loader with the asset manager. Called from
/// DekiRenderingInitSystem so a static link keeps it; a DLL registers it from
/// a static initialiser as well.
void DekiRenderingRegisterShaderLoader();
