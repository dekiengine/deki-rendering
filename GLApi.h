#pragma once

// Only where the platform has OpenGL: the build defines DEKI_CAPABILITY_OPENGL
// for one whose platform provides "opengl" (the desktop, a Linux handheld) and
// never for a microcontroller, where this file compiles to nothing.
#ifdef DEKI_CAPABILITY_OPENGL

// The OpenGL Deki's GPU code uses, loaded at run time, and the context it
// draws in.
//
// Loaded rather than linked, so a package needs no GL headers or import
// libraries to build and a machine without a driver still runs the game: the
// context fails to come up, the caller says so, and drawing stays on the CPU.
// The calls are the common ground of OpenGL ES 3.0 and desktop OpenGL 3.3
// core, so one code path serves a handheld's Mali and a desktop.
//
// A context is either a window's, made by the display that shows the frames
// (GLContext::Adopt), or off-screen, made here: a hidden window for WGL on
// Windows, a 1x1 pbuffer or no surface at all through EGL on Linux. Every GL
// user in the program shares one (SharedContext), because objects made in one
// context cannot be used in another.

#include <cstddef>
#include <cstdint>
#include <memory>

#include "DekiRenderingPackage.h"  // DEKI_RENDERING_API

namespace DekiGL
{

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLboolean = unsigned char;
using GLfloat = float;
using GLdouble = double;
using GLbitfield = unsigned int;
using GLchar = char;
using GLubyte = unsigned char;
using GLsizeiptr = std::ptrdiff_t;

#if defined(_WIN32)
#define DEKI_GL_APIENTRY __stdcall
#else
#define DEKI_GL_APIENTRY
#endif

namespace gl
{
// GL_TRUE and GL_FALSE, renamed: <windows.h> defines TRUE and FALSE.
constexpr GLboolean kFalse = 0;
constexpr GLboolean kTrue = 1;
constexpr GLenum TRIANGLES = 0x0004;
constexpr GLenum TRIANGLE_STRIP = 0x0005;
constexpr GLenum ONE = 1;
constexpr GLenum ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr GLenum SRC_COLOR = 0x0300;
constexpr GLenum DST_COLOR = 0x0306;
constexpr GLenum NONE = 0;
constexpr GLenum LESS = 0x0201;
constexpr GLenum LEQUAL = 0x0203;
constexpr GLenum FRONT = 0x0404;
constexpr GLenum BACK = 0x0405;
constexpr GLenum CCW = 0x0901;
constexpr GLenum CULL_FACE = 0x0B44;
constexpr GLenum DEPTH_TEST = 0x0B71;
constexpr GLenum DITHER = 0x0BD0;
constexpr GLenum BLEND = 0x0BE2;
constexpr GLenum SCISSOR_TEST = 0x0C11;
constexpr GLenum UNPACK_ALIGNMENT = 0x0CF5;
constexpr GLenum PACK_ALIGNMENT = 0x0D05;
constexpr GLenum MAX_TEXTURE_SIZE = 0x0D33;
constexpr GLenum TEXTURE_2D = 0x0DE1;
constexpr GLenum UNSIGNED_BYTE = 0x1401;
constexpr GLenum SHORT = 0x1402;
constexpr GLenum UNSIGNED_SHORT = 0x1403;
constexpr GLenum INT = 0x1404;
constexpr GLenum UNSIGNED_INT = 0x1405;
constexpr GLenum FLOAT = 0x1406;
constexpr GLenum DEPTH_COMPONENT = 0x1902;
constexpr GLenum RGBA = 0x1908;
constexpr GLenum RENDERER = 0x1F01;
constexpr GLenum VERSION = 0x1F02;
constexpr GLenum NEAREST = 0x2600;
constexpr GLenum LINEAR = 0x2601;
constexpr GLenum LINEAR_MIPMAP_LINEAR = 0x2703;
constexpr GLenum TEXTURE_MAG_FILTER = 0x2800;
constexpr GLenum TEXTURE_MIN_FILTER = 0x2801;
constexpr GLenum TEXTURE_WRAP_S = 0x2802;
constexpr GLenum TEXTURE_WRAP_T = 0x2803;
constexpr GLenum REPEAT = 0x2901;
constexpr GLenum POLYGON_OFFSET_FILL = 0x8037;
constexpr GLenum CLAMP_TO_EDGE = 0x812F;
constexpr GLenum RGBA8 = 0x8058;
constexpr GLenum DEPTH_COMPONENT24 = 0x81A6;
constexpr GLenum TEXTURE0 = 0x84C0;
constexpr GLenum TEXTURE1 = 0x84C1;
constexpr GLenum TEXTURE2 = 0x84C2;
constexpr GLenum TEXTURE_COMPARE_MODE = 0x884C;
constexpr GLenum TEXTURE_COMPARE_FUNC = 0x884D;
constexpr GLenum COMPARE_REF_TO_TEXTURE = 0x884E;
constexpr GLenum ARRAY_BUFFER = 0x8892;
constexpr GLenum ELEMENT_ARRAY_BUFFER = 0x8893;
constexpr GLenum STREAM_DRAW = 0x88E0;
constexpr GLenum STATIC_DRAW = 0x88E4;
constexpr GLenum FRAGMENT_SHADER = 0x8B30;
constexpr GLenum VERTEX_SHADER = 0x8B31;
constexpr GLenum COMPILE_STATUS = 0x8B81;
constexpr GLenum LINK_STATUS = 0x8B82;
constexpr GLenum INFO_LOG_LENGTH = 0x8B84;
constexpr GLenum READ_FRAMEBUFFER = 0x8CA8;
constexpr GLenum DRAW_FRAMEBUFFER = 0x8CA9;
constexpr GLenum FRAMEBUFFER_COMPLETE = 0x8CD5;
constexpr GLenum COLOR_ATTACHMENT0 = 0x8CE0;
constexpr GLenum DEPTH_ATTACHMENT = 0x8D00;
constexpr GLenum FRAMEBUFFER = 0x8D40;
constexpr GLenum RENDERBUFFER = 0x8D41;
constexpr GLbitfield DEPTH_BUFFER_BIT = 0x0100;
constexpr GLbitfield COLOR_BUFFER_BIT = 0x4000;
}  // namespace gl

/// The entry points, named as in GL without the prefix: GL::DrawElements is
/// glDrawElements. Every one is set once the context is up, except ClearDepthf
/// (OpenGL ES and desktop 4.1) and ClearDepth (desktop), of which one is, and
/// InvalidateFramebuffer (OpenGL ES 3.0, desktop 4.3), which may be null.
struct GL
{
#define DEKI_GL_FUNCTIONS(X)                                                                                      \
    X(const GLubyte*, GetString, (GLenum name))                                                                   \
    X(GLenum, GetError, ())                                                                                       \
    X(void, GetIntegerv, (GLenum name, GLint * value))                                                            \
    X(void, Viewport, (GLint x, GLint y, GLsizei width, GLsizei height))                                         \
    X(void, Scissor,  (GLint x, GLint y, GLsizei width, GLsizei height))                                         \
    X(void, Enable, (GLenum cap))                                                                                 \
    X(void, Disable, (GLenum cap))                                                                                \
    X(void, Clear, (GLbitfield mask))                                                                             \
    X(void, ClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                                             \
    X(void, DepthFunc, (GLenum func))                                                                             \
    X(void, DepthMask, (GLboolean flag))                                                                          \
    X(void, CullFace, (GLenum mode))                                                                              \
    X(void, FrontFace, (GLenum mode))                                                                             \
    X(void, GenFramebuffers, (GLsizei n, GLuint * ids))                                                           \
    X(void, BindFramebuffer, (GLenum target, GLuint id))                                                          \
    X(void, FramebufferRenderbuffer, (GLenum target, GLenum attachment, GLenum rbTarget, GLuint rb))              \
    X(void, FramebufferTexture2D, (GLenum target, GLenum attachment, GLenum texTarget, GLuint texture, GLint level)) \
    X(GLenum, CheckFramebufferStatus, (GLenum target))                                                            \
    X(void, DeleteFramebuffers, (GLsizei n, const GLuint* ids))                                                   \
    X(void, GenRenderbuffers, (GLsizei n, GLuint * ids))                                                          \
    X(void, BindRenderbuffer, (GLenum target, GLuint id))                                                         \
    X(void, RenderbufferStorage, (GLenum target, GLenum format, GLsizei width, GLsizei height))                   \
    X(void, DeleteRenderbuffers, (GLsizei n, const GLuint* ids))                                                  \
    X(void, GenTextures, (GLsizei n, GLuint * ids))                                                               \
    X(void, BindTexture, (GLenum target, GLuint id))                                                              \
    X(void, TexImage2D,                                                                                           \
      (GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height, GLint border,             \
       GLenum format, GLenum type, const void* pixels))                                                           \
    X(void, TexParameteri, (GLenum target, GLenum name, GLint value))                                             \
    X(void, DeleteTextures, (GLsizei n, const GLuint* ids))                                                       \
    X(void, ActiveTexture, (GLenum unit))                                                                         \
    X(void, PixelStorei, (GLenum name, GLint value))                                                              \
    X(void, ReadPixels, (GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* out)) \
    X(void, GenBuffers, (GLsizei n, GLuint * ids))                                                                \
    X(void, BindBuffer, (GLenum target, GLuint id))                                                               \
    X(void, BufferData, (GLenum target, GLsizeiptr size, const void* data, GLenum usage))                         \
    X(void, DeleteBuffers, (GLsizei n, const GLuint* ids))                                                        \
    X(void, GenVertexArrays, (GLsizei n, GLuint * ids))                                                           \
    X(void, BindVertexArray, (GLuint id))                                                                         \
    X(void, DeleteVertexArrays, (GLsizei n, const GLuint* ids))                                                   \
    X(void, VertexAttribPointer,                                                                                  \
      (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* offset))          \
    X(void, VertexAttribIPointer, (GLuint index, GLint size, GLenum type, GLsizei stride, const void* offset))    \
    X(void, EnableVertexAttribArray, (GLuint index))                                                              \
    X(void, DisableVertexAttribArray, (GLuint index))                                                             \
    X(void, VertexAttrib4f, (GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w))                           \
    X(GLuint, CreateShader, (GLenum type))                                                                        \
    X(void, ShaderSource, (GLuint shader, GLsizei count, const GLchar* const* sources, const GLint* lengths))     \
    X(void, CompileShader, (GLuint shader))                                                                       \
    X(void, GetShaderiv, (GLuint shader, GLenum name, GLint * value))                                             \
    X(void, GetShaderInfoLog, (GLuint shader, GLsizei size, GLsizei * length, GLchar * log))                      \
    X(void, DeleteShader, (GLuint shader))                                                                        \
    X(GLuint, CreateProgram, ())                                                                                  \
    X(void, AttachShader, (GLuint program, GLuint shader))                                                        \
    X(void, LinkProgram, (GLuint program))                                                                        \
    X(void, GetProgramiv, (GLuint program, GLenum name, GLint * value))                                           \
    X(void, GetProgramInfoLog, (GLuint program, GLsizei size, GLsizei * length, GLchar * log))                    \
    X(void, UseProgram, (GLuint program))                                                                         \
    X(void, DeleteProgram, (GLuint program))                                                                      \
    X(GLint, GetUniformLocation, (GLuint program, const GLchar* name))                                            \
    X(void, Uniform1i, (GLint location, GLint value))                                                             \
    X(void, Uniform1f, (GLint location, GLfloat value))                                                           \
    X(void, Uniform2f, (GLint location, GLfloat x, GLfloat y))                                                    \
    X(void, Uniform3f, (GLint location, GLfloat x, GLfloat y, GLfloat z))                                         \
    X(void, Uniform4f, (GLint location, GLfloat x, GLfloat y, GLfloat z, GLfloat w))                              \
    X(void, UniformMatrix4fv, (GLint location, GLsizei count, GLboolean transpose, const GLfloat* value))         \
    X(void, DrawElements, (GLenum mode, GLsizei count, GLenum type, const void* offset))                         \
    X(void, DrawArrays, (GLenum mode, GLint first, GLsizei count))                                                \
    X(void, BlendFunc, (GLenum src, GLenum dst))                                                                  \
    X(void, ColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a))                                      \
    X(void, PolygonOffset, (GLfloat factor, GLfloat units))                                                       \
    X(void, DrawBuffers, (GLsizei n, const GLenum* buffers))                                                      \
    X(void, ReadBuffer, (GLenum buffer))                                                                          \
    X(void, GenerateMipmap, (GLenum target))                                                                      \
    X(void, TexSubImage2D,                                                                                        \
      (GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,   \
       const void* pixels))                                                                                       \
    X(void, BlitFramebuffer,                                                                                      \
      (GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,    \
       GLbitfield mask, GLenum filter))

#define DEKI_GL_DECLARE(ret, name, args) ret(DEKI_GL_APIENTRY* name) args = nullptr;
    DEKI_GL_FUNCTIONS(DEKI_GL_DECLARE)
#undef DEKI_GL_DECLARE

