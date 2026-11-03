// Only where the platform has OpenGL: the build defines DEKI_CAPABILITY_OPENGL
// for one whose platform provides "opengl" (the desktop, a Linux handheld) and
// never for a microcontroller, where this file compiles to nothing.
#ifdef DEKI_CAPABILITY_OPENGL

#include "GLPresenter.h"

#include "GLBlitSink.h"
#include "ScreenEffect.h"
#include "deki-rendering/PixelFormat.h"
#include "deki-rendering/QuadBlit.h"
#include "deki-rendering/ShaderAsset.h"

#include <deki/LogSystem.h>

#include <algorithm>
#include <chrono>
#include <string>

namespace DekiGL
{
namespace
{

GLPresenter* s_Active = nullptr;

// One triangle over the whole target, from the vertex id alone: no buffers.
const char* const kVertexShader = R"(
void main()
{
    vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0);
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

// A layer's texel for each frame pixel, exactly: the layer's rows run top
// down like the CPU buffer, the frame's bottom up like GL.
const char* const kFragmentShader = R"(
precision highp float;
precision highp int;
uniform highp sampler2D uLayer;
uniform int uHeight;
out vec4 fragColor;
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    fragColor = texelFetch(uLayer, ivec2(p.x, uHeight - 1 - p.y), 0);
}
)";

/// One row of `format` as RGBA8, premultiplied. RGB565A8 from the 2D
/// blitter already is (a cleared, transparent buffer blends to colour times
/// coverage); the other formats have no alpha and are opaque. Returns
/// whether the row covers anything.
template <Deki::ColorFormat F>
bool RowToRGBA(const uint8_t* row, int width, uint8_t* out)
{
    bool any = false;
    for (int x = 0; x < width; ++x, out += 4)
    {
        DekiPixel::ReadDstPixel<F>(row, static_cast<size_t>(x), out[0], out[1], out[2], out[3]);
        any = any || out[3] != 0;
    }
    return any;
}

}  // namespace

GLPresenter* GLPresenter::Active()
{
    return s_Active;
}

GLPresenter::GLPresenter(std::shared_ptr<GLContext> context, const Window& window)
    : m_Context(std::move(context)), m_Window(window)
{
}

std::unique_ptr<GLPresenter> GLPresenter::Create(std::shared_ptr<GLContext> context, const Window& window)
{
    if (!context || !context->MakeCurrent())
    {
        return nullptr;
    }
    std::unique_ptr<GLPresenter> presenter(new GLPresenter(std::move(context), window));
    if (!presenter->Build())
    {
        return nullptr;
    }
    SetWindowContext(presenter->m_Context);
    s_Active = presenter.get();
    // The 2D renderer's blits come here from now on; without the 2D shader
    // they stay on the CPU and arrive as layers, as before.
    presenter->m_Blits = std::make_unique<GLBlitSink>(*presenter);
    if (!presenter->m_Blits->Build())
    {
        DEKI_LOG_WARNING("OpenGL: drawing 2D on the CPU: its shader does not build");
        presenter->m_Blits->Release();
        presenter->m_Blits.reset();
    }
    QuadBlit::SetBlitSink(presenter->m_Blits.get());
    const GL& gl = presenter->m_Context->Functions();
    const char* renderer = reinterpret_cast<const char*>(gl.GetString(gl::RENDERER));
    const char* version = reinterpret_cast<const char*>(gl.GetString(gl::VERSION));
    DEKI_LOG_INFO("OpenGL: presenting through %s, %s", renderer ? renderer : "?", version ? version : "?");
    return presenter;
}

GLPresenter::~GLPresenter()
{
    if (s_Active == this)
    {
        s_Active = nullptr;
        SetWindowContext(nullptr);
    }
    if (m_Blits && QuadBlit::GetBlitSink() == m_Blits.get())
    {
        QuadBlit::SetBlitSink(nullptr);
    }
    if (m_Context->MakeCurrent())
    {
        const GL& gl = m_Context->Functions();
        if (m_Blits)
        {
            m_Blits->Release();
        }
        ReleaseFrame();
        gl.DeleteTextures(1, &m_LayerTexture);
        gl.DeleteVertexArrays(1, &m_VertexArray);
        gl.DeleteProgram(m_Program);
        for (auto& entry : m_Effects)
        {
            gl.DeleteProgram(entry.second.id);
        }
    }
}

