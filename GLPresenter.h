#pragma once

// Only where the platform has OpenGL: the build defines DEKI_CAPABILITY_OPENGL
// for one whose platform provides "opengl" (the desktop, a Linux handheld) and
// never for a microcontroller, where this file compiles to nothing.
#ifdef DEKI_CAPABILITY_OPENGL

// Builds each frame on the GPU and shows it in a window, for a display that
// presents through OpenGL.
//
// The 2D renderer's blits are drawn here too, straight into the frame
// (GLBlitSink, QuadBlit's sink), unless that is turned off (SetDraws2D). The
// frame buffer then only collects what code writes into it directly. What
// reaches the frame, and how:
//
//   2D          each blit a quad, in draw order, its source a texture kept
//               between frames; the frame starts from the camera's clear
//   PushLayer   what a buffer holds goes up as a texture and over the frame
//               being built, premultiplied, the way the 2D blitter leaves a
//               cleared RGB565A8 buffer: 2D drawn on the CPU, or what code
//               wrote into the buffer directly
//   BindFrame   3D draws straight into the frame between two layers (a GPU
//               mesh backend does this when it starts a batch, then clears
//               the CPU buffer to transparent for the 2D that follows)
//   Present     the last layer, then the frame to the window, scaled by whole
//               pixels and centred, through the scene's ScreenEffect when it
//               has one, and the swap
//
// With 2D on the CPU, a frame of 2D under 3D under 2D is two uploads and no
// readback; with no 3D, one upload, and the presenter is just a faster way to
// show the buffer. With 2D here too, the CPU draws no pixels at all.
//
// Layers need a buffer that keeps coverage, so a display that presents
// through here reports RGB565A8. Any other format still presents, opaque.

#include <deki/Engine.h>  // Deki::ColorFormat
#include <deki/Vector.h>

#include "DekiRenderingPackage.h"
#include "GLApi.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace DekiRendering
{
class Lighting2D;
class ShaderAsset;
}

namespace DekiGL
{

class GLBlitSink;

class DEKI_RENDERING_API GLPresenter
{
public:
    /// What the presenter needs from the window it shows frames in.
    struct Window
    {
        void (*swap)(void* user);                        // shows the back buffer
        void (*drawableSize)(void* user, int* w, int* h);  // in pixels, not points
        void* user;
        // The framebuffer the window is: 0, the default one, for a real
        // window; another to present into a texture.
        GLuint target = 0;
    };

    /// A full-screen shader to show a frame through: its Effect(), with what
    /// it reads as uParams and uColor (0xAABBGGRR). The scene's ScreenEffect
    /// is one; code can pass its own.
    struct Effect
    {
        const DekiRendering::ShaderAsset* shader = nullptr;
        Deki::Vector3 params;
        uint32_t color = 0xFFFFFFFFu;
    };

    /// A presenter over `context`, a window's, which must be current. It
    /// becomes the program's shared context (SharedContext) and the presenter
    /// the active one. Null, with the reason logged, when GL fails.
    static std::unique_ptr<GLPresenter> Create(std::shared_ptr<GLContext> context, const Window& window);
    ~GLPresenter();

    GLPresenter(const GLPresenter&) = delete;
    GLPresenter& operator=(const GLPresenter&) = delete;

    /// The presenter showing frames, if a display presents through one.
    static GLPresenter* Active();

    /// Whether 2D drawn into a buffer like this can be composed in layers:
    /// RGB565A8, which keeps coverage, at the frame's size. A presenter whose
    /// frame has not started yet takes any size.
    bool TakesLayers(int width, int height, Deki::ColorFormat format) const;

    /// Composes what `buffer` holds over the frame being built, starting the
    /// frame if this is its first layer. A buffer that has nothing in it, all
    /// transparent, costs the scan and nothing else.
    void PushLayer(const uint8_t* buffer, int width, int height, Deki::ColorFormat format);

    /// Binds the frame being built for drawing into, the viewport over all of
    /// it. Starts the frame if nothing has yet. Its depth buffer is the
    /// caller's to clear.
    void BindFrame(int width, int height);

