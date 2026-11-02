// QuadBlit's blit sink (QuadBlit::BlitSink), which draws a frame's blits
// somewhere else than the CPU, a GPU:
// - SinkBlit's contract: a sink that does exactly what its comment says, on
//   the CPU, draws every blit byte for byte as QuadBlit's own code does, for
//   every source layout, scale, rotation, flip, tint, key, dither and clip.
//   A GPU sink implements that comment in a shader;
// - only blits into the sink frame's target reach the sink, and nothing
//   changes without a sink;
// - a blit the sink cannot draw is drawn on the CPU, and the sink told;
// - DekiRenderSystem hands whole frames of a camera that clears to the sink,
//   with the clear, and never clears the buffer itself;
// - render passes that write into the buffer tell the sink after each hook,
//   and those that say they do not, do not;
// - ForgetScaled reaches the sink.

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <deki/Engine.h>
#include <deki/Object.h>
#include <deki/Scene.h>
#include <deki/reflection/ComponentFactory.h>
#include "CameraComponent.h"
#include "DekiRenderSystem.h"
#include "PixelFormat.h"
#include "QuadBlit.h"
#include "RenderPass.h"
#include "RendererComponent.h"
#include "Standard2DRenderer.h"

// Tests name the package's types unqualified.
using namespace DekiRendering;

namespace
{

constexpr int kW = 48, kH = 40;
constexpr Deki::ColorFormat kFormat = Deki::ColorFormat::RGB565A8;

std::vector<uint8_t> Cleared(uint8_t r, uint8_t g, uint8_t b)
{
    std::vector<uint8_t> buffer(Deki::FrameBufferBytes(kFormat, kW, kH));
    for (size_t i = 0; i < static_cast<size_t>(kW) * kH; ++i)
    {
        DekiPixel::WriteDstPixel<kFormat>(buffer.data(), i, r, g, b, 255);
    }
    return buffer;
}

/// A source in one of QuadBlit's layouts, with colours and alphas of every
/// kind: 0, partial and opaque.
struct TestSource
{
    std::vector<uint8_t> pixels;
    QuadBlit::Source source;
};

TestSource MakeTestSource(QuadBlit::PixelLayout layout, int w, int h, uint32_t seed)
{
    TestSource t;
    t.pixels.resize(static_cast<size_t>(w) * h * layout.bytesPerPixel);
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const uint32_t n = static_cast<uint32_t>(x * 37 + y * 101) + seed * 13;
            const uint8_t r = static_cast<uint8_t>(n * 7), g = static_cast<uint8_t>(n * 3 + 90),
                          b = static_cast<uint8_t>(n * 11 + 30);
            const int k = (x + 2 * y + static_cast<int>(seed)) % 7;
            const uint8_t a = k == 0 ? 0 : k == 1 ? 96 : k == 2 ? 200 : 255;
            uint8_t* p = &t.pixels[(static_cast<size_t>(y) * w + x) * layout.bytesPerPixel];
            if (layout.isRGB565)
            {
                DekiPixel::Store565(p, DekiPixel::PackRGB565(r, g, b));
                if (layout.bytesPerPixel == 3)
                {
                    p[2] = a;
                }
            }
            else if (layout.bytesPerPixel == 1)
            {
                p[0] = a;
            }
            else
            {
                p[0] = r;
                p[1] = g;
                p[2] = b;
                if (layout.bytesPerPixel == 4)
                {
                    p[3] = a;
                }
            }
        }
    }
    t.source = QuadBlit::MakeSource(t.pixels.data(), w, h, layout);
    return t;
}

const QuadBlit::PixelLayout kLayouts[] = {
    QuadBlit::PixelLayout::RGB565(),   QuadBlit::PixelLayout::RGB565A8(), QuadBlit::PixelLayout::RGB565A8NoAlpha(),
    QuadBlit::PixelLayout::RGBA8888(), QuadBlit::PixelLayout::RGB888(),   QuadBlit::PixelLayout::Alpha8(),
};

/// SinkBlit's comment, implemented on the CPU into an RGB565A8 frame:
/// nothing else is known of the blit than what the sink is handed.
class ReferenceSink : public QuadBlit::BlitSink
{
public:
    std::vector<uint8_t> frame;
    uint8_t* target = nullptr;
    int draws = 0, written = 0, ends = 0;
    bool decline = false;  // Draw says it cannot