bool GLPresenter::Build()
{
    const GL& gl = m_Context->Functions();
    const char* version = m_Context->IsES() ? "#version 300 es\n" : "#version 330 core\n";
    auto compile = [&](GLenum type, const char* body) -> GLuint
    {
        const GLuint shader = gl.CreateShader(type);
        const GLchar* sources[] = { version, body };
        gl.ShaderSource(shader, 2, sources, nullptr);
        gl.CompileShader(shader);
        GLint ok = 0;
        gl.GetShaderiv(shader, gl::COMPILE_STATUS, &ok);
        if (!ok)
        {
            GLint length = 0;
            gl.GetShaderiv(shader, gl::INFO_LOG_LENGTH, &length);
            std::string log(static_cast<size_t>(std::max(length, 1)), '\0');
            gl.GetShaderInfoLog(shader, length, nullptr, log.data());
            DEKI_LOG_ERROR("OpenGL: compiling the layer shader failed: %s", log.c_str());
            gl.DeleteShader(shader);
            return 0;
        }
        return shader;
    };
    const GLuint vs = compile(gl::VERTEX_SHADER, kVertexShader);
    const GLuint fs = compile(gl::FRAGMENT_SHADER, kFragmentShader);
    if (!vs || !fs)
    {
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
        DEKI_LOG_ERROR("OpenGL: linking the layer shader failed");
        return false;
    }
    m_LayerUniform = gl.GetUniformLocation(m_Program, "uLayer");
    m_HeightUniform = gl.GetUniformLocation(m_Program, "uHeight");
    // Core profiles draw nothing without a vertex array bound, even one that
    // holds no attributes.
    gl.GenVertexArrays(1, &m_VertexArray);
    gl.GenTextures(1, &m_LayerTexture);
    return gl.GetError() == 0;
}

void GLPresenter::ReleaseFrame()
{
    const GL& gl = m_Context->Functions();
    if (m_Framebuffer)
    {
        gl.DeleteFramebuffers(1, &m_Framebuffer);
    }
    gl.DeleteTextures(1, &m_ColorTexture);
    gl.DeleteRenderbuffers(1, &m_DepthBuffer);
    m_Framebuffer = m_ColorTexture = m_DepthBuffer = 0;
    m_Width = m_Height = 0;
}

bool GLPresenter::EnsureFrame(int width, int height)
{
    if (m_Framebuffer && width == m_Width && height == m_Height)
    {
        return true;
    }
    const GL& gl = m_Context->Functions();
    ReleaseFrame();
    gl.GenFramebuffers(1, &m_Framebuffer);
    gl.GenTextures(1, &m_ColorTexture);
    gl.GenRenderbuffers(1, &m_DepthBuffer);
    gl.BindTexture(gl::TEXTURE_2D, m_ColorTexture);
    gl.TexImage2D(gl::TEXTURE_2D, 0, static_cast<GLint>(gl::RGBA8), width, height, 0, gl::RGBA, gl::UNSIGNED_BYTE,
                  nullptr);
    // Nearest: an effect samples the frame's pixels as they are, sharp.
    gl.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, static_cast<GLint>(gl::NEAREST));
    gl.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, static_cast<GLint>(gl::NEAREST));
    gl.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, static_cast<GLint>(gl::CLAMP_TO_EDGE));
    gl.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, static_cast<GLint>(gl::CLAMP_TO_EDGE));
    gl.BindRenderbuffer(gl::RENDERBUFFER, m_DepthBuffer);
    gl.RenderbufferStorage(gl::RENDERBUFFER, gl::DEPTH_COMPONENT24, width, height);
    gl.BindFramebuffer(gl::FRAMEBUFFER, m_Framebuffer);
    gl.FramebufferTexture2D(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT0, gl::TEXTURE_2D, m_ColorTexture, 0);
    gl.FramebufferRenderbuffer(gl::FRAMEBUFFER, gl::DEPTH_ATTACHMENT, gl::RENDERBUFFER, m_DepthBuffer);
    if (gl.CheckFramebufferStatus(gl::FRAMEBUFFER) != gl::FRAMEBUFFER_COMPLETE)
    {
        DEKI_LOG_ERROR("OpenGL: no %dx%d frame to build on", width, height);
        ReleaseFrame();
        return false;
    }
    gl.BindTexture(gl::TEXTURE_2D, m_LayerTexture);
    gl.TexImage2D(gl::TEXTURE_2D, 0, static_cast<GLint>(gl::RGBA8), width, height, 0, gl::RGBA, gl::UNSIGNED_BYTE,
                  nullptr);
    gl.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, static_cast<GLint>(gl::NEAREST));
    gl.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, static_cast<GLint>(gl::NEAREST));
    gl.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, static_cast<GLint>(gl::CLAMP_TO_EDGE));
    gl.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, static_cast<GLint>(gl::CLAMP_TO_EDGE));
    m_Width = width;
    m_Height = height;
    return true;
}

