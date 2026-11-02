// Only where the platform has OpenGL: the build defines DEKI_CAPABILITY_OPENGL
// for one whose platform provides "opengl" (the desktop, a Linux handheld) and
// never for a microcontroller, where this file compiles to nothing.
#ifdef DEKI_CAPABILITY_OPENGL

#include "GLBlitSink.h"

#include "deki-rendering/PixelFormat.h"

#include <deki/LogSystem.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

namespace DekiGL
{
namespace
{

// Quads per batch: four corners each, indexed in 16 bits.
constexpr uint32_t kMaxQuads = 16384;

// Textures not drawn for this many frames go (two seconds at 60 fps), and
// past this many bytes the ones drawn longest ago go first.
constexpr uint32_t kIdleFrames = 120;
constexpr size_t kTextureBudget = 64u * 1024u * 1024u;

// Vertex::key[3].
enum : uint8_t
{
    kFlagFlipH = 1,
    kFlagFlipV = 2,
    kFlagFlipD = 4,  // only for a square source, as the CPU transposes only those
    kFlagRepeatColumn = 8,
    kFlagRepeatRow = 16,
    kFlagKey = 32,
    kFlagDither = 64,
};

const char* const kQuadVertexShader = R"(
layout(location = 0) in ivec2 aCorner;
layout(location = 1) in ivec4 aOrigin;
layout(location = 2) in ivec4 aStep;
layout(location = 3) in ivec4 aSize;
layout(location = 4) in uvec4 aTint;
layout(location = 5) in uvec4 aKey;

uniform vec2 uFrame;  // the frame's size in pixels

flat out ivec4 vOrigin;
flat out ivec4 vStep;
flat out ivec4 vSize;
flat out uvec4 vTint;
flat out uvec4 vKey;

void main()
{
    // Corners on pixel edges, top-down like the CPU buffer.
    vec2 p = vec2(aCorner) / uFrame;
    gl_Position = vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
    vOrigin = aOrigin;
    vStep = aStep;
    vSize = aSize;
    vTint = aTint;
    vKey = aKey;
}
)";

// QuadBlit's pixel pipeline (CompositePixel) for one destination pixel, with
// the CPU's integer stepping and Div255. The blend itself is the GPU's,
// premultiplied: the frame keeps 8 bits a channel where the CPU's RGB565A8
// buffer rounds each write to 565. kBayer is filled in from PixelFormat.h.
const char* const kQuadFragmentHead = R"(
precision highp float;
precision highp int;

uniform highp sampler2D uTexture;
uniform int uHeight;  // the frame's, to count rows from the top

flat in ivec4 vOrigin;
flat in ivec4 vStep;
flat in ivec4 vSize;
flat in uvec4 vTint;
flat in uvec4 vKey;

out vec4 fragColor;
)";

const char* const kQuadFragmentMain = R"(
uint Div255(uint x)
{
    return (x + 1u + (x >> 8)) >> 8;
}

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    int px = p.x;
    int py = uHeight - 1 - p.y;

    // The 16.16 walk, in 32 bits that wrap, as the CPU steps it.
    uint dx = uint(px - vOrigin.x);
    uint dy = uint(py - vOrigin.y);
    int x = int(uint(vOrigin.z) + dx * uint(vStep.x) + dy * uint(vStep.z)) >> 16;
    int y = int(uint(vOrigin.w) + dx * uint(vStep.y) + dy * uint(vStep.w)) >> 16;
    if (x < 0 || y < 0 || x >= vSize.z || y >= vSize.w)
    {
        discard;
    }
    uint flags = vKey.w;
    if ((flags & 8u) != 0u)
    {
        x = 0;
    }
    if ((flags & 16u) != 0u)
    {
        y = 0;
    }
    // ApplyFlips' order: V, then H, then the transpose.
    if ((flags & 2u) != 0u)
    {
        y = vSize.y - 1 - y;
    }
    if ((flags & 1u) != 0u)
    {
        x = vSize.x - 1 - x;
    }
    if ((flags & 4u) != 0u)
    {
        int t = x;
        x = y;
        y = t;
    }

    uvec4 c = uvec4(texelFetch(uTexture, ivec2(x, y), 0) * 255.0 + 0.5);
    if (c.a == 0u)
    {
        discard;
    }
    // The key is compared against the untinted colour.
    if ((flags & 32u) != 0u && c.rgb == vKey.rgb)
    {
        discard;
    }
    uvec3 rgb = uvec3(Div255(c.r * vTint.r), Div255(c.g * vTint.g), Div255(c.b * vTint.b));
    uint a = Div255(c.a * vTint.a);
    if (a == 0u)
    {
        discard;
    }
    if ((flags & 64u) != 0u)
    {
        // 255 always passes, since the matrix tops out at 252.
        if (int(a) <= kBayer[((py & 7) << 3) | (px & 7)])
        {
            discard;
        }
        a = 255u;
    }
    float alpha = float(a) / 255.0;
    fragColor = vec4(vec3(rgb) / 255.0 * alpha, alpha);
}
)";

