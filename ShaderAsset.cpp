#include "ShaderAsset.h"

#include <deki/assets/AssetManager.h>
#include <deki/providers/Memory.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace DekiRendering
{
namespace
{

std::atomic<uint32_t> s_Revision{ 0 };

/// Where `marker` starts a line of `text`, at or after `from`, or npos.
size_t FindMarker(const std::string& text, const char* marker, size_t from = 0)
{
    for (size_t at = text.find(marker, from); at != std::string::npos; at = text.find(marker, at + 1))
    {
        if (at == 0 || text[at - 1] == '\n')
        {
            return at;
        }
    }
    return std::string::npos;
}

}  // namespace

bool ShaderAsset::Parse(const char* text, size_t size)
{
    if (!text || size == 0)
    {
        return false;
    }
    m_Common.clear();
    m_Vertex.clear();
    m_Fragment.clear();
#ifndef DEKI_CAPABILITY_OPENGL
    // Nothing on this platform compiles a shader, so its source is not kept:
    // a mesh that names one costs a board this object, not the text.
    m_Revision = 1;
    return true;
#else
    const std::string all(text, size);

    // Each part runs to the next marker or the end. A part keeps its marker
    // line, a comment, so a compiler's line numbers count from the file.
    const size_t vertexAt = FindMarker(all, "// @vertex");
    const size_t fragmentAt = FindMarker(all, "// @fragment");
    const size_t firstMarker = std::min(vertexAt, fragmentAt);
    m_Common = all.substr(0, firstMarker == std::string::npos ? all.size() : firstMarker);
    if (vertexAt != std::string::npos)
    {
        const size_t end = fragmentAt > vertexAt ? fragmentAt : std::string::npos;
        m_Vertex = all.substr(vertexAt, end == std::string::npos ? std::string::npos : end - vertexAt);
    }
    if (fragmentAt != std::string::npos)
    {
        const size_t end = vertexAt > fragmentAt ? vertexAt : std::string::npos;
        m_Fragment = all.substr(fragmentAt, end == std::string::npos ? std::string::npos : end - fragmentAt);
    }

    auto lineOf = [&](size_t at)
    { return at == std::string::npos ? 1 : 1 + static_cast<int>(std::count(all.begin(), all.begin() + at, '\n')); };
    m_VertexLine = lineOf(vertexAt);
    m_FragmentLine = lineOf(fragmentAt);

    m_Revision = s_Revision.fetch_add(1) + 1;
    if (m_Revision == 0)
    {
        m_Revision = s_Revision.fetch_add(1) + 1;
    }
    return true;
#endif
}

}  // namespace DekiRendering

void DekiRenderingRegisterShaderLoader()
{
    static bool s_Registered = false;
    if (s_Registered)
    {
        return;
    }
    s_Registered = true;

    using DekiRendering::ShaderAsset;
    auto loader = [](const char* path) -> void*
    {
        Deki::Buffer<uint8_t> bytes;
        if (!Deki::AssetManager::ReadWholeFile(path, bytes, Deki::Memory::External))
        {
            return nullptr;
        }
        auto* shader = new ShaderAsset();
        if (shader->Parse(reinterpret_cast<const char*>(bytes.Data()), bytes.Count()))
        {
            return shader;
        }
        delete shader;
        return nullptr;
    };
    auto memLoader = [](const uint8_t* data, size_t size) -> void*
    {
        auto* shader = new ShaderAsset();
        if (shader->Parse(reinterpret_cast<const char*>(data), size))
        {
            return shader;
        }
        delete shader;
        return nullptr;
    };
    auto unloader = [](void* asset) { delete static_cast<ShaderAsset*>(asset); };
    Deki::AssetManager::RegisterLoader(ShaderAsset::kAssetTypeName, loader, unloader, memLoader);
}

namespace
{
// A static registrar for the DLL; DekiRenderingInitSystem calls the same for
// a static link, which drops a file nothing references.
struct ShaderLoaderRegistrar
{
    ShaderLoaderRegistrar() { DekiRenderingRegisterShaderLoader(); }
};
ShaderLoaderRegistrar s_ShaderLoaderRegistrar;
}  // namespace