void GLPresenter::StartFrame(int width, int height)
{
    if (m_InFrame || !EnsureFrame(width, height))
    {
        return;
    }
    const GL& gl = m_Context->Functions();
    gl.BindFramebuffer(gl::FRAMEBUFFER, m_Framebuffer);
    gl.Viewport(0, 0, width, height);
    gl.Disable(gl::SCISSOR_TEST);
    gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl.SetClearDepth(1.0f);
    gl.DepthMask(gl::kTrue);
    gl.Clear(gl::COLOR_BUFFER_BIT | gl::DEPTH_BUFFER_BIT);
    m_InFrame = true;
}

void GLPresenter::ClearFrame(int width, int height, uint8_t r, uint8_t g, uint8_t b)
{
    StartFrame(width, height);
    if (!m_InFrame)
    {
        return;
    }
    const GL& gl = m_Context->Functions();
    gl.BindFramebuffer(gl::FRAMEBUFFER, m_Framebuffer);
    gl.Viewport(0, 0, m_Width, m_Height);
    gl.Disable(gl::SCISSOR_TEST);
    gl.ClearColor(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f, 1.0f);
    gl.Clear(gl::COLOR_BUFFER_BIT);
}

void GLPresenter::Flush2D()
{
    if (m_Blits)
    {
        m_Blits->Flush();
    }
}

void GLPresenter::SetDraws2D(bool draws)
{
    if (m_Blits)
    {
        m_Blits->SetEnabled(draws);
    }
}

bool GLPresenter::Draws2D() const
{
    return m_Blits && m_Blits->Enabled();
}

GLPresenter::DrawStats GLPresenter::Stats2D() const
{
    return m_Blits ? m_Blits->Stats() : DrawStats{};
}

bool GLPresenter::TakesLayers(int width, int height, Deki::ColorFormat format) const
{
    return format == Deki::ColorFormat::RGB565A8 && (!m_InFrame || (width == m_Width && height == m_Height));
}

void GLPresenter::BindFrame(int width, int height)
{
    if (!m_Context->MakeCurrent())
    {
        return;
    }
    Flush2D();
    StartFrame(width, height);
    const GL& gl = m_Context->Functions();
    gl.BindFramebuffer(gl::FRAMEBUFFER, m_Framebuffer);
    gl.Viewport(0, 0, m_Width, m_Height);
}

