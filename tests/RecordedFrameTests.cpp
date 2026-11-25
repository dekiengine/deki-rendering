// A frame's blits recorded (BeginRecording) and played back in strips
// (PlayRecording) give the pixels drawing them would: every kind of blit, a
// clip, a fill, in strips of any height, two strips at once on two threads.
// Anything written into the frame other than by a blit gives the recording
// up, and pixels freed after their blit are kept for the playing back.

#include <gtest/gtest.h>

#include <deki/Engine.h>
#include "PixelFormat.h"
#include "QuadBlit.h"

#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace
{

constexpr int kW = 120, kH = 90;
constexpr uint16_t kClear = 0x1234;

std::vector<uint8_t> Picture(int w, int h, bool alpha, uint16_t seed)
{
    const int bpp = alpha ? 3 : 2;
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * bpp);
    for (int i = 0; i < w * h; ++i)
    {
        DekiPixel::Store565(&px[static_cast<size_t>(i) * bpp], static_cast<uint16_t>(seed * (i + 3)));
        if (alpha)
        {
            const int a = (i / 5) % 3;
            px[static_cast<size_t>(i) * bpp + 2] = static_cast<uint8_t>(a == 0 ? 0 : (a == 1 ? 255 : 90));
        }
    }
    return px;
}

struct Pictures
{
    std::vector<uint8_t> opaque = Picture(17, 11, false, 0x2C51);
    std::vector<uint8_t> clear = Picture(23, 19, true, 0x1F3B);
    std::vector<uint8_t> runs;
    std::vector<uint8_t> glyphs;  // one byte a pixel, as baked text
    Pictures()
    {
        glyphs.resize(37 * 13);
        for (size_t i = 0; i < glyphs.size(); ++i)
        {
            glyphs[i] = static_cast<uint8_t>((i * 7) % 5 == 0 ? 0 : ((i * 13) % 3 == 0 ? 255 : 40 + (i * 29) % 200));
        }
        runs.resize(64 * 1024);
        runs.resize(QuadBlit::PackRuns(clear.data(), 23, 19, 23 * 3, false, false, runs.data(), runs.size()));
    }
};

// The frame's blits: a clear, an opaque picture at 2x (and 3x, clipped), one
// with alpha 1:1 and scaled, tinted, turned, and drawn from runs.
void Draw(const Pictures& p, uint8_t* target, bool recording, bool ownsPixels = false)
{
    using QuadBlit::PixelLayout;
    QuadBlit::ClearClipStack();
    uint8_t pixel[4] = {};
    DekiPixel::Store565(pixel, kClear);
    if (recording)
    {
        QuadBlit::RecordFill(0, 0, kW, kH, pixel);
    }
    else
    {
        for (int i = 0; i < kW * kH; ++i)
        {
            std::memcpy(target + i * 2, pixel, 2);
        }
    }
    QuadBlit::Source opaque = QuadBlit::MakeSource(p.opaque.data(), 17, 11, PixelLayout::RGB565());
    QuadBlit::Source clear = QuadBlit::MakeSource(p.clear.data(), 23, 19, PixelLayout::RGB565A8());
    clear.ownsPixels = ownsPixels;
    QuadBlit::BlitScaled(opaque, target, kW, kH, Deki::ColorFormat::RGB565, -5, 3, 34, 22);
    QuadBlit::PushClipRect(10, 20, 90, 61);
    QuadBlit::BlitScaled(opaque, target, kW, kH, Deki::ColorFormat::RGB565, 13, 15, 51, 33);
    QuadBlit::BlitScaled(clear, target, kW, kH, Deki::ColorFormat::RGB565, 40, 30, 23, 19);
    QuadBlit::PopClipRect();
    QuadBlit::BlitScaled(clear, target, kW, kH, Deki::ColorFormat::RGB565, 70, 50, 46, 38, 200, 120, 255, 255);
    QuadBlit::BlitScaled(clear, target, kW, kH, Deki::ColorFormat::RGB565, 5, 60, 31, 25, 255, 255, 255, 140);
    QuadBlit::Blit(clear, target, kW, kH, Deki::ColorFormat::RGB565, 60, 20, 1.5f, 1.5f, 0.6f, 0.5f, 0.5f);
    QuadBlit::BlitRuns(p.runs.data(), 23, 19, target, kW, kH, 95, 2, 255, 255, 255, 255);
    // Text: a one-byte-a-pixel bake in one colour, 1:1, and at a tint's alpha.
    QuadBlit::Source text = QuadBlit::MakeSource(p.glyphs.data(), 37, 13, PixelLayout::Alpha8());
    QuadBlit::BlitScaled(text, target, kW, kH, Deki::ColorFormat::RGB565, 41, 61, 37, 13, 140, 150, 170, 255);
    QuadBlit::BlitScaled(text, target, kW, kH, Deki::ColorFormat::RGB565, 3, 40, 37, 13, 255, 255, 255, 120);
}

