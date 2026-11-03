#pragma once

// What a .shader can be used for, and compiling one for each (editor only).
//
// A use is what draws with a shader: deki-3d's GPU meshes ("mesh"), this
// package's screen effect ("effect"). It names the hooks it calls and wraps
// a file in its own HLSL: a head before the file (its uniforms, its types,
// helpers like Frame()) and a tail after it (the entry points, which call the
// hooks the file defines, each behind a define the compiler sets).
//
// A file is compiled once for every use with at least one of its hooks
// defined, in both stages, and the asset keeps each result
// (DekiRendering::ShaderAsset). Every problem is an error naming the file and
// line: one that defines no hook of any use, one that misses a hook a use
// cannot draw without, one that does not compile.
//
// The package that draws with a use registers it, from a static initialiser
// in its editor code, so this package does not need to know deki-3d.

#ifdef DEKI_EDITOR

#include "../DekiRenderingPackage.h"
#include "../ShaderAsset.h"

#include <string>
#include <vector>

namespace DekiRendering
{

struct ShaderHook
{
    std::string name;    // the function the file defines, e.g. "Shade"
    std::string define;  // what the tail tests to call it, e.g. "DEKI_SHADE"
    bool required = false;  // a file with this use's other hooks must have it too
};

struct ShaderUse
{
    std::string name;  // what the runtime asks the asset for: "mesh", "effect"
    std::vector<ShaderHook> hooks;
    std::string head;  // HLSL before the file
    std::string tail;  // HLSL after it: the entry points
    std::string vertexEntry = "VertexMain";
    std::string fragmentEntry = "FragmentMain";
};

/// Adds `use`, replacing one of the same name.
DEKI_RENDERING_API void RegisterShaderUse(const ShaderUse& use);

/// Every registered use, in registration order.
DEKI_RENDERING_API std::vector<ShaderUse> ShaderUses();

/// Whether HLSL `source` defines a function `name` (a declaration is not a
/// definition; comments are skipped).
DEKI_RENDERING_API bool DefinesFunction(const std::string& source, const std::string& name);

/// Compiles `source` (the file `fileName`'s text) for `use`, calling the
/// hooks in `hooks` (names from use.hooks). A use with no hooks is its own
/// built-in program.
DEKI_RENDERING_API bool CompileShaderUse(const ShaderUse& use, const std::string& source, const std::string& fileName,
                                         const std::vector<std::string>& hooks, ShaderProgram& program,
                                         std::string& errors, std::string* warnings = nullptr);

struct ShaderCompileResult
{
    std::vector<ShaderProgram> programs;
    std::vector<std::string> hooks;  // the hooks the file defines, of every use
    std::string errors;              // empty when it compiled
    std::string warnings;            // what the compiler warned about, when it compiled
};

/// Compiles a .shader for every use it defines hooks of. False with
/// `result.errors` when anything went wrong; then `programs` is empty.
DEKI_RENDERING_API bool CompileShaderFile(const std::string& source, const std::string& fileName,
                                          ShaderCompileResult& result);

/// CompileShaderFile, then loaded into `asset` as a build would ship it.
DEKI_RENDERING_API bool CompileShaderText(const std::string& source, const std::string& fileName, ShaderAsset& asset,
                                          std::string& errors);

}  // namespace DekiRendering

#endif  // DEKI_EDITOR