    bool BeginFrame(uint8_t* t, int32_t width, int32_t height, Deki::ColorFormat format, uint8_t r, uint8_t g,
                    uint8_t b) override
    {
        EXPECT_EQ(width, kW);
        EXPECT_EQ(height, kH);
        EXPECT_EQ(format, kFormat);
        frame = Cleared(r, g, b);
        std::memset(t, 0, Deki::FrameBufferBytes(format, width, height));
        target = t;
        return true;
    }

    bool Draw(const QuadBlit::SinkBlit& blit) override
    {
        ++draws;
        if (decline)
        {
            return false;
        }
        const QuadBlit::Source& s = *blit.source;
        const int32_t stride = s.stride ? s.stride : s.width * s.bytesPerPixel;
        for (int32_t py = blit.top; py < blit.bottom; ++py)
        {
            for (int32_t px = blit.left; px < blit.right; ++px)
            {
                EXPECT_TRUE(px >= 0 && py >= 0 && px < kW && py < kH);
                const uint32_t dx = static_cast<uint32_t>(px - blit.originX);
                const uint32_t dy = static_cast<uint32_t>(py - blit.originY);
                int32_t x =
                    static_cast<int32_t>(static_cast<uint32_t>(blit.originSx) + dx * static_cast<uint32_t>(blit.dSxDx) +
                                         dy * static_cast<uint32_t>(blit.dSxDy)) >>
                    16;
                int32_t y =
                    static_cast<int32_t>(static_cast<uint32_t>(blit.originSy) + dx * static_cast<uint32_t>(blit.dSyDx) +
                                         dy * static_cast<uint32_t>(blit.dSyDy)) >>
                    16;
                if (x < 0 || y < 0 || x >= blit.sampleWidth || y >= blit.sampleHeight)
                {
                    continue;
                }
                x = blit.repeatColumn ? 0 : x;
                y = blit.repeatRow ? 0 : y;
                if (s.flipV)
                {
                    y = s.height - 1 - y;
                }
                if (s.flipH)
                {
                    x = s.width - 1 - x;
                }
                if (s.flipD && s.width == s.height)
                {
                    std::swap(x, y);
                }
                Composite(s, s.pixels + y * stride + x * s.bytesPerPixel, blit, px, py);
            }
        }
        return true;
    }

    void TargetWritten() override { ++written; }
    void EndFrame() override { ++ends; }
    void Forget(const uint8_t*, size_t) override {}

private:
    void Composite(const QuadBlit::Source& s, const uint8_t* p, const QuadBlit::SinkBlit& blit, int32_t px, int32_t py)
    {
        using DekiPixel::Div255;
        uint8_t r, g, b, a;
        if (s.isRGB565)
        {
            if (s.bytesPerPixel >= 3)
            {
                DekiPixel::ReadSrcPixel<DekiPixel::SrcKind::RGB565A8>(p, s.hasAlpha, r, g, b, a);
            }
            else
            {
                DekiPixel::ReadSrcPixel<DekiPixel::SrcKind::RGB565>(p, s.hasAlpha, r, g, b, a);
            }
        }
        else if (s.bytesPerPixel == 4)
        {
            DekiPixel::ReadSrcPixel<DekiPixel::SrcKind::RGBA8888>(p, s.hasAlpha, r, g, b, a);
        }
        else if (s.bytesPerPixel == 3)
        {
            DekiPixel::ReadSrcPixel<DekiPixel::SrcKind::RGB888>(p, s.hasAlpha, r, g, b, a);
        }
        else
        {
            DekiPixel::ReadSrcPixel<DekiPixel::SrcKind::ALPHA8>(p, s.hasAlpha, r, g, b, a);
        }
        if (a == 0 || (s.hasChromaKey && r == s.keyR && g == s.keyG && b == s.keyB))
        {
            return;
        }
        r = Div255(r * blit.tintR);
        g = Div255(g * blit.tintG);
        b = Div255(b * blit.tintB);
        a = Div255(a * blit.tintA);
        if (a == 0)
        {
            return;
        }
        const size_t i = static_cast<size_t>(py) * kW + px;
        if (blit.orderedDither)
        {
            if (a > DekiPixel::BayerThreshold(px, py))
            {
                DekiPixel::WriteDstPixel<kFormat>(frame.data(), i, r, g, b, 255);
            }
            return;
        }
        if (a == 255)
        {
            DekiPixel::WriteDstPixel<kFormat>(frame.data(), i, r, g, b, 255);
            return;
        }
        uint8_t br, bg, bb, ba;
        DekiPixel::ReadDstPixel<kFormat>(frame.data(), i, br, bg, bb, ba);
        const uint32_t inv = 255u - a;
        DekiPixel::WriteDstPixel<kFormat>(frame.data(), i, Div255(r * a + br * inv), Div255(g * a + bg * inv),
                                          Div255(b * a + bb * inv), DekiPixel::AlphaUnion(a, ba));
    }
};

