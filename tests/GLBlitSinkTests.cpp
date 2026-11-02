// Tests that the presenter draws the 2D renderer's blits (GLBlitSink) as
// QuadBlit's CPU code draws them into an RGB565A8 frame buffer: the same
// blits are drawn both ways and the pictures compared pixel by pixel, the
// GPU's read back from the frame it built. They may differ only by the CPU's
// rounding of each write to 565 (up to 7 a channel, 8 with the blend's own
// rounding), for every source layout, at 1:1, scaled, rotated, flipped,
// tinted, keyed, dithered and clipped.
//
// Also: sources are uploaded once and kept, a buffer baked again shows its new
// pixels, what code writes into the frame buffer directly keeps its place in
// the draw order, and unused textures go. On a machine with no OpenGL the
// tests skip.

// Only where the platform has OpenGL (DEKI_CAPABILITY_OPENGL); a build
// without it, as for a board, has none of the code under test.
#ifdef DEKI_CAPABILITY_OPENGL

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "deki-rendering/GLApi.h"
#include "deki-rendering/GLPresenter.h"
#include "deki-rendering/PixelFormat.h"
#include "deki-rendering/QuadBlit.h"

using namespace DekiGL;

namespace
{

constexpr int kW = 64;
constexpr int kH = 48;
constexpr Deki::ColorFormat kFormat = Deki::ColorFormat::RGB565A8;
constexpr uint8_t kClear[3] = { 40, 60, 90 };
// The CPU rounds each write to 565: 7 at most in red and blue, and one more
// for the GPU blend's rounding. A blend over a pixel the CPU already rounded
// from a blend rounds twice.
constexpr int kTolerance = 8;
constexpr int kTwoRoundings = 16;

std::unique_ptr<GLPresenter> MakePresenter(int width = kW, int height = kH)
{
    std::shared_ptr<GLContext> context = SharedContext();
    if (!context)
    {
        return nullptr;
    }
    static int s_W, s_H;
    s_W = width;
    s_H = height;
    GLPresenter::Window window{ [](void*) {},
                                [](void*, int* w, int* h)
                                {
                                    *w = s_W;
                                    *h = s_H;
                                },
                                nullptr };
    return GLPresenter::Create(context, window);
}

#define REQUIRE_PRESENTER(p)                                                                                           \
    std::unique_ptr<GLPresenter> p = MakePresenter();                                                                  \
    if (!p)                                                                                                            \
    {                                                                                                                  \
        GTEST_SKIP() << "no OpenGL on this machine";                                                                   \
    }                                                                                                                  \
    ASSERT_TRUE(p->Draws2D()) << "the 2D shader does not build"

/// An RGB565A8 frame buffer as the render system's clear leaves it.
std::vector<uint8_t> Cleared(int w = kW, int h = kH)
{
    std::vector<uint8_t> buffer(Deki::FrameBufferBytes(kFormat, w, h));
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
    {
        DekiPixel::WriteDstPixel<kFormat>(buffer.data(), i, kClear[0], kClear[1], kClear[2], 255);
    }
    return buffer;
}

/// A source in one of QuadBlit's layouts, with colours and alphas of every
/// kind: 0, partial and opaque.
struct TestSource
{
    std::vector<uint8_t> pixels;
    QuadBlit::Source source;