/// The engine's 8x8 Bayer matrix as a GLSL array, so the shader's threshold
/// is the CPU's.
std::string BayerDeclaration()
{
    std::string s = "const int kBayer[64] = int[64](";
    for (int i = 0; i < 64; ++i)
    {
        s += std::to_string(DekiPixel::kBayer8x8[i]);
        s += i < 63 ? ", " : ");\n";
    }
    return s;
}

/// One row of a source as RGBA8, each pixel read as QuadBlit reads it.
template <DekiPixel::SrcKind SK>
void SourceRowToRGBA(const uint8_t* row, int32_t width, int32_t bytesPerPixel, bool hasAlpha, uint8_t* out)
{
    for (int32_t x = 0; x < width; ++x, row += bytesPerPixel, out += 4)
    {
        DekiPixel::ReadSrcPixel<SK>(row, hasAlpha, out[0], out[1], out[2], out[3]);
    }
}

}  // namespace

size_t GLBlitSink::KeyHash::operator()(const Key& k) const
{
    size_t h = std::hash<const void*>()(k.pixels);
    auto mix = [&](size_t v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); };
    mix(static_cast<size_t>(k.width));
    mix(static_cast<size_t>(k.height));
    mix(static_cast<size_t>(k.stride));
    mix(static_cast<size_t>(k.bytesPerPixel) | (static_cast<size_t>(k.isRGB565) << 8) |
        (static_cast<size_t>(k.hasAlpha) << 9));
    return h;
}

GLBlitSink::GLBlitSink(GLPresenter& presenter)
    : m_Presenter(presenter)
{
}

bool GLBlitSink::Build()
{
    const GL& gl = m_Presenter.m_Context->Functions();
    const std::string version = m_Presenter.m_Context->IsES() ? "#version 300 es\n" : "#version 330 core\n";
    const GLuint vs = m_Presenter.Compile(gl::VERTEX_SHADER, version + kQuadVertexShader, "the 2D vertex shader");
    const GLuint fs = vs ? m_Presenter.Compile(gl::FRAGMENT_SHADER,
                                               version + kQuadFragmentHead + BayerDeclaration() + kQuadFragmentMain,
                                               "the 2D fragment shader")
                         : 0;
    if (!vs || !fs)
    {
        gl.DeleteShader(vs);
        return false;
    }
    m_Program = gl.CreateProgram();
    gl.AttachShader(m_Program, vs);
    gl.AttachShader(m_Program, fs);
    gl.LinkProgram(m_Program);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    GLint linked = 0;
    gl.GetProgramiv(m_Program, gl::LINK_STATUS, &linked);
    if (!linked)
    {
        DEKI_LOG_ERROR("OpenGL: linking the 2D shader failed");
        return false;
    }
    m_FrameUniform = gl.GetUniformLocation(m_Program, "uFrame");
    m_HeightUniform = gl.GetUniformLocation(m_Program, "uHeight");
    m_TextureUniform = gl.GetUniformLocation(m_Program, "uTexture");
    gl.GetIntegerv(gl::MAX_TEXTURE_SIZE, &m_MaxTextureSize);
    m_MaxTextureSize = std::min<GLint>(m_MaxTextureSize, 32767);  // Vertex::size is 16 bits

    // Every batch draws its quads as two triangles each, from one index list.
    constexpr uint16_t kCorners[6] = { 0, 1, 2, 2, 1, 3 };
    std::vector<uint16_t> indices(static_cast<size_t>(kMaxQuads) * 6);
    for (size_t i = 0; i < indices.size(); ++i)
    {
        indices[i] = static_cast<uint16_t>(i / 6 * 4 + kCorners[i % 6]);
    }
    gl.GenVertexArrays(1, &m_VertexArray);
    gl.GenBuffers(1, &m_VertexBuffer);
    gl.GenBuffers(1, &m_IndexBuffer);
    gl.BindVertexArray(m_VertexArray);
    gl.BindBuffer(gl::ELEMENT_ARRAY_BUFFER, m_IndexBuffer);
    gl.BufferData(gl::ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(uint16_t)), indices.data(),
                  gl::STATIC_DRAW);
    gl.BindBuffer(gl::ARRAY_BUFFER, m_VertexBuffer);
    auto attribute = [&](GLuint index, GLint size, GLenum type, size_t offset)
    {
        gl.EnableVertexAttribArray(index);
        gl.VertexAttribIPointer(index, size, type, sizeof(Vertex), reinterpret_cast<const void*>(offset));
    };
    attribute(0, 2, gl::SHORT, offsetof(Vertex, x));
    attribute(1, 4, gl::INT, offsetof(Vertex, origin));
    attribute(2, 4, gl::INT, offsetof(Vertex, step));
    attribute(3, 4, gl::SHORT, offsetof(Vertex, size));
    attribute(4, 4, gl::UNSIGNED_BYTE, offsetof(Vertex, tint));
    attribute(5, 4, gl::UNSIGNED_BYTE, offsetof(Vertex, key));
    gl.BindVertexArray(0);
    return gl.GetError() == 0;
}

