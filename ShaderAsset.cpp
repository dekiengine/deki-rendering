#include "ShaderAsset.h"

#include <deki/assets/AssetManager.h>
#include <deki/providers/Memory.h>

#include <atomic>
#include <cstring>

namespace DekiRendering
{
namespace
{

std::atomic<uint32_t> s_Revision{ 0 };

// The compiled file: "DKSH", a version byte, a program count (u16), then per
// program its use and the four stage texts (ES vertex, ES fragment, GL
// vertex, GL fragment), each a u32 length and its bytes. Little endian.
constexpr uint8_t kMagic[4] = { 'D', 'K', 'S', 'H' };
constexpr uint8_t kVersion = 1;

void PutU32(std::vector<uint8_t>& out, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
    {
        out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
}

void PutText(std::vector<uint8_t>& out, const std::string& text)
{
    PutU32(out, static_cast<uint32_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
}

/// Reads through a blob; any read past its end leaves it failed.
struct Reader
{
    const uint8_t* at;
    const uint8_t* end;
    bool ok = true;

    bool Take(size_t n)
    {
        ok = ok && static_cast<size_t>(end - at) >= n;
        return ok;
    }
    uint32_t U32()
    {
        if (!Take(4))
        {
            return 0;
        }
        const uint32_t v = at[0] | (at[1] << 8) | (at[2] << 16) | (static_cast<uint32_t>(at[3]) << 24);
        at += 4;
        return v;
    }
    std::string Text()
    {
        const uint32_t size = U32();
        if (!Take(size))
        {
            return {};
        }
        std::string text(reinterpret_cast<const char*>(at), size);
        at += size;
        return text;
    }
};

}  // namespace

std::vector<uint8_t> ShaderAsset::Encode(const std::vector<ShaderProgram>& programs)
{
    std::vector<uint8_t> out(kMagic, kMagic + 4);
    out.push_back(kVersion);
    out.push_back(static_cast<uint8_t>(programs.size()));
    out.push_back(static_cast<uint8_t>(programs.size() >> 8));
    for (const ShaderProgram& program : programs)
    {
        PutText(out, program.use);
        PutText(out, program.es.vertex);
        PutText(out, program.es.fragment);
        PutText(out, program.gl.vertex);
        PutText(out, program.gl.fragment);
    }
    return out;
}

bool ShaderAsset::Parse(const uint8_t* data, size_t size)
{
    m_Programs.clear();
    if (!data || size < 7 || std::memcmp(data, kMagic, 4) != 0 || data[4] != kVersion)
    {
        return false;
    }
#ifndef DEKI_CAPABILITY_OPENGL
    // Nothing on this platform compiles a shader, so none of it is kept. A
    // build for such a board refuses a shader a scene uses; this is for one
    // loaded some other way.
    m_Revision = 1;
    return true;
#else
    Reader in{ data + 7, data + size };
    const uint16_t count = static_cast<uint16_t>(data[5] | (data[6] << 8));
    for (uint16_t i = 0; i < count && in.ok; ++i)
    {
        ShaderProgram program;
        program.use = in.Text();
        program.es.vertex = in.Text();
        program.es.fragment = in.Text();
        program.gl.vertex = in.Text();
        program.gl.fragment = in.Text();
        m_Programs.push_back(std::move(program));
    }
    if (!in.ok)
    {
        m_Programs.clear();
        return false;
    }

    m_Revision = s_Revision.fetch_add(1) + 1;
    if (m_Revision == 0)
    {
        m_Revision = s_Revision.fetch_add(1) + 1;
    }
    return true;
#endif
}

const ShaderProgramSource* ShaderAsset::Program(const char* use, bool es) const
{
    for (const ShaderProgram& program : m_Programs)
    {
        if (program.use == use)
        {
            return es ? &program.es : &program.gl;
        }
    }
    return nullptr;
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
        if (shader->Parse(bytes.Data(), bytes.Count()))
        {
            return shader;
        }
        delete shader;
        return nullptr;
    };
    auto memLoader = [](const uint8_t* data, size_t size) -> void*
    {
        auto* shader = new ShaderAsset();
        if (shader->Parse(data, size))
        {
            return shader;
        }
        delete shader;
        return nullptr;
    };
    auto unloader = [](void* asset) { delete static_cast<ShaderAsset*>(asset); };
    Deki::AssetManager::RegisterLoader(ShaderAsset::kAssetTypeName, loader, unloader, memLoader);
    // A component's AssetRef<ShaderAsset> is resolved by its class name (the
    // reflection tables name the type the field holds), as deki-2d's
    // FrameAnimationData is.
    Deki::AssetManager::RegisterLoader("ShaderAsset", loader, unloader, memLoader);
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