    void Fill(uint32_t seed)
    {
        const QuadBlit::Source& s = source;
        for (int y = 0; y < s.height; ++y)
        {
            for (int x = 0; x < s.width; ++x)
            {
                const uint32_t n = static_cast<uint32_t>(x * 37 + y * 101) + seed * 13;
                const uint8_t r = static_cast<uint8_t>(n * 7), g = static_cast<uint8_t>(n * 3 + 90),
                              b = static_cast<uint8_t>(n * 11 + 30);
                const int k = (x + 2 * y + static_cast<int>(seed)) % 7;
                const uint8_t a = k == 0 ? 0 : k == 1 ? 96 : k == 2 ? 200 : 255;
                uint8_t* p = &pixels[(static_cast<size_t>(y) * s.width + x) * s.bytesPerPixel];
                if (s.isRGB565)
                {
                    DekiPixel::Store565(p, DekiPixel::PackRGB565(r, g, b));
                    if (s.bytesPerPixel == 3)
                    {
                        p[2] = a;
                    }
                }
                else if (s.bytesPerPixel == 1)
                {
                    p[0] = a;
                }
                else
                {
                    p[0] = r;
                    p[1] = g;
                    p[2] = b;
                    if (s.bytesPerPixel == 4)
                    {
                        p[3] = a;
                    }
                }
            }
        }
    }
};

std::unique_ptr<TestSource> MakeTestSource(QuadBlit::PixelLayout layout, int w, int h, uint32_t seed)
{
    auto t = std::make_unique<TestSource>();
    t->pixels.resize(static_cast<size_t>(w) * h * layout.bytesPerPixel);
    t->source = QuadBlit::MakeSource(t->pixels.data(), w, h, layout);
    t->Fill(seed);
    return t;
}

struct Layout
{
    const char* name;
    QuadBlit::PixelLayout layout;
};
const Layout kLayouts[] = {
    { "RGB565", QuadBlit::PixelLayout::RGB565() },
    { "RGB565A8", QuadBlit::PixelLayout::RGB565A8() },
    { "RGB565A8NoAlpha", QuadBlit::PixelLayout::RGB565A8NoAlpha() },
    { "RGBA8888", QuadBlit::PixelLayout::RGBA8888() },
    { "RGB888", QuadBlit::PixelLayout::RGB888() },
    { "Alpha8", QuadBlit::PixelLayout::Alpha8() },
};

void Blit(const QuadBlit::Source& s, uint8_t* target, int x, int y, float sx, float sy, float rotation, float px,
          float py, uint8_t r = 255, uint8_t g = 255, uint8_t b = 255, uint8_t a = 255, bool dither = false)
{
    QuadBlit::Blit(s, target, kW, kH, kFormat, x, y, sx, sy, rotation, px, py, r, g, b, a, dither);
}

/// Draws a frame through the presenter: `draw` with the sink, then what the
/// buffer still holds as the last layer, as Present composes it. The frame,
/// read back, top row first.
template <typename Draw>
std::vector<uint8_t> DrawOnGpu(GLPresenter& presenter, Draw draw)
{
    std::vector<uint8_t> buffer(Deki::FrameBufferBytes(kFormat, kW, kH), 0x5A);
    QuadBlit::ClearClipStack();
    EXPECT_TRUE(QuadBlit::BeginSinkFrame(buffer.data(), kW, kH, kFormat, kClear[0], kClear[1], kClear[2]));
    draw(buffer.data());
    QuadBlit::EndSinkFrame();
    presenter.PushLayer(buffer.data(), kW, kH, kFormat);
    std::vector<uint8_t> frame;
    EXPECT_TRUE(presenter.ReadFrame(frame));
    QuadBlit::ClearClipStack();
    return frame;
}

template <typename Draw>
std::vector<uint8_t> DrawOnCpu(Draw draw)
{
    std::vector<uint8_t> buffer = Cleared();
    QuadBlit::ClearClipStack();
    draw(buffer.data());
    QuadBlit::ClearClipStack();
    return buffer;
}

/// Pixels further apart than the tolerance in any channel, of an RGB565A8
/// buffer and an RGBA8 frame; and how many pixels the CPU drew at all.
struct Comparison
{
    int drawn = 0;
    int mismatched = 0;
    std::string first;  // where the first mismatch is, and the two colours
};

Comparison Compare(const std::vector<uint8_t>& cpu, const std::vector<uint8_t>& frame, int tolerance = kTolerance)
{
    Comparison c;
    for (int i = 0; i < kW * kH; ++i)
    {
        uint8_t r, g, b, a;
        DekiPixel::ReadDstPixel<kFormat>(cpu.data(), static_cast<size_t>(i), r, g, b, a);
        const uint8_t* p = &frame[static_cast<size_t>(i) * 4];
        c.drawn += r != (kClear[0] & 0xF8) || g != (kClear[1] & 0xFC) || b != (kClear[2] & 0xF8);
        if (std::abs(p[0] - r) > tolerance || std::abs(p[1] - g) > tolerance || std::abs(p[2] - b) > tolerance)
        {
            if (c.mismatched++ == 0)
            {
                char text[128];
                std::snprintf(text, sizeof(text), "(%d, %d): CPU %d %d %d, GPU %d %d %d", i % kW, i / kW, r, g, b, p[0],
                              p[1], p[2]);
                c.first = text;
            }
        }
    }
    return c;
}

template <typename Draw>
void ExpectGpuMatchesCpu(GLPresenter& presenter, const std::string& what, Draw draw, int tolerance = kTolerance)
{
    SCOPED_TRACE(what);
    const std::vector<uint8_t> cpu = DrawOnCpu(draw);
    const std::vector<uint8_t> frame = DrawOnGpu(presenter, draw);
    ASSERT_EQ(frame.size(), static_cast<size_t>(kW) * kH * 4);
    const Comparison c = Compare(cpu, frame, tolerance);
    EXPECT_GT(c.drawn, 0) << "the case draws nothing";
    EXPECT_EQ(c.mismatched, 0) << "first at " << c.first;
}

}  // namespace