void GLBlitSink::Release()
{
    const GL& gl = m_Presenter.m_Context->Functions();
    for (auto& entry : m_Textures)
    {
        gl.DeleteTextures(1, &entry.second.id);
    }
    m_Textures.clear();
    gl.DeleteTextures(1, &m_Scratch.id);
    m_Scratch = Texture{};
    m_TextureBytes = 0;
    gl.DeleteBuffers(1, &m_VertexBuffer);
    gl.DeleteBuffers(1, &m_IndexBuffer);
    gl.DeleteVertexArrays(1, &m_VertexArray);
    gl.DeleteProgram(m_Program);
    m_VertexBuffer = m_IndexBuffer = m_VertexArray = m_Program = 0;
}

bool GLBlitSink::BeginFrame(uint8_t* target, int32_t width, int32_t height, Deki::ColorFormat format, uint8_t clearR,
                            uint8_t clearG, uint8_t clearB)
{
    m_Target = nullptr;
    m_Written = false;
    m_Vertices.clear();
    m_Runs.clear();
    const char* why = nullptr;
    if (!m_Enabled)
    {
        why = "turned off";
    }
    else if (!m_Presenter.TakesLayers(width, height, format))
    {
        why = "the frame buffer is not RGB565A8 at the frame's size";
    }
    else if (!m_Presenter.m_Context->MakeCurrent())
    {
        why = "OpenGL is gone";
    }
    // Which way 2D reaches the screen, said once and again whenever it
    // changes, since the two differ so much in cost.
    const int mode = why ? 2 : 1;
    if (mode != m_LoggedMode)
    {
        m_LoggedMode = mode;
        if (why)
        {
            DEKI_LOG_INFO("OpenGL: drawing 2D on the CPU (%s)", why);
        }
        else
        {
            DEKI_LOG_INFO("OpenGL: drawing 2D straight into the presented frame");
        }
    }
    if (why)
    {
        return false;
    }

    if (++m_Frame % 60 == 0)
    {
        Evict();
    }
    // The colour the CPU's clear writes: RGB565A8 keeps 565.
    DekiPixel::QuantizeRGB565(clearR, clearG, clearB);
    m_Presenter.ClearFrame(width, height, clearR, clearG, clearB);
    if (!m_Presenter.m_InFrame)
    {
        return false;  // no frame to draw in; said by the presenter
    }
    std::memset(target, 0, Deki::FrameBufferBytes(format, width, height));
    m_Target = target;
    m_Width = width;
    m_Height = height;
    m_Format = format;
    return true;
}