void GLPresenter::PushLayer(const uint8_t* buffer, int width, int height, Deki::ColorFormat format)
{
    if (!buffer || width <= 0 || height <= 0 || !m_Context->MakeCurrent())
    {
        return;
    }
    StartFrame(width, height);
    if (!m_InFrame || width != m_Width || height != m_Height)
    {
        return;
    }
    // The 2D queued so far is below this layer. If the layer is the 2D's own
    // buffer, what was written into it is in the frame after this.
    Flush2D();
    if (m_Blits)
    {
        m_Blits->LayerPushed(buffer);
    }

    // To RGBA8, noting the rows that hold anything: a layer of interface over
    // 3D is mostly transparent, and only its covered rows go up and draw.
    m_Staging.resize(static_cast<size_t>(width) * height * 4);
    const size_t pitch = Deki::FrameBufferBytes(format, width, 1);
    int first = height, last = -1;
    for (int y = 0; y < height; ++y)
    {
        const uint8_t* row = buffer + static_cast<size_t>(y) * pitch;
        uint8_t* out = &m_Staging[static_cast<size_t>(y) * width * 4];
        bool any = false;
        switch (format)
        {
            case Deki::ColorFormat::RGB565: any = RowToRGBA<Deki::ColorFormat::RGB565>(row, width, out); break;
            case Deki::ColorFormat::RGB888: any = RowToRGBA<Deki::ColorFormat::RGB888>(row, width, out); break;
            case Deki::ColorFormat::ARGB8888: any = RowToRGBA<Deki::ColorFormat::ARGB8888>(row, width, out); break;
            case Deki::ColorFormat::RGB565A8: any = RowToRGBA<Deki::ColorFormat::RGB565A8>(row, width, out); break;
        }
        if (any)
        {
            first = std::min(first, y);
            last = y;
        }
    }
    if (last < 0)
    {
        return;  // all transparent
    }

    const GL& gl = m_Context->Functions();
    const int rows = last - first + 1;
    gl.ActiveTexture(gl::TEXTURE0);
    gl.BindTexture(gl::TEXTURE_2D, m_LayerTexture);
    gl.PixelStorei(gl::UNPACK_ALIGNMENT, 4);
    gl.TexSubImage2D(gl::TEXTURE_2D, 0, 0, first, width, rows, gl::RGBA, gl::UNSIGNED_BYTE,
                     &m_Staging[static_cast<size_t>(first) * width * 4]);

    gl.BindFramebuffer(gl::FRAMEBUFFER, m_Framebuffer);
    gl.Viewport(0, 0, width, height);
    gl.Disable(gl::DEPTH_TEST);
    gl.Disable(gl::CULL_FACE);
    gl.Enable(gl::BLEND);
    gl.BlendFunc(gl::ONE, gl::ONE_MINUS_SRC_ALPHA);
    // Only the covered rows; GL counts them from the bottom.
    gl.Enable(gl::SCISSOR_TEST);
    gl.Scissor(0, height - 1 - last, width, rows);
    gl.UseProgram(m_Program);
    gl.BindVertexArray(m_VertexArray);
    gl.Uniform1i(m_LayerUniform, 0);
    gl.Uniform1i(m_HeightUniform, height);
    gl.DrawArrays(gl::TRIANGLES, 0, 3);
    gl.Disable(gl::SCISSOR_TEST);
    gl.Disable(gl::BLEND);

    const GLenum error = gl.GetError();
    if (error != 0 && !m_ErrorReported)
    {
        m_ErrorReported = true;
        DEKI_LOG_WARNING("OpenGL: OpenGL error 0x%04X composing a layer", error);
    }
}

void GLPresenter::Present(const uint8_t* buffer, int width, int height, Deki::ColorFormat format)
{
    Effect effect;
    if (const ScreenEffect* scene = ScreenEffect::Current())
    {
        effect.shader = scene->shader.Get();
        effect.params = scene->params;
        effect.color = scene->color.ToABGR8888();  // 0xAABBGGRR, as GLPresenter::Effect wants
    }
    Present(buffer, width, height, format, effect);
}