/// Registers a sink for the test's length.
struct SinkScope
{
    explicit SinkScope(QuadBlit::BlitSink* sink) { QuadBlit::SetBlitSink(sink); }
    ~SinkScope()
    {
        QuadBlit::EndSinkFrame();
        QuadBlit::SetBlitSink(nullptr);
        QuadBlit::ClearClipStack();
    }
};

/// Draws `draw` with QuadBlit's CPU code and through the reference sink, and
/// expects the same bytes.
template <typename Draw>
void ExpectSinkMatchesCpu(const char* what, Draw draw)
{
    SCOPED_TRACE(what);
    std::vector<uint8_t> cpu = Cleared(20, 40, 60);
    QuadBlit::ClearClipStack();
    draw(cpu.data());

    ReferenceSink sink;
    SinkScope scope(&sink);
    std::vector<uint8_t> target(Deki::FrameBufferBytes(kFormat, kW, kH), 0x5A);
    QuadBlit::ClearClipStack();
    ASSERT_TRUE(QuadBlit::BeginSinkFrame(target.data(), kW, kH, kFormat, 20, 40, 60));
    draw(target.data());
    QuadBlit::EndSinkFrame();

    EXPECT_GT(sink.draws, 0);
    EXPECT_EQ(sink.written, 0);
    EXPECT_EQ(sink.ends, 1);
    EXPECT_EQ(target, std::vector<uint8_t>(target.size(), 0)) << "nothing reaches the buffer";
    EXPECT_NE(cpu, Cleared(20, 40, 60)) << "the case draws nothing";
    int differ = 0;
    for (size_t i = 0; i < cpu.size(); ++i)
    {
        differ += cpu[i] != sink.frame[i];
    }
    EXPECT_EQ(differ, 0);
}

void Blit(const QuadBlit::Source& s, uint8_t* target, int x, int y, float sx, float sy, float rotation, float px,
          float py, uint8_t r = 255, uint8_t g = 255, uint8_t b = 255, uint8_t a = 255, bool dither = false)
{
    QuadBlit::Blit(s, target, kW, kH, kFormat, x, y, sx, sy, rotation, px, py, r, g, b, a, dither);
}

}  // namespace

