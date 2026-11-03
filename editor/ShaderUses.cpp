// Editor-only translation unit (a firmware build compiles nothing from it).
#ifdef DEKI_EDITOR

#include "ShaderUses.h"

#include <deki-editor/ShaderCompiler.h>

#include <cctype>
#include <mutex>

namespace DekiRendering
{
namespace
{

std::mutex& UsesMutex()
{
    static std::mutex s_Mutex;
    return s_Mutex;
}

std::vector<ShaderUse>& Uses()
{
    static std::vector<ShaderUse> s_Uses;
    return s_Uses;
}

/// `source` with its comments as spaces, so positions and lines stay.
std::string WithoutComments(const std::string& source)
{
    std::string out = source;
    for (size_t i = 0; i + 1 < out.size(); ++i)
    {
        if (out[i] == '/' && out[i + 1] == '/')
        {
            for (; i < out.size() && out[i] != '\n'; ++i)
            {
                out[i] = ' ';
            }
        }
        else if (out[i] == '/' && out[i + 1] == '*')
        {
            for (; i < out.size() && !(out[i] == '*' && i + 1 < out.size() && out[i + 1] == '/'); ++i)
            {
                if (out[i] != '\n')
                {
                    out[i] = ' ';
                }
            }
            if (i + 1 < out.size())
            {
                out[i] = out[i + 1] = ' ';
                ++i;
            }
        }
    }
    return out;
}

bool IsIdentifier(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

/// The HLSL for `use` around the file, with #line so the compiler counts the file's
/// own lines in its messages.
std::string Wrap(const ShaderUse& use, const std::string& source, const std::string& fileName)
{
    std::string name = fileName;
    for (char& c : name)
    {
        if (c == '"' || c == '\\')
        {
            c = '/';
        }
    }
    return "#line 1 \"" + use.name + " shader head (Deki)\"\n" + use.head + "\n#line 1 \"" + name + "\"\n" + source +
           "\n#line 1 \"" + use.name + " shader entry points (Deki)\"\n" + use.tail;
}

}  // namespace

void RegisterShaderUse(const ShaderUse& use)
{
    std::lock_guard<std::mutex> lock(UsesMutex());
    for (ShaderUse& existing : Uses())
    {
        if (existing.name == use.name)
        {
            existing = use;
            return;
        }
    }
    Uses().push_back(use);
}

std::vector<ShaderUse> ShaderUses()
{
    std::lock_guard<std::mutex> lock(UsesMutex());
    return Uses();
}

bool DefinesFunction(const std::string& source, const std::string& name)
{
    const std::string text = WithoutComments(source);
    for (size_t at = text.find(name); at != std::string::npos; at = text.find(name, at + 1))
    {
        if ((at > 0 && IsIdentifier(text[at - 1])) ||
            (at + name.size() < text.size() && IsIdentifier(text[at + name.size()])))
        {
            continue;
        }
        size_t i = at + name.size();
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
        {
            ++i;
        }
        if (i >= text.size() || text[i] != '(')
        {
            continue;
        }
        // The parameter list, then a body (after a return semantic, if any)
        // makes it a definition; a ';' first makes it a declaration or a call.
        int depth = 0;
        for (; i < text.size(); ++i)
        {
            depth += text[i] == '(' ? 1 : text[i] == ')' ? -1 : 0;
            if (depth == 0)
            {
                break;
            }
        }
        for (++i; i < text.size(); ++i)
        {
            if (text[i] == '{')
            {
                return true;
            }
            if (text[i] == ';' || text[i] == '(' || text[i] == ')' || text[i] == '}')
            {
                break;
            }
        }
    }
    return false;
}

bool CompileShaderUse(const ShaderUse& use, const std::string& source, const std::string& fileName,
                      const std::vector<std::string>& hooks, ShaderProgram& program, std::string& errors,
                      std::string* warnings)
{
    DekiEditor::ShaderCompileRequest request;
    request.source = Wrap(use, source, fileName);
    request.fileName = fileName;
    for (const ShaderHook& hook : use.hooks)
    {
        for (const std::string& defined : hooks)
        {
            if (defined == hook.name)
            {
                request.defines.push_back(hook.define);
            }
        }
    }

    program = ShaderProgram{};
    program.use = use.name;
    DekiEditor::ShaderStageText vertex, fragment;
    request.stage = DekiEditor::ShaderStage::Vertex;
    request.entry = use.vertexEntry;
    std::string vertexWarnings, fragmentWarnings;
    if (!DekiEditor::ShaderCompiler::Compile(request, vertex, errors, &vertexWarnings))
    {
        return false;
    }
    request.stage = DekiEditor::ShaderStage::Fragment;
    request.entry = use.fragmentEntry;
    if (!DekiEditor::ShaderCompiler::Compile(request, fragment, errors, &fragmentWarnings))
    {
        return false;
    }
    if (warnings)
    {
        // Both stages read the same file, so mostly say the same.
        *warnings = vertexWarnings;
        if (!fragmentWarnings.empty() && fragmentWarnings != vertexWarnings)
        {
            *warnings += (warnings->empty() ? "" : "\n") + fragmentWarnings;
        }
    }
    program.es = { vertex.es, fragment.es };
    program.gl = { vertex.gl, fragment.gl };
    return true;
}

bool CompileShaderFile(const std::string& source, const std::string& fileName, ShaderCompileResult& result)
{
    result = ShaderCompileResult{};
    const std::vector<ShaderUse> uses = ShaderUses();
    std::string every;
    for (const ShaderUse& use : uses)
    {
        std::vector<std::string> defined;
        std::string missing;
        for (const ShaderHook& hook : use.hooks)
        {
            every += (every.empty() ? "" : ", ") + hook.name + "()";
            if (DefinesFunction(source, hook.name))
            {
                defined.push_back(hook.name);
                result.hooks.push_back(hook.name);
            }
            else if (hook.required)
            {
                missing += (missing.empty() ? "" : ", ") + hook.name + "()";
            }
        }
        if (defined.empty())
        {
            continue;
        }
        if (!missing.empty())
        {
            result.errors += fileName + ": defines " + defined.front() + "() for a " + use.name +
                             " shader, which needs " + missing + " too\n";
            continue;
        }
        ShaderProgram program;
        std::string errors, warnings;
        if (!CompileShaderUse(use, source, fileName, defined, program, errors, &warnings))
        {
            result.errors += errors + "\n";
            continue;
        }
        if (!warnings.empty() && result.warnings.find(warnings) == std::string::npos)
        {
            result.warnings += (result.warnings.empty() ? "" : "\n") + warnings;
        }
        result.programs.push_back(std::move(program));
    }
    if (result.errors.empty() && result.programs.empty())
    {
        result.errors = fileName + ": defines none of the functions a shader is drawn with (" +
                        (every.empty() ? std::string("no package that draws with shaders is loaded") : every) + ")";
    }
    while (!result.errors.empty() && result.errors.back() == '\n')
    {
        result.errors.pop_back();
    }
    if (!result.errors.empty())
    {
        result.programs.clear();
        return false;
    }
    return true;
}

bool CompileShaderText(const std::string& source, const std::string& fileName, ShaderAsset& asset,
                       std::string& errors)
{
    ShaderCompileResult result;
    if (!CompileShaderFile(source, fileName, result))
    {
        errors = result.errors;
        return false;
    }
    const std::vector<uint8_t> blob = ShaderAsset::Encode(result.programs);
    if (!asset.Parse(blob.data(), blob.size()))
    {
        errors = fileName + ": the compiled shader does not load";
        return false;
    }
    return true;
}

}  // namespace DekiRendering

#endif  // DEKI_EDITOR