bool GLBlitSink::Draw(const QuadBlit::SinkBlit& blit)
{
    const QuadBlit::Source& source = *blit.source;
    if (!m_Target || !m_Presenter.m_InFrame || source.width > m_MaxTextureSize || source.height > m_MaxTextureSize ||
        blit.sampleWidth > 32767 || blit.sampleHeight > 32767 || blit.right > 32767 || blit.bottom > 32767)
    {
        return false;
    }

    // What was written into the buffer since the last quad goes under this one.
    if (m_Written)
    {
        m_Written = false;
        m_Presenter.PushLayer(m_Target, m_Width, m_Height, m_Format);
        std::memset(m_Target, 0, Deki::FrameBufferBytes(m_Format, m_Width, m_Height));
    }

    // A full batch draws before the texture is found, so a texture queued
    // with this quad is queued in the batch it is drawn in.
    if (m_Vertices.size() >= static_cast<size_t>(kMaxQuads) * 4)
    {
        Flush();
    }
    const GLuint texture = TextureFor(source);
    if (!texture)
    {
        return false;
    }

    Vertex v;
    v.origin[0] = blit.originX;
    v.origin[1] = blit.originY;
    v.origin[2] = blit.originSx;
    v.origin[3] = blit.originSy;
    v.step[0] = blit.dSxDx;
    v.step[1] = blit.dSyDx;
    v.step[2] = blit.dSxDy;
    v.step[3] = blit.dSyDy;
    v.size[0] = static_cast<int16_t>(source.width);
    v.size[1] = static_cast<int16_t>(source.height);
    v.size[2] = static_cast<int16_t>(blit.sampleWidth);
    v.size[3] = static_cast<int16_t>(blit.sampleHeight);
    v.tint[0] = blit.tintR;
    v.tint[1] = blit.tintG;
    v.tint[2] = blit.tintB;
    v.tint[3] = blit.tintA;
    v.key[0] = source.keyR;
    v.key[1] = source.keyG;
    v.key[2] = source.keyB;
    uint8_t flags = 0;
    flags |= source.flipH ? kFlagFlipH : 0;
    flags |= source.flipV ? kFlagFlipV : 0;
    flags |= source.flipD && source.width == source.height ? kFlagFlipD : 0;
    flags |= blit.repeatColumn ? kFlagRepeatColumn : 0;
    flags |= blit.repeatRow ? kFlagRepeatRow : 0;
    flags |= source.hasChromaKey ? kFlagKey : 0;
    flags |= blit.orderedDither ? kFlagDither : 0;
    v.key[3] = flags;
    const int16_t xs[2] = { static_cast<int16_t>(blit.left), static_cast<int16_t>(blit.right) };
    const int16_t ys[2] = { static_cast<int16_t>(blit.top), static_cast<int16_t>(blit.bottom) };
    for (int corner = 0; corner < 4; ++corner)
    {
        v.x = xs[corner & 1];
        v.y = ys[corner >> 1];
        m_Vertices.push_back(v);
    }

    const uint32_t quad = static_cast<uint32_t>(m_Vertices.size() / 4 - 1);
    if (!m_Runs.empty() && m_Runs.back().texture == texture)
    {
        ++m_Runs.back().count;
    }
    else
    {
        m_Runs.push_back({ texture, quad, 1 });
    }
    ++m_Stats.blits;
    return true;
}

void GLBlitSink::TargetWritten()
{
    if (m_Target)
    {
        m_Written = true;
    }
}

void GLBlitSink::EndFrame()
{
    Flush();
    // What the buffer still holds, the display composes as it presents.
    m_Target = nullptr;
    m_Written = false;
}

void GLBlitSink::LayerPushed(const uint8_t* buffer)
{
    if (buffer && buffer == m_Target)
    {
        m_Written = false;
    }
}

void GLBlitSink::Forget(const uint8_t* begin, size_t bytes)
{
    // No OpenGL here (a sprite may go while no context is current): the
    // texture is marked, uploaded again if drawn, and evicted if not.
    const uint8_t* end = begin + bytes;
    for (auto& entry : m_Textures)
    {
        if (entry.first.pixels >= begin && entry.first.pixels < end)
        {
            entry.second.revision = 0;
            entry.second.stable = false;
        }
    }
}