    void(DEKI_GL_APIENTRY* ClearDepthf)(GLfloat depth) = nullptr;
    void(DEKI_GL_APIENTRY* ClearDepth)(GLdouble depth) = nullptr;
    void(DEKI_GL_APIENTRY* InvalidateFramebuffer)(GLenum target, GLsizei count, const GLenum* attachments) = nullptr;

    void SetClearDepth(float depth) const
    {
        if (ClearDepthf)
        {
            ClearDepthf(depth);
        }
        else if (ClearDepth)
        {
            ClearDepth(depth);
        }
    }
};

/// A context and the functions loaded for it.
class DEKI_RENDERING_API GLContext
{
public:
    /// Brings an off-screen context up, or returns null and logs why: no
    /// driver, or one too old for OpenGL ES 3.0 / desktop OpenGL 3.3.
    static std::unique_ptr<GLContext> Create();

    /// How to reach a context someone else made: a display's, over its window.
    struct Host
    {
        void* (*load)(const char* name);   // the function named, or null
        bool (*makeCurrent)(void* user);  // binds it to the calling thread
        void* user;
        bool isES;  // OpenGL ES rather than desktop OpenGL
    };
    /// Loads the functions of the context `host` reaches, which must be
    /// current. Null, with the reason logged, when the driver lacks any.
    static std::unique_ptr<GLContext> Adopt(const Host& host);