TEST(GLBlitSink, EveryLayoutAtEveryScaleAndRotation)
{
    REQUIRE_PRESENTER(presenter);
    for (const Layout& l : kLayouts)
    {
        auto t = MakeTestSource(l.layout, 13, 11, 1);
        const QuadBlit::Source& s = t->source;
        const std::string name = l.name;
        ExpectGpuMatchesCpu(*presenter, name + " 1:1", [&](uint8_t* fb) { Blit(s, fb, 5, 7, 1, 1, 0, 0, 0); });
        ExpectGpuMatchesCpu(*presenter, name + " off the edges",
                            [&](uint8_t* fb)
                            {
                                Blit(s, fb, -4, kH - 6, 1, 1, 0, 0, 0);
                                Blit(s, fb, kW - 5, -3, 1, 1, 0, 0, 0);
                            });
        ExpectGpuMatchesCpu(*presenter, name + " scaled up",
                            [&](uint8_t* fb) { Blit(s, fb, 30, 22, 3.3f, 2.7f, 0, 0.5f, 0.5f); });
        ExpectGpuMatchesCpu(*presenter, name + " scaled down",
                            [&](uint8_t* fb) { Blit(s, fb, 9, 9, 0.6f, 0.45f, 0, 0, 0); });
        ExpectGpuMatchesCpu(*presenter, name + " rotated",
                            [&](uint8_t* fb) { Blit(s, fb, 32, 24, 2.7f, 2.3f, 0.7f, 0.5f, 0.5f); });
        ExpectGpuMatchesCpu(*presenter, name + " rotated about a corner",
                            [&](uint8_t* fb) { Blit(s, fb, 4, 6, 2.0f, 2.0f, -2.4f, 0.2f, 0.8f); });
        ExpectGpuMatchesCpu(*presenter, name + " tinted",
                            [&](uint8_t* fb) { Blit(s, fb, 10, 6, 2.5f, 2.5f, 0, 0, 0, 200, 120, 60); });
        ExpectGpuMatchesCpu(*presenter, name + " alpha tint",
                            [&](uint8_t* fb) { Blit(s, fb, 10, 6, 2.5f, 2.5f, 0, 0, 0, 255, 255, 255, 140); });
        ExpectGpuMatchesCpu(*presenter, name + " both tints, rotated",
                            [&](uint8_t* fb) { Blit(s, fb, 32, 24, 2.2f, 2.4f, 2.1f, 0.5f, 0.5f, 90, 255, 160, 170); });
        ExpectGpuMatchesCpu(*presenter, name + " ordered dither",
                            [&](uint8_t* fb) { Blit(s, fb, 4, 4, 3.0f, 3.0f, 0, 0, 0, 255, 255, 255, 170, true); });
        ExpectGpuMatchesCpu(*presenter, name + " ordered dither, rotated", [&](uint8_t* fb)
                            { Blit(s, fb, 32, 24, 2.5f, 2.5f, 0.4f, 0.5f, 0.5f, 255, 255, 255, 255, true); });
        ExpectGpuMatchesCpu(*presenter, name + " BlitScaled", [&](uint8_t* fb)
                            { QuadBlit::BlitScaled(s, fb, kW, kH, kFormat, 3, 2, 51, 27, 255, 200, 255, 230); });
    }
}