void GLPresenter::Present(const uint8_t* buffer, int width, int height, Deki::ColorFormat format,
                          const Effect& effect)
{
    if (!m_Context->MakeCurrent())
    {
        return;
    }
    PushLayer(buffer, width, height, format);
    if (!m_InFrame)
    {
        return;
    }
    const GL& gl = m_Context->Functions();

    int windowW = 0, windowH = 0;
    m_Window.drawableSize(m_Window.user, &windowW, &windowH);
    // Whole pixels where the window allows, centred; a window smaller than
    // the screen gets the largest fit that keeps the shape.
    int scale = std::min(windowW / m_Width, windowH / m_Height);
    int dstW = m_Width * scale, dstH = m_Height * scale;
    if (scale < 1)
    {
        const float fit = std::min(static_cast<float>(windowW) / m_Width, static_cast<float>(windowH) / m_Height);
        dstW = static_cast<int>(m_Width * fit);
        dstH = static_cast<int>(m_Height * fit);
    }
    const int dstX = (windowW - dstW) / 2;
    const int dstY = (windowH - dstH) / 2;

    gl.BindFramebuffer(gl::DRAW_FRAMEBUFFER, m_Window.target);
    gl.Viewport(0, 0, windowW, windowH);
    gl.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl.Clear(gl::COLOR_BUFFER_BIT);
    if (!effect.shader || !DrawEffect(effect, dstX, dstY, dstW, dstH))
    {
        gl.BindFramebuffer(gl::READ_FRAMEBUFFER, m_Framebuffer);
        gl.BlitFramebuffer(0, 0, m_Width, m_Height, dstX, dstY, dstX + dstW, dstY + dstH, gl::COLOR_BUFFER_BIT,
                           gl::NEAREST);
    }

    // The frame's contents are spent. Saying so lets a tiled GPU (Mali)
    // skip writing them back to memory.
    if (gl.InvalidateFramebuffer)
    {
        gl.BindFramebuffer(gl::FRAMEBUFFER, m_Framebuffer);
        const GLenum spent[2] = { gl::COLOR_ATTACHMENT0, gl::DEPTH_ATTACHMENT };
        gl.InvalidateFramebuffer(gl::FRAMEBUFFER, 2, spent);
    }
    gl.BindFramebuffer(gl::FRAMEBUFFER, 0);
    m_Window.swap(m_Window.user);
    m_InFrame = false;
}

GLuint GLPresenter::Compile(GLenum type, const std::string& source, const char* what)
{
    const GL& gl = m_Context->Functions();
    const GLuint shader = gl.CreateShader(type);
    const GLchar* sources[] = { source.c_str() };
    gl.ShaderSource(shader, 1, sources, nullptr);
    gl.CompileShader(shader);
    GLint ok = 0;
    gl.GetShaderiv(shader, gl::COMPILE_STATUS, &ok);
    if (!ok)
    {
        GLint length = 0;
        gl.GetShaderiv(shader, gl::INFO_LOG_LENGTH, &length);
        std::string log(static_cast<size_t>(std::max(length, 1)), '\0');
        gl.GetShaderInfoLog(shader, length, nullptr, log.data());
        DEKI_LOG_ERROR("OpenGL: %s does not build: %s", what, log.c_str());
        gl.DeleteShader(shader);
        return 0;
    }
    return shader;
}

GLPresenter::EffectProgram GLPresenter::LinkEffect(const DekiRendering::ShaderAsset& shader)
{
    const GL& gl = m_Context->Functions();
    // The asset's "effect" program, compiled from HLSL by the editor
    // (editor/EffectShaderUse.cpp): both stages, for this context's GL.
    const DekiRendering::ShaderProgramSource* source = shader.Program("effect", m_Context->IsES());
    EffectProgram program;
    const GLuint vs = Compile(gl::VERTEX_SHADER, source->vertex, "a screen effect's shader");
    const GLuint fs = vs ? Compile(gl::FRAGMENT_SHADER, source->fragment, "a screen effect's shader") : 0;
    if (!vs || !fs)
    {
        gl.DeleteShader(vs);
        DEKI_LOG_WARNING("OpenGL: showing the frame without its screen effect");
        return program;
    }
    program.id = gl.CreateProgram();
    gl.AttachShader(program.id, vs);
    gl.AttachShader(program.id, fs);
    gl.LinkProgram(program.id);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    GLint linked = 0;
    gl.GetProgramiv(program.id, gl::LINK_STATUS, &linked);
    if (!linked)
    {
        DEKI_LOG_ERROR("OpenGL: a screen effect's shader does not link; showing the frame without it");
        gl.DeleteProgram(program.id);
        program.id = 0;
        return program;
    }
    // The compiler keeps the uniforms in one struct, `u`, and names a texture
    // as the shader does (deki-editor's ShaderCompiler.h).
    program.frame = gl.GetUniformLocation(program.id, "uFrame");
    program.dst = gl.GetUniformLocation(program.id, "u.uDst");
    program.resolution = gl.GetUniformLocation(program.id, "u.uResolution");
    program.time = gl.GetUniformLocation(program.id, "u.uTime");
    program.params = gl.GetUniformLocation(program.id, "u.uParams");
    program.color = gl.GetUniformLocation(program.id, "u.uColor");
    return program;
}