std::vector<uint8_t> Drawn(const Pictures& p)
{
    std::vector<uint8_t> frame(static_cast<size_t>(kW) * kH * 2, 0xEE);
    Draw(p, frame.data(), false);
    return frame;
}

// Rows [top, top + rows) of the recording, played into a strip of their own
// and copied into `frame`.
void PlayInto(const QuadBlit::RecordedFrame& rec, std::vector<uint8_t>& frame, int top, int rows, int lane)
{
    rows = std::min(rows, kH - top);
    std::vector<uint8_t> strip(static_cast<size_t>(kW) * rows * 2, 0x77);
    QuadBlit::PlayRecording(rec, strip.data(), top, top + rows, lane);
    std::memcpy(&frame[static_cast<size_t>(top) * kW * 2], strip.data(), strip.size());
}

}  // namespace

TEST(RecordedFrames, PlayedBackInStripsTheBlitsMatchDrawingThem)
{
    const Pictures p;
    const std::vector<uint8_t> expected = Drawn(p);
    std::vector<uint8_t> stand(static_cast<size_t>(kW) * kH * 2, 0);
    ASSERT_TRUE(QuadBlit::BeginRecording(stand.data(), kW, kH, Deki::ColorFormat::RGB565));
    EXPECT_TRUE(QuadBlit::IsRecording(stand.data()));
    Draw(p, stand.data(), true);
    const char* why = nullptr;
    const QuadBlit::RecordedFrame* rec = QuadBlit::EndRecording(&why);
    ASSERT_NE(rec, nullptr) << (why ? why : "");
    EXPECT_FALSE(QuadBlit::IsRecording(stand.data()));
    EXPECT_GT(QuadBlit::RecordedCount(*rec), 6u);
    for (uint8_t b : stand)
    {
        ASSERT_EQ(b, 0) << "nothing drawn into the frame recorded";
    }

    for (int rows : { 1, 7, 16, 48, kH })
    {
        std::vector<uint8_t> frame(expected.size(), 0x55);
        for (int top = 0, lane = 0; top < kH; top += rows, lane ^= 1)
        {
            PlayInto(*rec, frame, top, rows, lane);
        }
        EXPECT_EQ(frame, expected) << "strips of " << rows;
    }

    // Two strips at a time, the second on another thread.
    for (int rows : { 5, 12 })
    {
        std::vector<uint8_t> frame(expected.size(), 0x55);
        for (int top = 0; top < kH; top += 2 * rows)
        {
            std::vector<uint8_t> a = frame, b = frame;
            std::thread other([&] {
                if (top + rows < kH)
                {
                    PlayInto(*rec, b, top + rows, rows, 1);
                }
            });
            PlayInto(*rec, a, top, rows, 0);
            other.join();
            const int aRows = std::min(rows, kH - top);
            std::memcpy(&frame[static_cast<size_t>(top) * kW * 2], &a[static_cast<size_t>(top) * kW * 2],
                        static_cast<size_t>(aRows) * kW * 2);
            if (top + rows < kH)
            {
                const int bRows = std::min(rows, kH - top - rows);
                std::memcpy(&frame[static_cast<size_t>(top + rows) * kW * 2],
                            &b[static_cast<size_t>(top + rows) * kW * 2], static_cast<size_t>(bRows) * kW * 2);
            }
        }
        EXPECT_EQ(frame, expected) << "two at a time, strips of " << rows;
    }
    QuadBlit::FlushPlayStats();
}

