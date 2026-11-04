#pragma once

// Only where the platform has OpenGL: the build defines DEKI_CAPABILITY_OPENGL
// for one whose platform provides "opengl" (the desktop, a Linux handheld) and
// never for a microcontroller, where this file compiles to nothing.
#ifdef DEKI_CAPABILITY_OPENGL

// The 2D renderer's blits, drawn straight into the frame a GLPresenter builds.
//
// QuadBlit hands every blit of a frame here instead of drawing it on the CPU
// (QuadBlit::BlitSink), worked out as far as the CPU works it out: the
// clipped rectangle it covers and the 16.16 steps it samples the source with.
// Each becomes a quad whose fragment shader takes those same steps in
// integers and runs the CPU's pixel pipeline, so the GPU picks the source
// pixels the CPU picks and blends them the same way; only the CPU's rounding
// to 565 is missing. Quads queue in call order and draw in batches, one draw
// per run of quads from the same texture, whenever something else is about
// to touch the frame (the presenter flushes them first).
//
// Sources are uploaded as textures and kept: by Source::revision, or for a
// source that promises its pixels stay put (cacheScaled) until
// QuadBlit::ForgetScaled says they go. Anything else is uploaded again at
// every blit, which is always right. Textures not drawn for a while go.
//
// The frame's buffer is left empty: it collects only what code writes into
// it directly, and that is composed over the frame, as a layer, before the
// next quad draws, so it keeps its place in the draw order.

#include <deki/Engine.h>  // Deki::ColorFormat

#include "GLApi.h"
#include "GLPresenter.h"
#include "deki-rendering/QuadBlit.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace DekiGL
{

class GLBlitSink final : public QuadBlit::BlitSink
{
public:
    explicit GLBlitSink(GLPresenter& presenter);
    ~GLBlitSink() override = default;

    GLBlitSink(const GLBlitSink&) = delete;
    GLBlitSink& operator=(const GLBlitSink&) = delete;

    /// Builds the program and buffers, in the presenter's context, which must
    /// be current. False, with the reason logged, when they do not build.
    bool Build();
    /// Frees what Build and the draws made. The context must be current.
    void Release();

    // QuadBlit::BlitSink
    bool BeginFrame(uint8_t* target, int32_t width, int32_t height, Deki::ColorFormat format, uint8_t clearR,
                    uint8_t clearG, uint8_t clearB) override;
    bool Draw(const QuadBlit::SinkBlit& blit) override;
    void TargetWritten() override;
    void EndFrame() override;
    void Forget(const uint8_t* begin, size_t bytes) override;
    bool Light(const DekiRendering::Lighting2D& lighting) override;

    /// Draws the queued quads into the frame. The presenter calls it before
    /// anything else draws into the frame or reads it.
    void Flush();
    /// The presenter composed `buffer` as a layer: when it is the frame's
    /// buffer, what it held is in the frame now (its caller empties it).
    void LayerPushed(const uint8_t* buffer);

    void SetEnabled(bool enabled) { m_Enabled = enabled; }
    bool Enabled() const { return m_Enabled; }
    const GLPresenter::DrawStats& Stats() const { return m_Stats; }

private:
    // One corner of a quad. Every corner carries the whole blit, read flat.
    struct Vertex
    {
        int16_t x, y;       // the corner, in frame pixels from the top left
        int32_t origin[4];  // originX, originY, originSx, originSy
        int32_t step[4];    // dSxDx, dSyDx, dSxDy, dSyDy
        int16_t size[4];    // the source's width and height, then the sampled ones
        uint8_t tint[4];    // r, g, b, a
        uint8_t key[4];     // the chroma key's r, g, b, then the flags (kFlag*)
        uint8_t sample[4];  // the filter (Deki::TextureFilter), then the top mipmap level
    };

    // The pixels a texture holds: a source's, as QuadBlit reads them.
    struct Key
    {
        const uint8_t* pixels;
        int32_t width, height, stride, bytesPerPixel;
        bool isRGB565, hasAlpha;
        bool operator==(const Key& o) const
        {
            return pixels == o.pixels && width == o.width && height == o.height && stride == o.stride &&
                   bytesPerPixel == o.bytesPerPixel && isRGB565 == o.isRGB565 && hasAlpha == o.hasAlpha;
        }
    };
    struct KeyHash
    {
        size_t operator()(const Key& k) const;
    };
    struct Texture
    {
        GLuint id = 0;
        int32_t width = 0, height = 0;  // as allocated
        uint32_t revision = 0;          // the source's when uploaded; 0 for none
        bool stable = false;            // uploaded for a cacheScaled source, not forgotten since
        uint8_t mipLevels = 0;          // levels above 0 made for it (a Trilinear source), 0 for none
        size_t mipBytes = 0;            // what those levels take
        uint32_t lastUsed = 0;          // m_Frame
        uint32_t batch = 0;             // m_Batch it was last queued in
    };
    // Consecutive quads drawn from one texture: one draw.
    struct Run
    {
        GLuint texture;
        uint32_t first;  // quad
        uint32_t count;
    };

    /// The texture with `source`'s pixels, uploaded now if what it holds may
    /// not be them. 0 when there is none.
    // The texture holding `source`'s pixels, and how many mipmap levels it
    // has (made for a Trilinear source).
    GLuint TextureFor(const QuadBlit::Source& source, uint8_t& mipLevels);
    void MakeMipmaps(Texture& texture);
    void Upload(Texture& texture, const QuadBlit::Source& source);
    void Evict();
    void DeleteTexture(Texture& texture);

    GLPresenter& m_Presenter;
    bool m_Enabled = true;
    int m_LoggedMode = 0;  // the path last logged: 1 on the GPU, 2 on the CPU

    GLuint m_Program = 0;
    GLint m_FrameUniform = -1;
    GLint m_HeightUniform = -1;
    GLint m_TextureUniform = -1;
    GLuint m_VertexArray = 0;
    GLuint m_VertexBuffer = 0;
    GLuint m_IndexBuffer = 0;
    GLint m_MaxTextureSize = 2048;  // what OpenGL ES 3.0 promises at least

    // The frame being drawn.
    uint8_t* m_Target = nullptr;
    int32_t m_Width = 0;
    int32_t m_Height = 0;
    Deki::ColorFormat m_Format = Deki::ColorFormat::RGB565A8;
    bool m_Written = false;  // the target holds something to compose before the next quad

    std::vector<Vertex> m_Vertices;  // four a quad
    std::vector<Run> m_Runs;
    uint32_t m_Batch = 1;  // one more each Flush

    std::unordered_map<Key, Texture, KeyHash> m_Textures;
    Texture m_Scratch;  // for pixels handed over (Source::ownsPixels)
    size_t m_TextureBytes = 0;
    uint32_t m_Frame = 0;
    std::vector<uint8_t> m_Staging;  // an upload's pixels as RGBA8
    bool m_ErrorReported = false;

    GLPresenter::DrawStats m_Stats;
};

}  // namespace DekiGL

#endif  // DEKI_CAPABILITY_OPENGL