GLuint GLBlitSink::TextureFor(const QuadBlit::Source& source)
{
    const GL& gl = m_Presenter.m_Context->Functions();
    // Pixels handed over (ownsPixels) are freed after the blit, and the next
    // ones may land anywhere: they go up into one texture kept for them, not
    // under their address.
    if (source.ownsPixels)
    {
        if (!m_Scratch.id)
        {
            gl.GenTextures(1, &m_Scratch.id);
        }
        if (m_Scratch.batch == m_Batch && !m_Vertices.empty())
        {
            Flush();
        }
        Upload(m_Scratch, source);
        m_Scratch.batch = m_Batch;
        return m_Scratch.id;
    }

    const int32_t stride = source.stride ? source.stride : source.width * source.bytesPerPixel;
    Key key;
    key.pixels = source.pixels;
    key.width = source.width;
    key.height = source.height;
    key.stride = stride;
    key.bytesPerPixel = source.bytesPerPixel;
    key.isRGB565 = source.isRGB565;
    key.hasAlpha = source.hasAlpha;
    auto [it, fresh] = m_Textures.try_emplace(key);
    Texture& texture = it->second;
    if (fresh)
    {
        gl.GenTextures(1, &texture.id);
        if (!texture.id)
        {
            m_Textures.erase(it);
            return 0;
        }
    }
    // What it holds is these pixels: the revision says so, or the source
    // promises they stay as they are and nothing said they went.
    const bool current =
        !fresh && (source.revision != 0 ? texture.revision == source.revision : source.cacheScaled && texture.stable);
    if (!current)
    {
        // Queued quads read what it holds now.
        if (texture.batch == m_Batch && !m_Vertices.empty())
        {
            Flush();
        }
        Upload(texture, source);
        texture.revision = source.revision;
        texture.stable = source.revision == 0 && source.cacheScaled;
    }
    texture.lastUsed = m_Frame;
    texture.batch = m_Batch;
    return texture.id;
}

void GLBlitSink::Upload(Texture& texture, const QuadBlit::Source& source)
{
    const int32_t w = source.width, h = source.height, bpp = source.bytesPerPixel;
    const int32_t stride = source.stride ? source.stride : w * bpp;
    m_Staging.resize(static_cast<size_t>(w) * h * 4);
    for (int32_t y = 0; y < h; ++y)
    {
        const uint8_t* row = source.pixels + static_cast<size_t>(y) * stride;
        uint8_t* out = &m_Staging[static_cast<size_t>(y) * w * 4];
        // QuadBlit's KindOf.
        if (source.isRGB565)
        {
            if (bpp >= 3)
            {
                SourceRowToRGBA<DekiPixel::SrcKind::RGB565A8>(row, w, bpp, source.hasAlpha, out);
            }
            else
            {
                SourceRowToRGBA<DekiPixel::SrcKind::RGB565>(row, w, bpp, source.hasAlpha, out);
            }
        }
        else if (bpp == 4)
        {
            SourceRowToRGBA<DekiPixel::SrcKind::RGBA8888>(row, w, bpp, source.hasAlpha, out);
        }
        else if (bpp == 3)
        {
            SourceRowToRGBA<DekiPixel::SrcKind::RGB888>(row, w, bpp, source.hasAlpha, out);
        }
        else
        {
            SourceRowToRGBA<DekiPixel::SrcKind::ALPHA8>(row, w, bpp, source.hasAlpha, out);
        }
    }

    const GL& gl = m_Presenter.m_Context->Functions();
    gl.ActiveTexture(gl::TEXTURE0);
    gl.BindTexture(gl::TEXTURE_2D, texture.id);
    gl.PixelStorei(gl::UNPACK_ALIGNMENT, 4);
    if (texture.width != w || texture.height != h)
    {
        gl.TexImage2D(gl::TEXTURE_2D, 0, static_cast<GLint>(gl::RGBA8), w, h, 0, gl::RGBA, gl::UNSIGNED_BYTE,
                      m_Staging.data());
        if (texture.width == 0)
        {
            // Read with texelFetch, but a texture is only complete for it
            // with a filter that needs no mipmaps.
            gl.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, static_cast<GLint>(gl::NEAREST));
            gl.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, static_cast<GLint>(gl::NEAREST));
            gl.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, static_cast<GLint>(gl::CLAMP_TO_EDGE));
            gl.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, static_cast<GLint>(gl::CLAMP_TO_EDGE));
        }
        m_TextureBytes -= static_cast<size_t>(texture.width) * texture.height * 4;
        m_TextureBytes += static_cast<size_t>(w) * h * 4;
        texture.width = w;
        texture.height = h;
    }
    else
    {
        gl.TexSubImage2D(gl::TEXTURE_2D, 0, 0, 0, w, h, gl::RGBA, gl::UNSIGNED_BYTE, m_Staging.data());
    }
    ++m_Stats.uploads;
    m_Stats.textures = static_cast<uint32_t>(m_Textures.size());
    m_Stats.textureBytes = m_TextureBytes;
}

