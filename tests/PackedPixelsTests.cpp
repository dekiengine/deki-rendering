// Pictures kept as runs: QuadBlit::PackRuns and BlitRuns draw what BlitScaled
// draws of the picture itself, and PackedPixels keeps them.

#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include <deki/Engine.h>  // Deki::ColorFormat

#include "PackedPixels.h"
#include "QuadBlit.h"

// Tests name the package's types unqualified.
using namespace DekiRendering;

namespace
{
uint32_t s_Seed = 0;
int32_t Next(uint32_t below)
{
    s_Seed ^= s_Seed << 13;
    s_Seed ^= s_Seed >> 17;
    s_Seed ^= s_Seed << 5;
    return static_cast<int32_t>(s_Seed % below);
}

// An RGB565A8 sheet of noise whose alpha comes in stretches of clear, opaque
// and soft pixels, as a sprite's does.
std::vector<uint8_t> Sheet(int32_t width, int32_t height)
{
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 3);
    int32_t left = 0;
    uint8_t alpha = 0;
    for (size_t i = 0; i < pixels.size(); i += 3)
    {
        if (left == 0)
        {
            left = 1 + Next(9);
            const int32_t kind = Next(4);
            alpha = kind == 0 ? 0 : kind == 1 ? 255 : static_cast<uint8_t>(1 + Next(254));
        }
        --left;
        pixels[i] = static_cast<uint8_t>(Next(256));
        pixels[i + 1] = static_cast<uint8_t>(Next(256));
        pixels[i + 2] = alpha;
    }
    return pixels;
}

// A target of noise, so a pixel wrongly written or wrongly left shows.
std::vector<uint16_t> Target(int32_t width, int32_t height)
{
    std::vector<uint16_t> pixels(static_cast<size_t>(width) * height);
    for (uint16_t& p : pixels)
    {
        p = static_cast<uint16_t>(Next(65536));
    }
    return pixels;
}
}  // namespace

TEST(PackedPixels, RunsDrawWhatThePictureDraws)
{
    s_Seed = 2463534242u;
    constexpr int32_t kTargetW = 64, kTargetH = 48;
    for (int round = 0; round < 300; ++round)
    {
        // A box inside a wider sheet.
        const int32_t sheetW = 40 + Next(60), sheetH = 30 + Next(40);
        const std::vector<uint8_t> sheet = Sheet(sheetW, sheetH);
        const int32_t w = 1 + Next(30), h = 1 + Next(25);
        const int32_t x = Next(sheetW - w), y = Next(sheetH - h);
        const uint8_t* first = sheet.data() + (static_cast<size_t>(y) * sheetW + x) * 3;

        std::vector<uint8_t> runs(static_cast<size_t>(w) * h * 5 + static_cast<size_t>(h) * 2 + 16);
        const size_t bytes = QuadBlit::PackRuns(first, w, h, sheetW * 3, runs.data(), runs.size());
        ASSERT_GT(bytes, 0u) << "round " << round;

        // Drawn anywhere, partly off the target, inside a clip or not.
        const std::vector<uint16_t> before = Target(kTargetW, kTargetH);
        std::vector<uint16_t> fromPicture = before, fromRuns = before;
        const int32_t destX = Next(kTargetW + 40) - 20, destY = Next(kTargetH + 30) - 15;
        const bool clipped = round % 3 != 0;
        const int32_t cl = Next(kTargetW), ct = Next(kTargetH);
        const int32_t cr = cl + 1 + Next(kTargetW), cb = ct + 1 + Next(kTargetH);

        QuadBlit::Source source = QuadBlit::MakeSource(first, w, h, QuadBlit::PixelLayout::RGB565A8());
        source.stride = sheetW * 3;
        if (clipped)
        {
            QuadBlit::PushClipRect(cl, ct, cr, cb);
        }
        QuadBlit::BlitScaled(source, reinterpret_cast<uint8_t*>(fromPicture.data()), kTargetW, kTargetH,
                             Deki::ColorFormat::RGB565, destX, destY, w, h);
        QuadBlit::BlitRuns(runs.data(), w, h, reinterpret_cast<uint8_t*>(fromRuns.data()), kTargetW, kTargetH, destX,
                           destY);
        if (clipped)
        {
            QuadBlit::PopClipRect();
        }
        ASSERT_EQ(std::memcmp(fromPicture.data(), fromRuns.data(), fromRuns.size() * sizeof(uint16_t)), 0)
            << "round " << round << ": " << w << "x" << h << " at " << destX << "," << destY;
    }
}

TEST(PackedPixels, RunsThatDoNotFitAreNotWritten)
{
    s_Seed = 88172645u;
    const std::vector<uint8_t> sheet = Sheet(20, 20);
    std::vector<uint8_t> runs(4096);
    const size_t bytes = QuadBlit::PackRuns(sheet.data(), 20, 20, 60, runs.data(), runs.size());
    ASSERT_GT(bytes, 0u);
    EXPECT_EQ(QuadBlit::PackRuns(sheet.data(), 20, 20, 60, runs.data(), bytes - 1), 0u);
    EXPECT_EQ(QuadBlit::PackRuns(sheet.data(), 20, 20, 60, runs.data(), 10), 0u);
}

