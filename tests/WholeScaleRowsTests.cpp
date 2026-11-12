// Plain blits onto ARGB8888 at a whole-number scale take WholeScaleRows, which
// reads each source pixel once for the block it covers. They must give the
// pipeline's pixels. The pipeline is reached for comparison with a chroma key
// no source pixel has: it changes the path, not a single pixel.

#include <gtest/gtest.h>
#include <cstdint>
#include <vector>

#include <deki/Engine.h>
#include "PixelFormat.h"
#include "QuadBlit.h"
#include "RenderProfiler.h"
#include "WholeScaleRows.h"

namespace
{

constexpr int kSrcW = 7, kSrcH = 5;
constexpr int kDstW = 40, kDstH = 30;

struct Format
{
    const char* name;
    QuadBlit::PixelLayout layout;
    int bpp;
};

// Opaque, clear and translucent pixels; no colour is the key (1, 2, 3).
std::vector<uint8_t> MakeSource(const Format& f)
{
    std::vector<uint8_t> pixels(static_cast<size_t>(kSrcW) * kSrcH * f.bpp);
    for (int y = 0; y < kSrcH; ++y)
    {
        for (int x = 0; x < kSrcW; ++x)
        {
            uint8_t* p = &pixels[(static_cast<size_t>(y) * kSrcW + x) * f.bpp];
            const int i = y * kSrcW + x;
            const uint8_t alpha = i % 3 == 0 ? 255 : (i % 3 == 1 ? 0 : static_cast<uint8_t>(40 + i * 7));
            for (int c = 0; c < f.bpp; ++c)
            {
                p[c] = static_cast<uint8_t>(10 + i * 31 + c * 67);
            }
            if (f.bpp == 4)
            {
                p[3] = alpha;
            }
            else if (f.bpp == 3 && f.layout.isRGB565)
            {
                p[2] = alpha;
            }
            else if (f.bpp == 1)
            {
                p[0] = alpha;
            }
        }
    }
    return pixels;
}

std::vector<uint32_t> Draw(const std::vector<uint8_t>& pixels, const Format& f, bool viaPipeline, int destX, int destY,
                           int destW, int destH, bool clip)
{
    std::vector<uint32_t> target(static_cast<size_t>(kDstW) * kDstH);
    for (size_t i = 0; i < target.size(); ++i)
    {
        target[i] = 0x80000000u | static_cast<uint32_t>(i * 2654435761u & 0xFFFFFFu);
    }
    QuadBlit::Source s = QuadBlit::MakeSource(pixels.data(), kSrcW, kSrcH, f.layout);
    if (viaPipeline)
    {
        s.hasChromaKey = true;
        s.keyR = 1;
        s.keyG = 2;
        s.keyB = 3;
    }
    QuadBlit::ClearClipStack();
    if (clip)
    {
        QuadBlit::PushClipRect(5, 3, 31, 22);
    }
    QuadBlit::BlitScaled(s, reinterpret_cast<uint8_t*>(target.data()), kDstW, kDstH, Deki::ColorFormat::ARGB8888, destX,
                         destY, destW, destH, 255, 255, 255, 255);
    QuadBlit::ClearClipStack();
    return target;
}

const Format kFormats[] = {
    { "RGBA8888", QuadBlit::PixelLayout::RGBA8888(), 4 }, { "RGB888", QuadBlit::PixelLayout::RGB888(), 3 },
    { "RGB565", QuadBlit::PixelLayout::RGB565(), 2 },     { "RGB565A8", QuadBlit::PixelLayout::RGB565A8(), 3 },
    { "ALPHA8", QuadBlit::PixelLayout::Alpha8(), 1 },
};

}  // namespace

TEST(WholeScaleRows, MatchThePipelineOntoARGB8888)
{
    const int scales[][2] = { { 1, 1 }, { 2, 2 }, { 3, 3 }, { 4, 4 }, { 2, 3 }, { 3, 1 } };
    const int origins[][2] = { { 2, 1 }, { -5, -3 }, { 20, 14 }, { 33, 26 } };
    for (const Format& f : kFormats)
    {
        const std::vector<uint8_t> pixels = MakeSource(f);
        for (const auto& scale : scales)
        {
            for (const auto& origin : origins)
            {
                for (bool clip : { false, true })
                {
                    const int w = kSrcW * scale[0];
                    const int h = kSrcH * scale[1];
                    const std::vector<uint32_t> fast = Draw(pixels, f, false, origin[0], origin[1], w, h, clip);
                    const std::vector<uint32_t> pipeline = Draw(pixels, f, true, origin[0], origin[1], w, h, clip);
                    EXPECT_EQ(fast, pipeline) << f.name << " at " << scale[0] << "x" << scale[1] << " from "
                                              << origin[0] << ", " << origin[1] << (clip ? ", clipped" : "");
                }
            }
        }
    }
}

TEST(WholeScaleRows, ASizeThatIsNotAWholeScaleIsLeftToThePipeline)
{
    EXPECT_EQ(QuadBlit::WholeScale(7, 14), 2);
    EXPECT_EQ(QuadBlit::WholeScale(7, 7), 1);
    EXPECT_EQ(QuadBlit::WholeScale(7, 15), 0);
    EXPECT_EQ(QuadBlit::WholeScale(7, 3), 0);  // shrunk
    EXPECT_EQ(QuadBlit::WholeScale(0, 3), 0);

    // Drawn anyway, by the pipeline: the same pixels either way.
    for (const Format& f : kFormats)
    {
        const std::vector<uint8_t> pixels = MakeSource(f);
        EXPECT_EQ(Draw(pixels, f, false, 3, 2, 17, 12, false), Draw(pixels, f, true, 3, 2, 17, 12, false)) << f.name;
    }
}

TEST(WholeScaleRows, IsThePathAPlainScaledBlitTakes)
{
    namespace Profiler = DekiRendering::RenderProfiler;
    Profiler::Enable([]() -> uint64_t { return 0; });
    const std::vector<uint8_t> pixels = MakeSource(kFormats[0]);
    Draw(pixels, kFormats[0], false, 2, 1, kSrcW * 2, kSrcH * 2, false);
    const std::vector<Profiler::PathEntry> paths = Profiler::TakePaths();
    uint32_t frames = 0;
    Profiler::Take(frames);
    Profiler::Enable(nullptr);
    ASSERT_EQ(paths.size(), 1u);
    EXPECT_STREQ(paths[0].name, "WholeScaleRows");
    EXPECT_EQ(paths[0].pixels, static_cast<uint64_t>(kSrcW * 2 * kSrcH * 2));
}