TEST(BlitSink, TheContractDrawsWhatTheCpuDraws)
{
    for (QuadBlit::PixelLayout layout : kLayouts)
    {
        TestSource t = MakeTestSource(layout, 13, 11, 1);
        const QuadBlit::Source& s = t.source;
        const std::string name = "layout " + std::to_string(layout.bytesPerPixel) + (layout.hasAlpha ? "a" : "") +
                                 (layout.isRGB565 ? " 565" : "");
        SCOPED_TRACE(name);
        ExpectSinkMatchesCpu("1:1", [&](uint8_t* target) { Blit(s, target, 5, 7, 1, 1, 0, 0, 0); });
        ExpectSinkMatchesCpu("off the edges", [&](uint8_t* target) { Blit(s, target, -4, kH - 6, 1, 1, 0, 0, 0); });
        ExpectSinkMatchesCpu("scaled up", [&](uint8_t* target) { Blit(s, target, 20, 18, 2.6f, 1.7f, 0, 0.5f, 0.5f); });
        ExpectSinkMatchesCpu("scaled down", [&](uint8_t* target) { Blit(s, target, 9, 9, 0.6f, 0.45f, 0, 0, 0); });
        ExpectSinkMatchesCpu("rotated",
                             [&](uint8_t* target) { Blit(s, target, 24, 20, 1.7f, 1.3f, 0.7f, 0.5f, 0.5f); });
        ExpectSinkMatchesCpu("rotated past a corner",
                             [&](uint8_t* target) { Blit(s, target, 2, 3, 2.0f, 2.0f, -2.4f, 0.2f, 0.8f); });
        ExpectSinkMatchesCpu("tinted",
                             [&](uint8_t* target) { Blit(s, target, 10, 6, 1.5f, 1.5f, 0, 0, 0, 200, 120, 60, 180); });
        ExpectSinkMatchesCpu("tinted and rotated", [&](uint8_t* target)
                             { Blit(s, target, 22, 22, 1.2f, 1.4f, 2.1f, 0.5f, 0.5f, 90, 255, 160, 140); });
        ExpectSinkMatchesCpu("dithered", [&](uint8_t* target)
                             { Blit(s, target, 4, 4, 2.0f, 2.0f, 0, 0, 0, 255, 255, 255, 170, true); });
        ExpectSinkMatchesCpu("dithered and rotated", [&](uint8_t* target)
                             { Blit(s, target, 24, 20, 1.5f, 1.5f, 0.4f, 0.5f, 0.5f, 255, 255, 255, 255, true); });
        ExpectSinkMatchesCpu("BlitScaled", [&](uint8_t* target)
                             { QuadBlit::BlitScaled(s, target, kW, kH, kFormat, 3, 2, 31, 17, 255, 200, 255, 230); });
        ExpectSinkMatchesCpu("clipped",
                             [&](uint8_t* target)
                             {
                                 QuadBlit::PushClipRect(10, 8, 30, 26);
                                 Blit(s, target, 6, 4, 2.0f, 2.0f, 0, 0, 0);
                                 Blit(s, target, 24, 20, 1.8f, 1.8f, 0.9f, 0.5f, 0.5f);
                                 QuadBlit::SetClipEnabled(false);
                                 Blit(s, target, 30, 2, 1, 1, 0, 0, 0);
                                 QuadBlit::SetClipEnabled(true);
                                 QuadBlit::PopClipRect();
                             });
    }
}