bool GLPresenter::DrawEffect(const Effect& effect, int x, int y, int width, int height)
{
    const DekiRendering::ShaderAsset* shader = effect.shader;
    if (!shader || !shader->Program("effect", m_Context->IsES()))
    {
        return false;
    }
    auto [it, fresh] = m_Effects.try_emplace(shader->Revision());
    if (fresh)
    {
        it->second = LinkEffect(*shader);
    }
    const EffectProgram& program = it->second;
    if (!program.id)
    {
        return false;
    }

    // The effect's clock, from its first frame.
    static const auto s_Start = std::chrono::steady_clock::now();
    const float time = std::chrono::duration<float>(std::chrono::steady_clock::now() - s_Start).count();
    const uint32_t c = effect.color;  // 0xAABBGGRR

    const GL& gl = m_Context->Functions();
    gl.BindFramebuffer(gl::DRAW_FRAMEBUFFER, m_Window.target);
    gl.Viewport(x, y, width, height);
    gl.Disable(gl::BLEND);
    gl.Disable(gl::DEPTH_TEST);
    gl.Disable(gl::CULL_FACE);
    gl.UseProgram(program.id);
    gl.BindVertexArray(m_VertexArray);
    gl.ActiveTexture(gl::TEXTURE0);
    gl.BindTexture(gl::TEXTURE_2D, m_ColorTexture);
    gl.Uniform1i(program.frame, 0);
    gl.Uniform4f(program.dst, static_cast<float>(x), static_cast<float>(y), static_cast<float>(width),
                 static_cast<float>(height));
    gl.Uniform2f(program.resolution, static_cast<float>(m_Width), static_cast<float>(m_Height));
    gl.Uniform1f(program.time, time);
    gl.Uniform3f(program.params, effect.params.x, effect.params.y, effect.params.z);
    gl.Uniform4f(program.color, static_cast<float>(c & 0xFF) / 255.0f, static_cast<float>((c >> 8) & 0xFF) / 255.0f,
                 static_cast<float>((c >> 16) & 0xFF) / 255.0f, static_cast<float>((c >> 24) & 0xFF) / 255.0f);
    gl.DrawArrays(gl::TRIANGLES, 0, 3);
    return true;
}

bool GLPresenter::ReadFrame(std::vector<uint8_t>& rgba)
{
    if (!m_InFrame || !m_Context->MakeCurrent())
    {
        return false;
    }
    Flush2D();
    const GL& gl = m_Context->Functions();
    const size_t row = static_cast<size_t>(m_Width) * 4;
    std::vector<uint8_t> bottomUp(row * m_Height);
    gl.BindFramebuffer(gl::FRAMEBUFFER, m_Framebuffer);
    gl.PixelStorei(gl::PACK_ALIGNMENT, 4);
    gl.ReadPixels(0, 0, m_Width, m_Height, gl::RGBA, gl::UNSIGNED_BYTE, bottomUp.data());
    rgba.resize(bottomUp.size());
    for (int y = 0; y < m_Height; ++y)
    {
        std::copy_n(&bottomUp[(m_Height - 1 - y) * row], row, &rgba[y * row]);
    }
    return gl.GetError() == 0;
}

}  // namespace DekiGL

#endif  // DEKI_CAPABILITY_OPENGL
