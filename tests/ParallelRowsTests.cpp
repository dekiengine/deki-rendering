// A large blit's rows drawn on two cores (RegisterParallelRows) give the same
// pixels as drawn on one: the plain copy, a whole-number scale, a tinted
// fractional scale and an alpha blend, each with the lower half on a second
// thread at the same time as the upper half. Small blits are not split, and a
// split that refuses leaves the blit to one core.

#include <gtest/gtest.h>

#include <deki/Engine.h>
#include "PixelFormat.h"
#include "QuadBlit.h"

#include <atomic>
#include <cstring>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

namespace
{

std::atomic<int> g_Splits{ 0 };
bool g_Refuse = false;

// The other core, as a thread: the lower rows there while the upper ones are
// drawn here.
bool SplitOnThread(QuadBlit::RowsJob job, void* context, int32_t startY, int32_t splitY, int32_t endY)
{
    if (g_Refuse)
    {
        return false;
    }
    ++g_Splits;
    std::thread lower([&] { job(context, splitY, endY); });
    job(context, startY, splitY);
    lower.join();
    return true;
}

constexpr int kTW = 320, kTH = 200;

std::vector<uint8_t> Source(int w, int h, bool alpha)
{
    const int bpp = alpha ? 3 : 2;
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * bpp);
    for (int i = 0; i < w * h; ++i)
    {
        DekiPixel::Store565(&px[static_cast<size_t>(i) * bpp], static_cast<uint16_t>(0x2C51u * (i + 1)));
        if (alpha)
        {
            // Clear, opaque and translucent runs.
            const int a = (i / 7) % 3;
            px[static_cast<size_t>(i) * bpp + 2] = static_cast<uint8_t>(a == 0 ? 0 : (a == 1 ? 255 : 120));
        }
    }
    return px;
}

std::vector<uint8_t> Draw(const std::vector<uint8_t>& src, int w, int h, bool alpha, int x, int y, int dw, int dh,
                          uint8_t tint)
{
    std::vector<uint8_t> target(static_cast<size_t>(kTW) * kTH * 2);
    for (size_t i = 0; i < target.size(); ++i)
    {
        target[i] = static_cast<uint8_t>(i * 13 + 5);
    }
    QuadBlit::Source s = QuadBlit::MakeSource(src.data(), w, h,
                                              alpha ? QuadBlit::PixelLayout::RGB565A8() : QuadBlit::PixelLayout::RGB565());
    QuadBlit::ClearClipStack();
    QuadBlit::BlitScaled(s, target.data(), kTW, kTH, Deki::ColorFormat::RGB565, x, y, dw, dh, tint, tint, 255, 255);
    return target;
}

struct Case
{
    const char* name;
    int w, h;
    bool alpha;
    int x, y, dw, dh;
    uint8_t tint;
};

const Case kCases[] = {
    { "copy", 300, 180, false, 7, 9, 300, 180, 255 },
    { "2x with alpha", 120, 80, true, -11, 13, 240, 160, 255 },
    { "3x, tinted", 90, 60, true, 4, -5, 270, 180, 200 },
    { "fractional scale, tinted", 100, 70, true, 3, 2, 251, 177, 180 },
    { "1:1 blend", 280, 150, true, 20, 30, 280, 150, 255 },
};

class ParallelRowsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        g_Splits = 0;
        g_Refuse = false;
    }
    void TearDown() override { QuadBlit::RegisterParallelRows(nullptr, 0); }
};

}  // namespace

TEST_F(ParallelRowsTest, TwoCoresDrawTheSamePixelsAsOne)
{
    for (const Case& c : kCases)
    {
        const std::vector<uint8_t> src = Source(c.w, c.h, c.alpha);
        QuadBlit::RegisterParallelRows(nullptr, 0);
        const std::vector<uint8_t> one = Draw(src, c.w, c.h, c.alpha, c.x, c.y, c.dw, c.dh, c.tint);
        QuadBlit::RegisterParallelRows(&SplitOnThread, 1000);
        g_Splits = 0;
        const std::vector<uint8_t> two = Draw(src, c.w, c.h, c.alpha, c.x, c.y, c.dw, c.dh, c.tint);
        EXPECT_EQ(g_Splits.load(), 1) << c.name;
        EXPECT_EQ(one, two) << c.name;
    }
}