TEST(PackedPixels, KeepsEachPicturesRunsApart)
{
    s_Seed = 123456789u;
    const std::vector<uint8_t> sheet = Sheet(64, 64);
    PackedPixels packed;
    const uint8_t* a = packed.RunsOf(sheet.data(), 1, 16, 16, 64 * 3);
    const uint8_t* b = packed.RunsOf(sheet.data() + 16 * 3, 1, 15, 17, 64 * 3);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a, b);

    // The first still draws as it did before the second was added.
    std::vector<uint8_t> alone(4096);
    const size_t bytes = QuadBlit::PackRuns(sheet.data(), 16, 16, 64 * 3, alone.data(), alone.size());
    ASSERT_GT(bytes, 0u);
    EXPECT_EQ(std::memcmp(a, alone.data(), bytes), 0);
    EXPECT_GE(packed.Used(), bytes);
}

TEST(PackedPixels, LeavesAPictureLargerThanTheLargest)
{
    s_Seed = 362436069u;
    const std::vector<uint8_t> sheet = Sheet(80, 80);
    PackedPixels packed;
    EXPECT_EQ(packed.RunsOf(sheet.data(), 1, 65, 64, 80 * 3), nullptr);
    EXPECT_NE(packed.RunsOf(sheet.data(), 1, 64, 64, 80 * 3), nullptr);
}

TEST(PackedPixels, TakesAnotherBlockWhenOneIsFull)
{
    s_Seed = 521288629u;
    const std::vector<uint8_t> sheet = Sheet(64, 64);
    PackedPixels packed;
    uint32_t revision = 1;
    const uint8_t* first = packed.RunsOf(sheet.data(), revision++, 64, 64, 64 * 3);
    ASSERT_NE(first, nullptr);
    std::vector<uint8_t> alone(packed.Used());
    std::memcpy(alone.data(), first, alone.size());

    // Past one block: every picture still has its runs, and the first is
    // where it was, as it was.
    while (packed.Used() <= PackedPixels::kBlockBytes)
    {
        ASSERT_NE(packed.RunsOf(sheet.data(), revision++, 64, 64, 64 * 3), nullptr);
    }
    EXPECT_EQ(std::memcmp(first, alone.data(), alone.size()), 0);
}

TEST(PackedPixels, AddsNoMoreAtTheMostBlocksUntilItIsCleared)
{
    s_Seed = 521288629u;
    const std::vector<uint8_t> sheet = Sheet(64, 64);
    PackedPixels packed;
    int added = 0;
    while (packed.RunsOf(sheet.data(), static_cast<uint32_t>(added + 1), 64, 64, 64 * 3) && added < 100000)
    {
        ++added;
    }
    EXPECT_GT(added, static_cast<int>(PackedPixels::kMostBlocks));
    EXPECT_LT(added, 100000);
    EXPECT_LE(packed.Used(), PackedPixels::kBlockBytes * PackedPixels::kMostBlocks);

    const uint32_t before = packed.Generation();
    packed.Clear();
    EXPECT_NE(packed.Generation(), before);
    EXPECT_EQ(packed.Used(), 0u);
    EXPECT_NE(packed.RunsOf(sheet.data(), 1, 64, 64, 64 * 3), nullptr);
}

TEST(PackedPixels, APictureAskedForAgainHasTheSameRuns)
{
    s_Seed = 886756453u;
    const std::vector<uint8_t> sheet = Sheet(64, 64);
    PackedPixels packed;
    const uint8_t* runs = packed.RunsOf(sheet.data(), 7, 20, 20, 64 * 3);
    ASSERT_NE(runs, nullptr);
    const size_t used = packed.Used();
    EXPECT_EQ(packed.RunsOf(sheet.data(), 7, 20, 20, 64 * 3), runs);
    EXPECT_EQ(packed.Used(), used);
    // Another revision, size or place is another picture.
    EXPECT_NE(packed.RunsOf(sheet.data(), 8, 20, 20, 64 * 3), runs);
    EXPECT_NE(packed.RunsOf(sheet.data(), 7, 20, 19, 64 * 3), runs);
    EXPECT_NE(packed.RunsOf(sheet.data() + 3, 7, 20, 20, 64 * 3), runs);
    EXPECT_GT(packed.Used(), used);
}

TEST(PackedPixels, KeepsNoMoreThanTheMostPictures)
{
    s_Seed = 1234567u;
    const std::vector<uint8_t> sheet = Sheet(8, 8);
    PackedPixels packed;
    for (uint32_t i = 0; i < PackedPixels::kMostPictures; ++i)
    {
        ASSERT_NE(packed.RunsOf(sheet.data(), i + 1, 4, 4, 8 * 3), nullptr) << "picture " << i;
    }
    EXPECT_EQ(packed.RunsOf(sheet.data(), 100000, 4, 4, 8 * 3), nullptr);
    // One it holds is still found.
    EXPECT_NE(packed.RunsOf(sheet.data(), 1, 4, 4, 8 * 3), nullptr);
}