void GLBlitSink::DeleteTexture(Texture& texture)
{
    const GL& gl = m_Presenter.m_Context->Functions();
    gl.DeleteTextures(1, &texture.id);
    m_TextureBytes -= static_cast<size_t>(texture.width) * texture.height * 4;
    texture = Texture{};
}

void GLBlitSink::Evict()
{
    for (auto it = m_Textures.begin(); it != m_Textures.end();)
    {
        if (m_Frame - it->second.lastUsed > kIdleFrames)
        {
            DeleteTexture(it->second);
            it = m_Textures.erase(it);
        }
        else
        {
            ++it;
        }
    }
    // Over the budget: the ones drawn longest ago, never one drawn this frame.
    while (m_TextureBytes > kTextureBudget)
    {
        auto oldest = m_Textures.end();
        for (auto it = m_Textures.begin(); it != m_Textures.end(); ++it)
        {
            if (it->second.lastUsed != m_Frame &&
                (oldest == m_Textures.end() || it->second.lastUsed < oldest->second.lastUsed))
            {
                oldest = it;
            }
        }
        if (oldest == m_Textures.end())
        {
            break;
        }
        DeleteTexture(oldest->second);
        m_Textures.erase(oldest);
    }
    m_Stats.textures = static_cast<uint32_t>(m_Textures.size());
    m_Stats.textureBytes = m_TextureBytes;
}

void GLBlitSink::Flush()
{
    if (m_Vertices.empty())
    {
        return;
    }
    if (!m_Presenter.m_InFrame || !m_Presenter.m_Context->MakeCurrent())
    {
        m_Vertices.clear();
        m_Runs.clear();
        return;
    }
    const GL& gl = m_Presenter.m_Context->Functions();
    const int width = m_Presenter.m_Width, height = m_Presenter.m_Height;

    // Everything set each time: meshes and layers draw in between.
    gl.BindFramebuffer(gl::FRAMEBUFFER, m_Presenter.m_Framebuffer);
    gl.Viewport(0, 0, width, height);
    gl.Disable(gl::DEPTH_TEST);
    gl.Disable(gl::CULL_FACE);
    gl.Disable(gl::SCISSOR_TEST);
    gl.Disable(gl::DITHER);
    gl.Enable(gl::BLEND);
    gl.BlendFunc(gl::ONE, gl::ONE_MINUS_SRC_ALPHA);
    gl.UseProgram(m_Program);
    gl.Uniform2f(m_FrameUniform, static_cast<float>(width), static_cast<float>(height));
    gl.Uniform1i(m_HeightUniform, height);
    gl.Uniform1i(m_TextureUniform, 0);
    gl.BindVertexArray(m_VertexArray);
    gl.BindBuffer(gl::ARRAY_BUFFER, m_VertexBuffer);
    gl.BufferData(gl::ARRAY_BUFFER, static_cast<GLsizeiptr>(m_Vertices.size() * sizeof(Vertex)), m_Vertices.data(),
                  gl::STREAM_DRAW);
    gl.ActiveTexture(gl::TEXTURE0);
    for (const Run& run : m_Runs)
    {
        gl.BindTexture(gl::TEXTURE_2D, run.texture);
        gl.DrawElements(gl::TRIANGLES, static_cast<GLsizei>(run.count * 6), gl::UNSIGNED_SHORT,
                        reinterpret_cast<const void*>(static_cast<uintptr_t>(run.first) * 6 * sizeof(uint16_t)));
    }
    gl.Disable(gl::BLEND);
    m_Stats.draws += static_cast<uint32_t>(m_Runs.size());

    const GLenum error = gl.GetError();
    if (error != 0 && !m_ErrorReported)
    {
        m_ErrorReported = true;
        DEKI_LOG_WARNING("OpenGL: OpenGL error 0x%04X drawing 2D", error);
    }
    m_Vertices.clear();
    m_Runs.clear();
    ++m_Batch;
}

}  // namespace DekiGL

#endif  // DEKI_CAPABILITY_OPENGL