TEST_F(ParallelRowsTest, ASmallBlitStaysOnOneCore)
{
    QuadBlit::RegisterParallelRows(&SplitOnThread, 1 << 20);
    const std::vector<uint8_t> src = Source(40, 30, false);
    Draw(src, 40, 30, false, 0, 0, 40, 30, 255);
    EXPECT_EQ(g_Splits.load(), 0);
}

// --- The rectangle copy (RegisterRectCopy) ---

namespace
{

int g_RectCalls = 0;
QuadBlit::RectCopyResult g_RectAnswer = QuadBlit::RectCopyResult::Done;
bool g_RectMayRun = false;
int32_t g_RectW = 0, g_RectH = 0, g_RectSrcStride = 0, g_RectDstStride = 0;

// A copy left running: its rows are copied only as they are waited for, so a
// write that did not wait for its rows would be painted over.
struct Running
{
    const uint8_t* src = nullptr;
    int32_t srcStride = 0;
    uint16_t* dst = nullptr;
    int32_t dstStride = 0;
    int32_t width = 0, height = 0, done = 0;
} g_Running;
int g_Waits = 0;

void CopyRows(const uint8_t* src, int32_t srcStrideBytes, uint16_t* dst, int32_t dstStridePixels, int32_t width,
              int32_t y0, int32_t y1)
{
    for (int32_t y = y0; y < y1; ++y)
    {
        std::memcpy(dst + y * dstStridePixels, src + y * srcStrideBytes, static_cast<size_t>(width) * 2);
    }
}

QuadBlit::RectCopyResult RectCopy(const uint8_t* src, int32_t srcStrideBytes, uint16_t* dst, int32_t dstStridePixels,
                                  int32_t width, int32_t height, bool mayRun)
{
    ++g_RectCalls;
    g_RectW = width;
    g_RectH = height;
    g_RectSrcStride = srcStrideBytes;
    g_RectDstStride = dstStridePixels;
    g_RectMayRun = mayRun;
    if (g_RectAnswer == QuadBlit::RectCopyResult::Done ||
        (g_RectAnswer == QuadBlit::RectCopyResult::Running && !mayRun))
    {
        CopyRows(src, srcStrideBytes, dst, dstStridePixels, width, 0, height);
        return QuadBlit::RectCopyResult::Done;
    }
    if (g_RectAnswer == QuadBlit::RectCopyResult::Running)
    {
        g_Running = { src, srcStrideBytes, dst, dstStridePixels, width, height, 0 };
    }
    return g_RectAnswer;
}

void RectWait(int32_t rows)
{
    ++g_Waits;
    const int32_t upTo = rows > g_Running.height ? g_Running.height : rows;
    if (upTo > g_Running.done)
    {
        CopyRows(g_Running.src, g_Running.srcStride, g_Running.dst, g_Running.dstStride, g_Running.width,
                 g_Running.done, upTo);
        g_Running.done = upTo;
    }
}

}  // namespace

TEST_F(ParallelRowsTest, ALargeCopyGoesToTheDmaAsOneRectangle)
{
    // Cut by the target's right and bottom edges: the part inside, once.
    const std::vector<uint8_t> src = Source(300, 180, false);
    const std::vector<uint8_t> one = Draw(src, 300, 180, false, 40, 50, 300, 180, 255);
    QuadBlit::RegisterRectCopy(&RectCopy, &RectWait, 1000);
    g_RectCalls = 0;
    g_RectAnswer = QuadBlit::RectCopyResult::Done;
    EXPECT_EQ(Draw(src, 300, 180, false, 40, 50, 300, 180, 255), one);
    EXPECT_EQ(g_RectCalls, 1);
    EXPECT_EQ(g_RectW, kTW - 40);
    EXPECT_EQ(g_RectH, kTH - 50);
    EXPECT_EQ(g_RectSrcStride, 300 * 2);
    EXPECT_EQ(g_RectDstStride, kTW);
    EXPECT_FALSE(g_RectMayRun);  // not whole rows of the target
    QuadBlit::RegisterRectCopy(nullptr, nullptr, 0);
}

