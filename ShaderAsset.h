#pragma once

// A GPU shader, as a project asset (.shader).
//
// The file is HLSL. It defines the hooks of what draws with it: deki-3d's
// GPU meshes call Shade() and, if the file has one, Displace(); a screen
// effect calls Effect(). What draws with it wraps the hooks in its own
// shader, so a file holds only the interesting part.
//
// Devices run GLSL, so the editor compiles the file (deki-editor's
// ShaderCompiler) once for each use whose hooks it defines: "mesh", "effect".
// This asset is that result, the GLSL of each program in the two dialects a
// device may have. A board without OpenGL (no DEKI_CAPABILITY_OPENGL) cannot
// use a shader at all: a build for one that needs a shader stops and says so.

#include "DekiRenderingPackage.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace DekiRendering
{

/// One program's stages in one dialect.
struct ShaderProgramSource
{
    std::string vertex;
    std::string fragment;
};

/// One program: what it is for, and its text for OpenGL ES 3.0 and desktop
/// OpenGL 3.3.
struct ShaderProgram
{
    std::string use;  // "mesh", "effect"
    ShaderProgramSource es;
    ShaderProgramSource gl;
};

class DEKI_RENDERING_API ShaderAsset
{
public:
    static constexpr const char* kAssetTypeName = "Shader";

    /// Reads what Encode() wrote. False when it is not that.
    bool Parse(const uint8_t* data, size_t size);

    /// The compiled form of `programs`, as the editor stores a .shader.
    static std::vector<uint8_t> Encode(const std::vector<ShaderProgram>& programs);

    /// The program for `use` in OpenGL ES (`es`) or desktop GL text, or null
    /// when the file does not define that use's hooks.
    const ShaderProgramSource* Program(const char* use, bool es) const;

    const std::vector<ShaderProgram>& Programs() const { return m_Programs; }

    /// Never zero, and new every time a shader loads, so a backend can keep
    /// one compiled program per revision.
    uint32_t Revision() const { return m_Revision; }

private:
    std::vector<ShaderProgram> m_Programs;
    uint32_t m_Revision = 0;
};

}  // namespace DekiRendering

/// Registers the Shader loader with the asset manager. Called from
/// DekiRenderingInitSystem so a static link keeps it; a DLL registers it from
/// a static initialiser as well.
void DekiRenderingRegisterShaderLoader();