    ~GLContext();

    GLContext(const GLContext&) = delete;
    GLContext& operator=(const GLContext&) = delete;

    /// Binds the context to the calling thread. Cheap when it already is.
    /// False once the context is lost.
    bool MakeCurrent();

    /// The context's owner destroyed it (a window closing). Everything made
    /// in it went with it; holders stop calling GL and just let go.
    void MarkLost() { m_Lost = true; }
    bool IsLost() const { return m_Lost; }

    /// OpenGL ES, rather than desktop OpenGL: shaders say "#version 300 es"
    /// there and "#version 330 core" on a desktop context.
    bool IsES() const { return m_IsES; }

    const GL& Functions() const { return m_GL; }

    struct Native;  // the platform's handles (GLApi.cpp)

private:
    GLContext() = default;
    /// Loads every entry point through `load`. False if any is missing.
    bool LoadFunctions(void* (*load)(const char* name, void* user), void* user);

    GL m_GL;
    bool m_IsES = false;
    bool m_Lost = false;
    Native* m_Native = nullptr;
    Host m_Host{};  // set for an adopted context
};

/// The context every GL user in the program draws in: the window's, once a
/// display has registered one, otherwise an off-screen one made on the first
/// call. Null where there is no OpenGL. Users ask once and keep the answer:
/// what they made lives in that context. Shared, so a context its owner has
/// destroyed (IsLost) still answers until its last user lets go.
DEKI_RENDERING_API std::shared_ptr<GLContext> SharedContext();

/// Makes `context` the window context SharedContext hands out, or forgets it
/// with null. A display registers before anything draws, so everyone shares
/// its context; users who already took an off-screen one keep it.
DEKI_RENDERING_API void SetWindowContext(std::shared_ptr<GLContext> context);

}  // namespace DekiGL

#endif  // DEKI_CAPABILITY_OPENGL