TEST(GLBlitSink, FlipsKeysTilesAndStretchedStrips)
{
    REQUIRE_PRESENTER(presenter);
    auto square = MakeTestSource(QuadBlit::PixelLayout::RGB565A8(), 12, 12, 2);
    for (int flips = 1; flips < 8; ++flips)
    {
        QuadBlit::Source s = square->source;
        s.flipH = (flips & 1) != 0;
        s.flipV = (flips & 2) != 0;
        s.flipD = (flips & 4) != 0;
        const std::string name = std::string(s.flipH ? "H" : "") + (s.flipV ? "V" : "") + (s.flipD ? "D" : "");
        ExpectGpuMatchesCpu(*presenter, "flipped " + name, [&](uint8_t* fb) { Blit(s, fb, 3, 3, 1, 1, 0, 0, 0); });
        ExpectGpuMatchesCpu(*presenter, "flipped " + name + ", scaled",
                            [&](uint8_t* fb) { Blit(s, fb, 20, 4, 3.3f, 2.6f, 0, 0, 0); });
        ExpectGpuMatchesCpu(*presenter, "flipped " + name + ", rotated",
                            [&](uint8_t* fb) { Blit(s, fb, 32, 24, 2.5f, 2.5f, 1.1f, 0.5f, 0.5f); });
    }

    // A chroma key, on a 565 source (quantised, as the loaders make it) and
    // an 888 one, compared before the tint.
    auto rgb565 = MakeTestSource(QuadBlit::PixelLayout::RGB565(), 13, 11, 3);
    QuadBlit::Source keyed = rgb565->source;
    keyed.hasChromaKey = true;
    DekiPixel::UnpackRGB565(DekiPixel::Load565(rgb565->pixels.data() + 2 * 5), keyed.keyR, keyed.keyG, keyed.keyB);
    ExpectGpuMatchesCpu(*presenter, "chroma key", [&](uint8_t* fb) { Blit(keyed, fb, 4, 4, 1, 1, 0, 0, 0); });
    ExpectGpuMatchesCpu(*presenter, "chroma key, scaled and tinted",
                        [&](uint8_t* fb) { Blit(keyed, fb, 4, 4, 3.2f, 3.5f, 0, 0, 0, 100, 200, 255); });
    auto rgb888 = MakeTestSource(QuadBlit::PixelLayout::RGB888(), 13, 11, 4);
    QuadBlit::Source keyed888 = rgb888->source;
    keyed888.hasChromaKey = true;
    keyed888.keyR = rgb888->pixels[3 * 7];
    keyed888.keyG = rgb888->pixels[3 * 7 + 1];
    keyed888.keyB = rgb888->pixels[3 * 7 + 2];
    ExpectGpuMatchesCpu(*presenter, "chroma key 888, rotated",
                        [&](uint8_t* fb) { Blit(keyed888, fb, 32, 24, 3.0f, 3.0f, 0.3f, 0.5f, 0.5f); });

    // A tile: a sub-rect of an atlas, through its stride.
    auto atlas = MakeTestSource(QuadBlit::PixelLayout::RGBA8888(), 20, 16, 5);
    QuadBlit::Source tile = atlas->source;
    tile.pixels = atlas->pixels.data() + (3 * 20 + 4) * 4;
    tile.width = 9;
    tile.height = 7;
    tile.stride = 20 * 4;
    ExpectGpuMatchesCpu(*presenter, "a tile, scaled", [&](uint8_t* fb) { Blit(tile, fb, 2, 2, 4.0f, 4.0f, 0, 0, 0); });
    ExpectGpuMatchesCpu(*presenter, "a tile, rotated",
                        [&](uint8_t* fb) { Blit(tile, fb, 32, 24, 3, 3, 0.8f, 0.5f, 0.5f); });

    // One column, or one row, stretched over a rectangle: a baked gradient.
    auto column = MakeTestSource(QuadBlit::PixelLayout::RGB565(), 1, 9, 6);
    QuadBlit::Source strip = column->source;
    strip.stretchWidth = 14;
    ExpectGpuMatchesCpu(*presenter, "a stretched column", [&](uint8_t* fb) { Blit(strip, fb, 3, 3, 3, 3, 0, 0, 0); });
    ExpectGpuMatchesCpu(*presenter, "a stretched column, rotated",
                        [&](uint8_t* fb) { Blit(strip, fb, 32, 24, 2.5f, 2.5f, 0.5f, 0.5f, 0.5f); });
    auto row = MakeTestSource(QuadBlit::PixelLayout::RGB565(), 9, 1, 7);
    QuadBlit::Source stripRow = row->source;
    stripRow.stretchHeight = 11;
    ExpectGpuMatchesCpu(*presenter, "a stretched row, rotated",
                        [&](uint8_t* fb) { Blit(stripRow, fb, 32, 24, 3, 3, -0.6f, 0.5f, 0.5f); });
}