// Played back only between two columns, the blits draw there what they draw
// in the whole rows, and nothing on either side.
TEST(RecordedFrames, PlayedBackBetweenColumnsOnlyThoseAreDrawn)
{
    const Pictures p;
    const std::vector<uint8_t> expected = Drawn(p);
    std::vector<uint8_t> stand(static_cast<size_t>(kW) * kH * 2, 0);
    ASSERT_TRUE(QuadBlit::BeginRecording(stand.data(), kW, kH, Deki::ColorFormat::RGB565));
    Draw(p, stand.data(), true);
    const QuadBlit::RecordedFrame* rec = QuadBlit::EndRecording();
    ASSERT_NE(rec, nullptr);
    for (const auto& cols : { std::pair<int, int>{ 0, kW }, { 7, 61 }, { 33, 34 }, { 90, kW + 40 }, { -10, 12 } })
    {
        for (int rows : { 5, kH })
        {
            for (int top = 0; top < kH; top += rows)
            {
                const int n = std::min(rows, kH - top);
                std::vector<uint8_t> strip(static_cast<size_t>(kW) * n * 2, 0x77);
                QuadBlit::PlayRecording(*rec, strip.data(), top, top + n, 0, cols.first, cols.second);
                for (int y = 0; y < n; ++y)
                {
                    for (int x = 0; x < kW; ++x)
                    {
                        const size_t at = (static_cast<size_t>(y) * kW + x) * 2;
                        const size_t whole = (static_cast<size_t>(top + y) * kW + x) * 2;
                        const bool inside = x >= cols.first && x < cols.second;
                        const uint8_t want0 = inside ? expected[whole] : 0x77;
                        const uint8_t want1 = inside ? expected[whole + 1] : 0x77;
                        ASSERT_EQ(strip[at], want0) << "columns " << cols.first << ".." << cols.second << ", x " << x
                                                    << ", y " << top + y;
                        ASSERT_EQ(strip[at + 1], want1);
                    }
                }
            }
        }
    }
    QuadBlit::FlushPlayStats();
}

TEST(RecordedFrames, WritingTheFrameOtherThanByBlitsGivesTheRecordingUp)
{
    std::vector<uint8_t> stand(static_cast<size_t>(kW) * kH * 2, 0);
    for (int way = 0; way < 3; ++way)
    {
        ASSERT_TRUE(QuadBlit::BeginRecording(stand.data(), kW, kH, Deki::ColorFormat::RGB565));
        if (way == 0)
        {
            QuadBlit::BeforeWriting(stand.data());
        }
        else if (way == 1)
        {
            QuadBlit::NoteWritten(stand.data());
        }
        else
        {
            QuadBlit::AbandonRecording("asked");
        }
        const char* why = nullptr;
        EXPECT_EQ(QuadBlit::EndRecording(&why), nullptr) << way;
        EXPECT_NE(why, nullptr);
    }
    // Writing another buffer does not.
    std::vector<uint8_t> other(16);
    ASSERT_TRUE(QuadBlit::BeginRecording(stand.data(), kW, kH, Deki::ColorFormat::RGB565));
    QuadBlit::BeforeWriting(other.data());
    QuadBlit::NoteWritten(other.data());
    EXPECT_NE(QuadBlit::EndRecording(), nullptr);
}

TEST(RecordedFrames, PixelsFreedAfterTheirBlitAreKept)
{
    Pictures p;
    const std::vector<uint8_t> expected = Drawn(p);
    std::vector<uint8_t> stand(static_cast<size_t>(kW) * kH * 2, 0);
    ASSERT_TRUE(QuadBlit::BeginRecording(stand.data(), kW, kH, Deki::ColorFormat::RGB565));
    Draw(p, stand.data(), true, /*ownsPixels=*/true);
    const QuadBlit::RecordedFrame* rec = QuadBlit::EndRecording();
    ASSERT_NE(rec, nullptr);
    // The renderer frees them; here they are written over instead.
    std::fill(p.clear.begin(), p.clear.end(), uint8_t{ 0x99 });
    std::vector<uint8_t> frame(expected.size(), 0x55);
    for (int top = 0; top < kH; top += 10)
    {
        PlayInto(*rec, frame, top, 10, 0);
    }
    EXPECT_EQ(frame, expected);
}