TEST(BlitSink, TheContractCoversFlipsKeysStrideAndStretch)
{
    TestSource square = MakeTestSource(QuadBlit::PixelLayout::RGB565A8(), 12, 12, 2);
    for (int flips = 1; flips < 8; ++flips)
    {
        QuadBlit::Source s = square.source;
        s.flipH = (flips & 1) != 0;
        s.flipV = (flips & 2) != 0;
        s.flipD = (flips & 4) != 0;
        SCOPED_TRACE("flips " + std::to_string(flips));
        ExpectSinkMatchesCpu("1:1", [&](uint8_t* target) { Blit(s, target, 3, 3, 1, 1, 0, 0, 0); });
        ExpectSinkMatchesCpu("scaled", [&](uint8_t* target) { Blit(s, target, 20, 4, 2.3f, 1.6f, 0, 0, 0); });
        ExpectSinkMatchesCpu("rotated",
                             [&](uint8_t* target) { Blit(s, target, 24, 20, 1.5f, 1.5f, 1.1f, 0.5f, 0.5f); });
    }

    // A chroma key, on a 565 source (quantised) and an 888 one.
    TestSource rgb565 = MakeTestSource(QuadBlit::PixelLayout::RGB565(), 13, 11, 3);
    QuadBlit::Source keyed = rgb565.source;
    keyed.hasChromaKey = true;
    DekiPixel::UnpackRGB565(DekiPixel::Load565(rgb565.pixels.data() + 2 * 5), keyed.keyR, keyed.keyG, keyed.keyB);
    ExpectSinkMatchesCpu("keyed 565", [&](uint8_t* target) { Blit(keyed, target, 4, 4, 1, 1, 0, 0, 0); });
    ExpectSinkMatchesCpu("keyed 565, scaled and tinted",
                         [&](uint8_t* target) { Blit(keyed, target, 4, 4, 2.2f, 2.5f, 0, 0, 0, 100, 200, 255, 255); });
    TestSource rgb888 = MakeTestSource(QuadBlit::PixelLayout::RGB888(), 13, 11, 4);
    QuadBlit::Source keyed888 = rgb888.source;
    keyed888.hasChromaKey = true;
    keyed888.keyR = rgb888.pixels[3 * 7];
    keyed888.keyG = rgb888.pixels[3 * 7 + 1];
    keyed888.keyB = rgb888.pixels[3 * 7 + 2];
    ExpectSinkMatchesCpu("keyed 888, rotated",
                         [&](uint8_t* target) { Blit(keyed888, target, 24, 20, 2.0f, 2.0f, 0.3f, 0.5f, 0.5f); });

    // A sub-rect of a bigger picture (an atlas tile).
    TestSource atlas = MakeTestSource(QuadBlit::PixelLayout::RGBA8888(), 20, 16, 5);
    QuadBlit::Source tile = atlas.source;
    tile.pixels = atlas.pixels.data() + (3 * 20 + 4) * 4;
    tile.width = 9;
    tile.height = 7;
    tile.stride = 20 * 4;
    ExpectSinkMatchesCpu("a tile, scaled", [&](uint8_t* target) { Blit(tile, target, 2, 2, 3.0f, 3.0f, 0, 0, 0); });
    ExpectSinkMatchesCpu("a tile, rotated",
                         [&](uint8_t* target) { Blit(tile, target, 24, 20, 2, 2, 0.8f, 0.5f, 0.5f); });

    // One column and one row stretched over a rectangle (a baked gradient).
    TestSource column = MakeTestSource(QuadBlit::PixelLayout::RGB565(), 1, 9, 6);
    QuadBlit::Source strip = column.source;
    strip.stretchWidth = 14;
    ExpectSinkMatchesCpu("a stretched column", [&](uint8_t* target) { Blit(strip, target, 3, 3, 2, 2, 0, 0, 0); });
    ExpectSinkMatchesCpu("a stretched column, rotated",
                         [&](uint8_t* target) { Blit(strip, target, 24, 20, 1.5f, 1.5f, 0.5f, 0.5f, 0.5f); });
    TestSource row = MakeTestSource(QuadBlit::PixelLayout::RGB565(), 9, 1, 7);
    QuadBlit::Source stripRow = row.source;
    stripRow.stretchHeight = 11;
    ExpectSinkMatchesCpu("a stretched row, rotated",
                         [&](uint8_t* target) { Blit(stripRow, target, 24, 20, 2, 2, -0.6f, 0.5f, 0.5f); });
}

// Only the sink frame's target goes to the sink; any other buffer, and every
// buffer outside a sink frame or without a sink, is drawn as before.
TEST(BlitSink, OnlyTheTargetGoesToTheSink)
{
    TestSource t = MakeTestSource(QuadBlit::PixelLayout::RGB565A8(), 8, 8, 8);
    std::vector<uint8_t> expected = Cleared(1, 2, 3);
    Blit(t.source, expected.data(), 4, 4, 1, 1, 0, 0, 0);

    std::vector<uint8_t> target(Deki::FrameBufferBytes(kFormat, kW, kH));
    EXPECT_FALSE(QuadBlit::BeginSinkFrame(target.data(), kW, kH, kFormat, 0, 0, 0)) << "no sink";
    EXPECT_EQ(QuadBlit::SinkTarget(), nullptr);

    ReferenceSink sink;
    SinkScope scope(&sink);
    std::vector<uint8_t> other = Cleared(1, 2, 3);
    Blit(t.source, other.data(), 4, 4, 1, 1, 0, 0, 0);
    EXPECT_EQ(other, expected) << "no sink frame: drawn on the CPU";
    EXPECT_EQ(sink.draws, 0);

    ASSERT_TRUE(QuadBlit::BeginSinkFrame(target.data(), kW, kH, kFormat, 0, 0, 0));
    EXPECT_EQ(QuadBlit::SinkTarget(), target.data());
    other = Cleared(1, 2, 3);
    Blit(t.source, other.data(), 4, 4, 1, 1, 0, 0, 0);
    EXPECT_EQ(other, expected) << "another buffer: drawn on the CPU";
    EXPECT_EQ(sink.draws, 0);
    Blit(t.source, target.data(), 4, 4, 1, 1, 0, 0, 0);
    EXPECT_EQ(sink.draws, 1);
    QuadBlit::NoteWritten(other.data());
    EXPECT_EQ(sink.written, 0) << "another buffer's writes are not the sink's";
    QuadBlit::NoteWritten(target.data());
    EXPECT_EQ(sink.written, 1);
    QuadBlit::EndSinkFrame();
    EXPECT_EQ(QuadBlit::SinkTarget(), nullptr);
    Blit(t.source, target.data(), 4, 4, 1, 1, 0, 0, 0);
    EXPECT_EQ(sink.draws, 1) << "after the frame: drawn on the CPU";
}