TEST(GLBlitSink, ClipRectsAndManyBlitsInOrder)
{
    REQUIRE_PRESENTER(presenter);
    // Opaque 565 underneath, so what blends over it starts from the same
    // colours on both sides.
    auto under = MakeTestSource(QuadBlit::PixelLayout::RGB565(), 16, 12, 8);
    auto a = MakeTestSource(QuadBlit::PixelLayout::RGB565A8(), 13, 11, 9);
    auto b = MakeTestSource(QuadBlit::PixelLayout::Alpha8(), 10, 10, 10);
    ExpectGpuMatchesCpu(*presenter, "clipped",
                        [&](uint8_t* fb)
                        {
                            QuadBlit::PushClipRect(10, 8, 40, 30);
                            Blit(under->source, fb, 0, 0, 4, 4, 0, 0, 0);
                            QuadBlit::PushClipRect(20, 12, 60, 44);  // within the first
                            Blit(a->source, fb, 32, 24, 3, 3, 0.9f, 0.5f, 0.5f);
                            QuadBlit::PopClipRect();
                            QuadBlit::SetClipEnabled(false);
                            Blit(b->source, fb, 40, 2, 1, 1, 0, 0, 0, 255, 0, 0);
                            QuadBlit::SetClipEnabled(true);
                            QuadBlit::PopClipRect();
                        });
    // Overlapping, each over the last: the order shows. Translucent over
    // translucent, so the CPU rounds twice where they meet.
    auto pile = [&](bool reversed)
    {
        return [&, reversed](uint8_t* fb)
        {
            Blit(under->source, fb, 0, 0, 4, 4, 0, 0, 0);
            for (int k = 0; k < 12; ++k)
            {
                const int i = reversed ? 11 - k : k;
                const QuadBlit::Source& s = (i % 2) ? a->source : b->source;
                Blit(s, fb, 8 + (i * 13) % 48, 8 + (i * 7) % 32, 2.0f + (i % 2), 2.0f, 0.0f, 0.5f, 0.5f, 255,
                     static_cast<uint8_t>(80 + i * 14), 255);
            }
        };
    };
    ExpectGpuMatchesCpu(*presenter, "a pile, in order", pile(false), kTwoRoundings);
    const Comparison reversed = Compare(DrawOnCpu(pile(true)), DrawOnGpu(*presenter, pile(false)), kTwoRoundings);
    EXPECT_GT(reversed.mismatched, 100) << "the pile looks the same in either order, so it shows no order";
}