    /// Composes `buffer` as the last layer, shows the frame (through the
    /// scene's ScreenEffect, if it has one) and swaps.
    void Present(const uint8_t* buffer, int width, int height, Deki::ColorFormat format);
    /// As above, through `effect` whatever the scene has; a null shader shows
    /// the frame as it is.
    void Present(const uint8_t* buffer, int width, int height, Deki::ColorFormat format, const Effect& effect);

    /// The frame built so far, top row first, as RGBA8: what Present would
    /// show. For screenshots and tests; it waits for the GPU.
    bool ReadFrame(std::vector<uint8_t>& rgba);
    int FrameWidth() const { return m_Width; }
    int FrameHeight() const { return m_Height; }

    GLContext& Context() { return *m_Context; }

    /// Whether the 2D renderer's blits are drawn here, into the frame (on by
    /// default, where the 2D shader builds), or on the CPU and composed as a
    /// layer. Takes effect from the next frame.
    void SetDraws2D(bool draws);
    bool Draws2D() const;

    /// What drawing 2D here has cost since the presenter was made. For tests
    /// and benchmarks.
    struct DrawStats
    {
        uint32_t blits = 0;     // quads drawn
        uint32_t draws = 0;     // draw calls they took
        uint32_t uploads = 0;   // sources uploaded as textures
        uint32_t textures = 0;  // kept now
        size_t textureBytes = 0;
    };
    DrawStats Stats2D() const;

private:
    friend class GLBlitSink;
    GLPresenter(std::shared_ptr<GLContext> context, const Window& window);
    bool Build();
    /// Draws the frame into the window's rectangle through `effect`'s
    /// shader. False when the shader does not build: copy it instead.
    bool DrawEffect(const Effect& effect, int x, int y, int width, int height);
    struct EffectProgram
    {
        GLuint id = 0;
        GLint frame = -1, dst = -1, resolution = -1, time = -1, params = -1, color = -1;
    };
    EffectProgram LinkEffect(const DekiRendering::ShaderAsset& shader);
    GLuint Compile(GLenum type, const std::string& source, const char* what);
    bool EnsureFrame(int width, int height);
    void StartFrame(int width, int height);
    void ReleaseFrame();
    /// Starts the frame, or starts it over, filled with one opaque colour.
    void ClearFrame(int width, int height, uint8_t r, uint8_t g, uint8_t b);
    /// Draws the 2D queued so far: before anything else touches the frame.
    void Flush2D();
    /// Lights the frame built so far with `lighting`, worked out per pixel
    /// (GLLight2DShader.inl). False when its program does not build.
    bool DrawLight2D(const DekiRendering::Lighting2D& lighting);
    bool BuildLight2D();

    std::shared_ptr<GLContext> m_Context;
    Window m_Window;

    GLuint m_Program = 0;
    GLint m_LayerUniform = -1;
    GLint m_HeightUniform = -1;
    GLuint m_VertexArray = 0;
    GLuint m_LayerTexture = 0;

    GLuint m_Framebuffer = 0;
    GLuint m_ColorTexture = 0;  // a texture, so an effect can sample the frame
    GLuint m_DepthBuffer = 0;
    int m_Width = 0;   // of the frame (and the layer texture)
    int m_Height = 0;
    bool m_InFrame = false;  // a frame is being built
    bool m_ErrorReported = false;

    std::vector<uint8_t> m_Staging;  // a layer's rows as RGBA8, premultiplied

    // Effect programs by shader revision; one that did not build keeps id 0.
    std::unordered_map<uint32_t, EffectProgram> m_Effects;

    // Draws the 2D renderer's blits; null where its shader does not build.
    std::unique_ptr<GLBlitSink> m_Blits;

    // The 2D light's program, built the first time a frame is lit.
    static constexpr int kMaxLights2D = 16;
    struct Light2DProgram
    {
        GLuint id = 0;
        GLint a[kMaxLights2D], b[kMaxLights2D], c[kMaxLights2D];
        GLint count = -1, ambient = -1, height = -1;
        bool tried = false;
    };
    Light2DProgram m_Light2D;
    bool m_TooManyLightsReported = false;
};

}  // namespace DekiGL

#endif  // DEKI_CAPABILITY_OPENGL