// A blit the sink cannot draw is drawn into the target, and the sink told so
// it composes it in its place.
TEST(BlitSink, ABlitTheSinkCannotDrawIsDrawnOnTheCpu)
{
    TestSource t = MakeTestSource(QuadBlit::PixelLayout::RGB565A8(), 8, 8, 9);
    ReferenceSink sink;
    sink.decline = true;
    SinkScope scope(&sink);
    std::vector<uint8_t> target(Deki::FrameBufferBytes(kFormat, kW, kH));
    ASSERT_TRUE(QuadBlit::BeginSinkFrame(target.data(), kW, kH, kFormat, 0, 0, 0));
    Blit(t.source, target.data(), 4, 4, 1.5f, 1.5f, 0.3f, 0, 0);
    EXPECT_EQ(sink.draws, 1);
    EXPECT_EQ(sink.written, 1);
    EXPECT_NE(target, std::vector<uint8_t>(target.size(), 0)) << "drawn into the buffer";
}

TEST(BlitSink, ForgetScaledReachesTheSink)
{
    struct ForgetSink : ReferenceSink
    {
        const uint8_t* begin = nullptr;
        size_t bytes = 0;
        void Forget(const uint8_t* b, size_t n) override
        {
            begin = b;
            bytes = n;
        }
    } sink;
    SinkScope scope(&sink);
    uint8_t pixels[64];
    QuadBlit::ForgetScaled(pixels, sizeof(pixels));
    EXPECT_EQ(sink.begin, pixels);
    EXPECT_EQ(sink.bytes, sizeof(pixels));
}

TEST(BlitSink, RevisionsAreFreshAndNeverZero)
{
    const uint32_t a = QuadBlit::NextRevision();
    const uint32_t b = QuadBlit::NextRevision();
    EXPECT_NE(a, 0u);
    EXPECT_NE(b, 0u);
    EXPECT_NE(a, b);
}

// ---------------------------------------------------------------------------
// The render system and the renderer, with a sink that records what it is
// handed.
// ---------------------------------------------------------------------------

namespace
{

/// A 4x4 opaque square, in its own colour so the record tells them apart.
class Patch : public RendererComponent
{
public:
    // Hand-written, not reflected: the class sets its own type info.
    static const Deki::ComponentTypeInfo kTypeInfo;
    Patch() { SetTypeInfo(&kTypeInfo); }
    uint8_t shade = 0;
    bool RenderContent(const Deki::Object*, QuadBlit::Source& out, float& px, float& py, uint8_t& r, uint8_t& g,
                       uint8_t& b, uint8_t& a) override
    {
        std::memset(m_Pixels, shade, sizeof(m_Pixels));
        out = QuadBlit::MakeSource(m_Pixels, 4, 4, QuadBlit::PixelLayout::RGB565());
        px = py = 0.5f;
        r = g = b = a = 255;
        return true;
    }

private:
    uint8_t m_Pixels[4 * 4 * 2] = {};
};
const Deki::ComponentTypeInfo Patch::kTypeInfo =
    Deki::MakeHandWrittenTypeInfo<Patch, RendererComponent>(/*declaresUpdate=*/false);

/// Records the order of what reaches it: "b" a frame begun, a draw as its
/// source's first byte, "w" the buffer written, "e" the frame ended.
class RecordingSink : public QuadBlit::BlitSink
{
public:
    std::string log;
    uint8_t clear[3] = {};
    bool accept = true;
    bool BeginFrame(uint8_t* target, int32_t width, int32_t height, Deki::ColorFormat format, uint8_t r, uint8_t g,
                    uint8_t b) override
    {
        if (!accept)
        {
            return false;
        }
        log += "b";
        clear[0] = r;
        clear[1] = g;
        clear[2] = b;
        std::memset(target, 0, Deki::FrameBufferBytes(format, width, height));
        return true;
    }
    bool Draw(const QuadBlit::SinkBlit& blit) override
    {
        log += std::to_string(blit.source->pixels[0]);
        return true;
    }
    void TargetWritten() override { log += "w"; }
    void EndFrame() override { log += "e"; }
    void Forget(const uint8_t*, size_t) override {}
};

/// Writes into the buffer by hand in Execute, for the patch it follows.
class HandPass : public RenderPass
{
public:
    bool writes = true;
    uint32_t HookMask() const override { return RenderPassHooks::Execute; }
    bool WritesBuffer() const override { return writes; }
    void Execute(Deki::Object*, RenderContext& ctx) override { ctx.buffer[0] = 0xFF; }
};

struct SinkScene
{
    Deki::Scene scene;
    CameraComponent* camera = nullptr;
    Standard2DRenderer renderer;
    DekiRenderSystem rs;