// A source is uploaded when it is first drawn and kept while its revision
// says it has not changed, or while it promises it stays put (cacheScaled).
// The same buffer baked again shows its new pixels, whatever said so: a new
// revision, ForgetScaled before the old pixels went, or no promise at all.
TEST(GLBlitSink, TexturesAreKeptAndNeverStale)
{
    REQUIRE_PRESENTER(presenter);
    auto t = MakeTestSource(QuadBlit::PixelLayout::RGB565A8(), 13, 11, 11);
    QuadBlit::Source& s = t->source;
    auto draw = [&](uint8_t* fb) { Blit(s, fb, 6, 6, 3, 3, 0, 0, 0); };

    // Kept by revision.
    s.revision = QuadBlit::NextRevision();
    const uint32_t before = presenter->Stats2D().uploads;
    for (int frame = 0; frame < 3; ++frame)
    {
        ExpectGpuMatchesCpu(*presenter, "a revision, frame " + std::to_string(frame), draw);
    }
    EXPECT_EQ(presenter->Stats2D().uploads - before, 1u) << "uploaded once";

    // Baked again in place: a new revision shows the new pixels.
    t->Fill(12);
    s.revision = QuadBlit::NextRevision();
    ExpectGpuMatchesCpu(*presenter, "baked again, a new revision", draw);
    EXPECT_EQ(presenter->Stats2D().uploads - before, 2u);

    // A promise to stay put, broken only after ForgetScaled.
    s.revision = 0;
    s.cacheScaled = true;
    ExpectGpuMatchesCpu(*presenter, "cacheScaled", draw);
    ExpectGpuMatchesCpu(*presenter, "cacheScaled, again", draw);
    EXPECT_EQ(presenter->Stats2D().uploads - before, 3u);
    QuadBlit::ForgetScaled(t->pixels.data(), t->pixels.size());
    t->Fill(13);
    ExpectGpuMatchesCpu(*presenter, "cacheScaled, forgotten and baked again", draw);
    EXPECT_EQ(presenter->Stats2D().uploads - before, 4u);

    // No revision, no promise: every blit uploads, even twice in one frame
    // with the pixels changed in between.
    s.cacheScaled = false;
    auto twice = [&](uint8_t* fb)
    {
        t->Fill(14);
        Blit(s, fb, 2, 2, 2, 2, 0, 0, 0);
        t->Fill(15);
        Blit(s, fb, 34, 20, 2, 2, 0, 0, 0);
    };
    ExpectGpuMatchesCpu(*presenter, "changed in place within a frame", twice);
}