TEST_F(ParallelRowsTest, ACopyTheDmaRefusesIsCopiedOnTheCpu)
{
    const std::vector<uint8_t> src = Source(300, 180, false);
    const std::vector<uint8_t> one = Draw(src, 300, 180, false, 3, 4, 300, 180, 255);
    QuadBlit::RegisterRectCopy(&RectCopy, &RectWait, 1000);
    g_RectCalls = 0;
    g_RectAnswer = QuadBlit::RectCopyResult::Refused;
    EXPECT_EQ(Draw(src, 300, 180, false, 3, 4, 300, 180, 255), one);
    EXPECT_EQ(g_RectCalls, 1);
    // A small one is not offered.
    g_RectCalls = 0;
    const std::vector<uint8_t> small = Source(20, 20, false);
    Draw(small, 20, 20, false, 0, 0, 20, 20, 255);
    EXPECT_EQ(g_RectCalls, 0);
    QuadBlit::RegisterRectCopy(nullptr, nullptr, 0);
}

// A map over whole rows, left running, then sprites over it, high and low: the
// frame is the one drawn with every copy done at once, so each sprite waited
// for the rows under it, and only as far as it needed to.
TEST_F(ParallelRowsTest, ACopyLeftRunningIsWaitedForRowByRow)
{
    const std::vector<uint8_t> map = Source(kTW, kTH, false);
    const std::vector<uint8_t> sprite = Source(40, 30, true);
    auto frame = [&]()
    {
        std::vector<uint8_t> target(static_cast<size_t>(kTW) * kTH * 2, 0);
        QuadBlit::ClearClipStack();
        QuadBlit::Source m = QuadBlit::MakeSource(map.data(), kTW, kTH, QuadBlit::PixelLayout::RGB565());
        QuadBlit::BlitScaled(m, target.data(), kTW, kTH, Deki::ColorFormat::RGB565, 0, 0, kTW, kTH);
        QuadBlit::Source s = QuadBlit::MakeSource(sprite.data(), 40, 30, QuadBlit::PixelLayout::RGB565A8());
        QuadBlit::BlitScaled(s, target.data(), kTW, kTH, Deki::ColorFormat::RGB565, 10, 5, 40, 30);
        const int32_t waitsAfterHigh = g_Waits;
        QuadBlit::BlitScaled(s, target.data(), kTW, kTH, Deki::ColorFormat::RGB565, 100, 120, 80, 60);
        QuadBlit::FinishCopies();
        return std::make_pair(target, waitsAfterHigh);
    };
    QuadBlit::RegisterRectCopy(&RectCopy, &RectWait, 1000);
    g_RectAnswer = QuadBlit::RectCopyResult::Done;
    const std::vector<uint8_t> atOnce = frame().first;
    g_RectAnswer = QuadBlit::RectCopyResult::Running;
    g_Waits = 0;
    g_Running = {};
    const auto [running, waitsAfterHigh] = frame();
    EXPECT_TRUE(g_RectMayRun);
    EXPECT_EQ(running, atOnce);
    // The high sprite waited for its 35 rows, not the whole map.
    EXPECT_EQ(waitsAfterHigh, 1);
    EXPECT_EQ(g_Running.done, kTH);
    QuadBlit::RegisterRectCopy(nullptr, nullptr, 0);
}

TEST_F(ParallelRowsTest, ASplitThatRefusesLeavesTheBlitToOneCore)
{
    const std::vector<uint8_t> src = Source(200, 100, true);
    const std::vector<uint8_t> one = Draw(src, 200, 100, true, 1, 1, 200, 100, 255);
    QuadBlit::RegisterParallelRows(&SplitOnThread, 1000);
    g_Refuse = true;
    EXPECT_EQ(Draw(src, 200, 100, true, 1, 1, 200, 100, 255), one);
}