    SinkScene()
    {
        auto* camObj = new Deki::Object("Camera");
        camera = camObj->AddComponent<CameraComponent>();
        camera->clearColor = Deki::Color(200, 100, 50);
        scene.AddObject(camObj);
        for (uint8_t shade : { 1, 2, 3 })
        {
            auto* o = new Deki::Object("patch");
            Patch* patch = o->AddComponent<Patch>();
            patch->shade = shade;
            patch->sortingOrder = shade;
            scene.AddObject(o);
        }
        EXPECT_TRUE(rs.Setup(kW, kH, kFormat));
        rs.SetRenderer(&renderer);
    }
};

}  // namespace

// A whole frame of a camera that clears goes to the sink: the clear colour
// with it, the blits in draw order, and the buffer is never cleared to the
// colour (it would be composed over the sink's frame).
TEST(BlitSink, TheRenderSystemDrawsWholeFramesOnTheSink)
{
    SinkScene s;
    RecordingSink sink;
    SinkScope scope(&sink);
    s.rs.Render(&s.scene);
    EXPECT_EQ(sink.log, "b123e");
    EXPECT_EQ(sink.clear[0], 200);
    EXPECT_EQ(sink.clear[1], 100);
    EXPECT_EQ(sink.clear[2], 50);
    const uint8_t* fb = s.rs.GetFrameBuffer();
    EXPECT_EQ(std::vector<uint8_t>(fb, fb + Deki::FrameBufferBytes(kFormat, kW, kH)),
              std::vector<uint8_t>(Deki::FrameBufferBytes(kFormat, kW, kH), 0));
    EXPECT_EQ(QuadBlit::SinkTarget(), nullptr) << "the frame is over";

    // Redrawing only what changed changes nothing: the sink's frames start over.
    s.rs.SetRedrawChanges(true);
    sink.log.clear();
    s.rs.Render(&s.scene);
    s.rs.Render(&s.scene);
    EXPECT_EQ(sink.log, "b123eb123e");

    // A camera that keeps its last frame stays on the CPU.
    s.camera->clearEveryFrame = false;
    sink.log.clear();
    s.rs.Render(&s.scene);
    EXPECT_EQ(sink.log, "");

    // So does a frame the sink declines: cleared and drawn as before.
    s.camera->clearEveryFrame = true;
    sink.accept = false;
    s.rs.SetRedrawChanges(false);
    s.rs.Render(&s.scene);
    EXPECT_EQ(sink.log, "");
    int opaque = 0;
    for (int i = 0; i < kW * kH; ++i)
    {
        opaque += s.rs.GetFrameBuffer()[i * 3 + 2] == 255;
    }
    EXPECT_EQ(opaque, kW * kH) << "cleared, opaque, on the CPU";
}

// What a pass writes into the buffer itself is the sink's to compose after
// each of its hooks, before the next blit; a pass that says it writes nothing
// costs nothing.
TEST(BlitSink, PassesThatWriteTellTheSinkAfterEachHook)
{
    SinkScene s;
    RecordingSink sink;
    SinkScope scope(&sink);
    HandPass pass;
    s.renderer.AddPass(&pass);
    s.rs.Render(&s.scene);
    // Each patch's blit, then the pass's hook for it.
    EXPECT_EQ(sink.log, "b1w2w3we");

    pass.writes = false;
    sink.log.clear();
    s.rs.Render(&s.scene);
    EXPECT_EQ(sink.log, "b123e");
    s.renderer.RemovePass(&pass);
}