// What code writes into the frame buffer directly (a pass that sets pixels
// itself, told with NoteWritten) goes over everything drawn before it and
// under everything drawn after.
TEST(GLBlitSink, DirectWritesKeepTheirPlace)
{
    REQUIRE_PRESENTER(presenter);
    auto under = MakeTestSource(QuadBlit::PixelLayout::RGB565(), 16, 12, 16);
    auto over = MakeTestSource(QuadBlit::PixelLayout::RGB565A8(), 13, 11, 17);
    auto bar = [&](uint8_t* fb)
    {
        // A bar by hand: opaque, and half covered (premultiplied, as a
        // blit into a cleared RGB565A8 buffer leaves it).
        for (int y = 14; y < 30; ++y)
        {
            for (int x = 4; x < 60; ++x)
            {
                uint8_t r, g, b, a;
                DekiPixel::ReadDstPixel<kFormat>(fb, static_cast<size_t>(y) * kW + x, r, g, b, a);
                const bool half = x >= 32;
                if (half && QuadBlit::SinkTarget() == fb)
                {
                    DekiPixel::WriteDstPixel<kFormat>(fb, static_cast<size_t>(y) * kW + x, 0, 124, 0, 128);
                }
                else if (half)
                {
                    // On the CPU the blend happens in the buffer.
                    DekiPixel::WriteDstPixel<kFormat>(fb, static_cast<size_t>(y) * kW + x, DekiPixel::Div255(r * 127),
                                                      static_cast<uint8_t>(124 + DekiPixel::Div255(g * 127)),
                                                      DekiPixel::Div255(b * 127), 255);
                }
                else
                {
                    DekiPixel::WriteDstPixel<kFormat>(fb, static_cast<size_t>(y) * kW + x, 248, 248, 0, 255);
                }
            }
        }
        QuadBlit::NoteWritten(fb);
    };
    auto draw = [&](uint8_t* fb)
    {
        Blit(under->source, fb, 0, 0, 4, 4, 0, 0, 0);
        bar(fb);
        Blit(over->source, fb, 32, 24, 3, 3, 0.4f, 0.5f, 0.5f);
        // Written after the last blit: the display composes it as it
        // presents (here, DrawOnGpu's PushLayer).
        for (int x = 0; x < kW; ++x)
        {
            DekiPixel::WriteDstPixel<kFormat>(fb, static_cast<size_t>(kH - 2) * kW + x, 0, 0, 248, 255);
        }
        QuadBlit::NoteWritten(fb);
    };
    ExpectGpuMatchesCpu(*presenter, "a bar by hand between two blits", draw, kTwoRoundings);

    // The bar over the blit instead looks otherwise: the order shows.
    auto barLast = [&](uint8_t* fb)
    {
        Blit(under->source, fb, 0, 0, 4, 4, 0, 0, 0);
        Blit(over->source, fb, 32, 24, 3, 3, 0.4f, 0.5f, 0.5f);
        bar(fb);
    };
    const Comparison c = Compare(DrawOnCpu(barLast), DrawOnGpu(*presenter, draw), kTwoRoundings);
    EXPECT_GT(c.mismatched, 100);
}

TEST(GLBlitSink, TurnedOffItDrawsNothing)
{
    REQUIRE_PRESENTER(presenter);
    presenter->SetDraws2D(false);
    EXPECT_FALSE(presenter->Draws2D());
    std::vector<uint8_t> buffer = Cleared();
    EXPECT_FALSE(QuadBlit::BeginSinkFrame(buffer.data(), kW, kH, kFormat, 0, 0, 0));
    EXPECT_EQ(buffer, Cleared()) << "the CPU's buffer is left alone";
    presenter->SetDraws2D(true);
    EXPECT_TRUE(QuadBlit::BeginSinkFrame(buffer.data(), kW, kH, kFormat, 0, 0, 0));
    QuadBlit::EndSinkFrame();

    // A buffer the frame cannot take as layers is declined too.
    std::vector<uint8_t> rgb565(Deki::FrameBufferBytes(Deki::ColorFormat::RGB565, kW, kH));
    EXPECT_FALSE(QuadBlit::BeginSinkFrame(rgb565.data(), kW, kH, Deki::ColorFormat::RGB565, 0, 0, 0));

    // Gone, the presenter takes its sink with it.
    presenter.reset();
    EXPECT_EQ(QuadBlit::GetBlitSink(), nullptr);
}

TEST(GLBlitSink, UnusedTexturesGo)
{
    REQUIRE_PRESENTER(presenter);
    auto t = MakeTestSource(QuadBlit::PixelLayout::RGB565(), 8, 8, 18);
    t->source.revision = QuadBlit::NextRevision();
    std::vector<uint8_t> buffer(Deki::FrameBufferBytes(kFormat, kW, kH));
    ASSERT_TRUE(QuadBlit::BeginSinkFrame(buffer.data(), kW, kH, kFormat, 0, 0, 0));
    Blit(t->source, buffer.data(), 1, 1, 1, 1, 0, 0, 0);
    QuadBlit::EndSinkFrame();
    EXPECT_EQ(presenter->Stats2D().textures, 1u);
    for (int frame = 0; frame < 200; ++frame)
    {
        ASSERT_TRUE(QuadBlit::BeginSinkFrame(buffer.data(), kW, kH, kFormat, 0, 0, 0));
        QuadBlit::EndSinkFrame();
    }
    EXPECT_EQ(presenter->Stats2D().textures, 0u);
    EXPECT_EQ(presenter->Stats2D().textureBytes, 0u);
}

// Not a check: how long a frame of sprites and text takes on the CPU, into
// the RGB565A8 buffer the presenter then uploads, and drawn by the sink, at a
// handheld's resolution. Run it with --gtest_also_run_disabled_tests.
TEST(GLBlitSink, DISABLED_BenchmarkAgainstTheCpu)
{
    constexpr int kWidth = 640, kHeight = 480, kFrames = 60;
    std::unique_ptr<GLPresenter> presenter = MakePresenter(kWidth, kHeight);
    if (!presenter)
    {
        GTEST_SKIP() << "no OpenGL on this machine";
    }
    auto sprite = MakeTestSource(QuadBlit::PixelLayout::RGB565A8(), 16, 16, 19);
    sprite->source.revision = QuadBlit::NextRevision();
    auto background = MakeTestSource(QuadBlit::PixelLayout::RGB565(), 1, 480, 20);  // a gradient strip
    background->source.stretchWidth = 640;
    background->source.revision = QuadBlit::NextRevision();
    auto text = MakeTestSource(QuadBlit::PixelLayout::Alpha8(), 180, 20, 21);
    text->source.revision = QuadBlit::NextRevision();

    std::vector<uint8_t> buffer(Deki::FrameBufferBytes(kFormat, kWidth, kHeight));
    for (int sprites : { 100, 500, 2000 })
    {
        auto drawScene = [&](uint8_t* fb)
        {
            QuadBlit::Blit(background->source, fb, kWidth, kHeight, kFormat, 0, 0, 1, 1, 0, 0, 0);
            for (int i = 0; i < sprites; ++i)
            {
                const float scale = (i % 4 == 0) ? 2.0f : 1.0f;
                const float rotation = (i % 5 == 0) ? 0.3f * static_cast<float>(i % 7) : 0.0f;
                QuadBlit::Blit(sprite->source, fb, kWidth, kHeight, kFormat, (i * 37) % kWidth, (i * 53) % kHeight,
                               scale, scale, rotation, 0.5f, 0.5f);
            }
            for (int i = 0; i < 20; ++i)
            {
                QuadBlit::Blit(text->source, fb, kWidth, kHeight, kFormat, 20 + (i % 3) * 200, 10 + i * 23, 1, 1, 0, 0,
                               0, 250, 250, 210);
            }
        };
        auto time = [&](bool gpu)
        {
            const auto start = std::chrono::steady_clock::now();
            for (int f = 0; f < kFrames; ++f)
            {
                if (gpu)
                {
                    QuadBlit::BeginSinkFrame(buffer.data(), kWidth, kHeight, kFormat, 0, 0, 0);
                    drawScene(buffer.data());
                    QuadBlit::EndSinkFrame();
                }
                else
                {
                    std::memset(buffer.data(), 0, buffer.size());
                    drawScene(buffer.data());
                }
                if (f == kFrames - 1)
                {
                    std::vector<uint8_t> frame;
                    presenter->PushLayer(buffer.data(), kWidth, kHeight, kFormat);
                    presenter->ReadFrame(frame);  // waits for the GPU
                }
                presenter->Present(buffer.data(), kWidth, kHeight, kFormat);
            }
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() /
                   kFrames;
        };
        time(true);  // uploads the textures, warms the driver
        const double cpu = time(false);
        const double gpu = time(true);
        std::printf("%d sprites, 20 texts, a background at %dx%d: CPU %.2f ms/frame, GPU %.2f ms/frame\n", sprites,
                    kWidth, kHeight, cpu, gpu);
    }
}

#endif  // DEKI_CAPABILITY_OPENGL
