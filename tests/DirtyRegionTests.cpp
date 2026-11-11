// DirtyRegion: clipping, folding of contained rectangles, the merge and
// full-coverage policies, alignment, union.

#include <algorithm>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "DirtyRegion.h"

// Tests name the package's types unqualified.
using namespace DekiRendering;

namespace
{
bool Covers(const DirtyRegion& r, int32_t l, int32_t t, int32_t rt, int32_t b)
{
    for (int32_t y = t; y < b; ++y)
    {
        for (int32_t x = l; x < rt; ++x)
        {
            if (!r.Contains(x, y))
            {
                return false;
            }
        }
    }
    return true;
}
}  // namespace

TEST(DirtyRegion, StartsEmptyAndClipsToTheTarget)
{
    DirtyRegion r;
    r.Reset(32, 16);
    EXPECT_TRUE(r.IsEmpty());
    r.Add(-5, -5, 3, 3);
    r.Add(30, 10, 100, 100);
    r.Add(10, 10, 10, 12);  // empty
    ASSERT_EQ(r.Rects().size(), 2u);
    EXPECT_EQ(r.Rects()[0].left, 0);
    EXPECT_EQ(r.Rects()[0].right, 3);
    EXPECT_EQ(r.Rects()[1].right, 32);
    EXPECT_EQ(r.Rects()[1].bottom, 16);
    EXPECT_FALSE(r.Contains(5, 5));
    EXPECT_TRUE(r.Contains(31, 15));
}

TEST(DirtyRegion, ContainedRectanglesFold)
{
    DirtyRegion r;
    r.Reset(64, 64);
    r.Add(10, 10, 20, 20);
    r.Add(12, 12, 14, 14);  // inside: nothing new
    EXPECT_EQ(r.Rects().size(), 1u);
    r.Add(0, 0, 30, 30);  // contains the first
    EXPECT_EQ(r.Rects().size(), 1u);
    EXPECT_EQ(r.Rects()[0].right, 30);
}

TEST(DirtyRegion, MergeThresholdKeepsTheSetSmallAndStillCovering)
{
    DirtyRegion r;
    r.Reset(200, 200);
    r.SetMergeThreshold(3);
    r.SetFullCoverageRatio(2.0f);  // never collapse in this test
    const Deki::Rect added[] = {
        { 0, 0, 10, 10 }, { 12, 0, 22, 10 }, { 100, 100, 110, 110 }, { 150, 150, 160, 160 }, { 14, 12, 20, 18 }
    };
    for (const Deki::Rect& a : added)
    {
        r.Add(a);
    }
    EXPECT_LE(r.Rects().size(), 3u);
    for (const Deki::Rect& a : added)
    {
        EXPECT_TRUE(Covers(r, a.left, a.top, a.right, a.bottom));
    }
    EXPECT_FALSE(r.IsFull());
    // The two neighbours at the top-left merged with each other, not with a far one.
    EXPECT_FALSE(r.Contains(60, 60));
}

TEST(DirtyRegion, CollapsesToFullAboveTheCoverageRatio)
{
    DirtyRegion r;
    r.Reset(100, 100);
    r.SetFullCoverageRatio(0.5f);
    r.Add(0, 0, 60, 60);  // 36%
    EXPECT_FALSE(r.IsFull());
    r.Add(50, 50, 100, 100);  // +25% (the overlap counts twice)
    EXPECT_TRUE(r.IsFull());
    EXPECT_TRUE(r.Rects().empty());
    EXPECT_TRUE(r.Contains(99, 0));
}

TEST(DirtyRegion, AlignRoundsOutwardsAndClamps)
{
    DirtyRegion r;
    r.Reset(50, 40);
    r.Add(5, 6, 17, 9);
    r.Align(8);
    ASSERT_EQ(r.Rects().size(), 1u);
    EXPECT_EQ(r.Rects()[0].left, 0);
    EXPECT_EQ(r.Rects()[0].top, 0);
    EXPECT_EQ(r.Rects()[0].right, 24);
    EXPECT_EQ(r.Rects()[0].bottom, 16);
    r.Add(45, 35, 50, 40);
    r.Align(16);
    EXPECT_EQ(r.Rects().back().right, 50);  // clamped to the target
    EXPECT_EQ(r.Rects().back().bottom, 40);
}

// A region reset, filled and aligned every frame, as the renderer does, keeps
// its lists: the rectangles live in the same two buffers frame after frame.
TEST(DirtyRegion, AligningEveryFrameKeepsItsBuffers)
{
    DirtyRegion r;
    std::vector<const Deki::Rect*> buffers;
    for (int frame = 0; frame < 12; ++frame)
    {
        r.Reset(320, 240);
        for (int i = 0; i < 12; ++i)
        {
            r.Add(i * 25, i * 18, i * 25 + 9, i * 18 + 7);
        }
        r.Align(4);
        ASSERT_EQ(r.Rects().size(), 12u);
        const Deki::Rect* data = r.Rects().data();
        if (std::find(buffers.begin(), buffers.end(), data) == buffers.end())
        {
            buffers.push_back(data);
        }
    }
    EXPECT_LE(buffers.size(), 2u);
}

TEST(DirtyRegion, UnionAddsTheOtherSet)
{
    DirtyRegion a, b;
    a.Reset(64, 64);
    b.Reset(64, 64);
    a.Add(0, 0, 8, 8);
    b.Add(40, 40, 48, 48);
    a.Union(b);
    EXPECT_TRUE(a.Contains(4, 4));
    EXPECT_TRUE(a.Contains(44, 44));
    EXPECT_FALSE(a.Contains(20, 20));
    b.SetFull();
    a.Union(b);
    EXPECT_TRUE(a.IsFull());
}

TEST(DirtyRegion, CoveredAreaSumsRectangles)
{
    DirtyRegion r;
    r.Reset(64, 64);
    r.SetFullCoverageRatio(2.0f);
    r.Add(0, 0, 8, 8);
    r.Add(8, 8, 12, 12);
    EXPECT_EQ(r.CoveredArea(), 64 + 16);
    r.SetFull();
    EXPECT_EQ(r.CoveredArea(), 64 * 64);
}

TEST(DirtyRegion, MergesOnlyWhatCostsLessThanAPart)
{
    DirtyRegion r;
    r.Reset(320, 240);
    r.Add(0, 0, 10, 10);
    r.Add(12, 0, 22, 10);       // 20 new pixels to merge with the first
    r.Add(200, 200, 210, 210);  // far: thousands
    r.MergeCheaperThan(100);
    ASSERT_EQ(r.Rects().size(), 2u);
    EXPECT_TRUE(r.Contains(11, 5)) << "the gap, merged";
    EXPECT_FALSE(r.Contains(100, 100));
    r.MergeCheaperThan(0);
    EXPECT_EQ(r.Rects().size(), 2u) << "nothing is free to merge";
}

namespace
{
// The merge DirtyRegion makes, the plain way: every pair compared again after
// each merge, the first pair that grows least taken.
void MergeTheSlowWay(std::vector<Deki::Rect>& rects, int64_t maxGrowth, size_t above)
{
    while (rects.size() >= 2 && rects.size() > above)
    {
        size_t bestA = 0, bestB = 1;
        int64_t bestGrowth = INT64_MAX;
        for (size_t i = 0; i < rects.size(); ++i)
        {
            const Deki::Rect& a = rects[i];
            for (size_t j = i + 1; j < rects.size(); ++j)
            {
                const Deki::Rect& b = rects[j];
                const int64_t unionArea = static_cast<int64_t>(std::max(a.right, b.right) - std::min(a.left, b.left)) *
                                          (std::max(a.bottom, b.bottom) - std::min(a.top, b.top));
                const int64_t growth = unionArea - static_cast<int64_t>(a.Width()) * a.Height() -
                                       static_cast<int64_t>(b.Width()) * b.Height();
                if (growth < bestGrowth)
                {
                    bestGrowth = growth;
                    bestA = i;
                    bestB = j;
                }
            }
        }
        if (bestGrowth > maxGrowth)
        {
            return;
        }
        Deki::Rect& a = rects[bestA];
        const Deki::Rect b = rects[bestB];
        a = { std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right),
              std::max(a.bottom, b.bottom) };
        rects.erase(rects.begin() + static_cast<std::ptrdiff_t>(bestB));
    }
}

bool SameRects(const std::vector<Deki::Rect>& a, const std::vector<Deki::Rect>& b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (a[i].left != b[i].left || a[i].top != b[i].top || a[i].right != b[i].right || a[i].bottom != b[i].bottom)
        {
            return false;
        }
    }
    return true;
}
}  // namespace

TEST(DirtyRegion, MergesTheSamePairsAsComparingEveryPairAgain)
{
    uint32_t seed = 2463534242u;
    auto next = [&seed](uint32_t below)
    {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return static_cast<int32_t>(seed % below);
    };
    for (int round = 0; round < 400; ++round)
    {
        DirtyRegion region;
        region.Reset(2000, 2000);
        region.SetFullCoverageRatio(2.0f);  // never full: the merges are what is compared
        region.SetMergeThreshold(64);
        const int count = 2 + next(22);
        for (int i = 0; i < count; ++i)
        {
            // Small sizes and a coarse grid, so equal growths are common.
            const int32_t l = next(60) * 8, t = next(60) * 8;
            region.Add(l, t, l + 8 + next(6) * 8, t + 8 + next(6) * 8);
        }
        std::vector<Deki::Rect> expected = region.Rects();
        const int64_t pixels = round % 5 == 0 ? INT64_MAX : next(6000);
        MergeTheSlowWay(expected, pixels, 0);
        region.MergeCheaperThan(pixels);
        ASSERT_TRUE(SameRects(region.Rects(), expected)) << "round " << round;
        int64_t area = 0;
        for (const Deki::Rect& r : region.Rects())
        {
            area += static_cast<int64_t>(r.Width()) * r.Height();
        }
        EXPECT_EQ(region.CoveredArea(), area) << "round " << round;
    }
}

TEST(DirtyRegion, TheThresholdMergesTheSamePairsAsComparingEveryPairAgain)
{
    uint32_t seed = 88172645u;
    auto next = [&seed](uint32_t below)
    {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return static_cast<int32_t>(seed % below);
    };
    for (int round = 0; round < 200; ++round)
    {
        DirtyRegion region;
        region.Reset(4000, 4000);
        region.SetFullCoverageRatio(2.0f);
        region.SetMergeThreshold(6);
        std::vector<Deki::Rect> expected;
        for (int i = 0; i < 30; ++i)
        {
            const int32_t l = next(480) * 8, t = next(480) * 8;
            const Deki::Rect r{ l, t, l + 8 + next(4) * 8, t + 8 + next(4) * 8 };
            // As Add: inside one already there adds nothing, around one
            // replaces it, and one whose box with another is no larger than
            // the two joins the first such.
            bool folded = false;
            Deki::Rect* joins = nullptr;
            for (Deki::Rect& e : expected)
            {
                if (r.left >= e.left && r.top >= e.top && r.right <= e.right && r.bottom <= e.bottom)
                {
                    folded = true;
                    break;
                }
                if (r.left <= e.left && r.top <= e.top && r.right >= e.right && r.bottom >= e.bottom)
                {
                    e = r;
                    folded = true;
                    break;
                }
                const Deki::Rect both{ std::min(r.left, e.left), std::min(r.top, e.top), std::max(r.right, e.right),
                                       std::max(r.bottom, e.bottom) };
                if (!joins &&
                    static_cast<int64_t>(both.Width()) * both.Height() <=
                        static_cast<int64_t>(r.Width()) * r.Height() + static_cast<int64_t>(e.Width()) * e.Height())
                {
                    joins = &e;
                }
            }
            if (!folded && joins)
            {
                *joins = Deki::Rect{ std::min(r.left, joins->left), std::min(r.top, joins->top),
                                     std::max(r.right, joins->right), std::max(r.bottom, joins->bottom) };
            }
            else if (!folded)
            {
                expected.push_back(r);
                if (expected.size() > 6)
                {
                    MergeTheSlowWay(expected, INT64_MAX, 5);  // a quarter under the threshold
                }
            }
            region.Add(r);
            ASSERT_TRUE(SameRects(region.Rects(), expected)) << "round " << round << " rect " << i;
        }
    }
}

TEST(DirtyRegion, FullAtTheSameAreaWhateverTheRatio)
{
    // The coverage test is made between whole numbers: full exactly when the
    // area reaches the ratio of the target.
    for (float ratio : { 0.1f, 0.33f, 0.5f, 0.8f, 0.999f, 1.0f })
    {
        for (int32_t rows = 1; rows <= 100; ++rows)
        {
            DirtyRegion region;
            region.SetFullCoverageRatio(ratio);
            region.Reset(37, 100);
            region.Add(0, 0, 37, rows);
            const bool full = static_cast<double>(37 * rows) >= static_cast<double>(3700) * ratio;
            EXPECT_EQ(region.IsFull(), full) << "ratio " << ratio << " rows " << rows;
        }
    }
}
TEST(DirtyRegion, ASpriteThatMovedAPixelIsOneRectangle)
{
    DirtyRegion region;
    region.Reset(320, 240);
    region.Add(100, 100, 120, 130);  // where it was
    region.Add(101, 100, 121, 130);  // where it is
    ASSERT_EQ(region.Rects().size(), 1u);
    EXPECT_EQ(region.Rects()[0].left, 100);
    EXPECT_EQ(region.Rects()[0].right, 121);
    EXPECT_EQ(region.CoveredArea(), 21 * 30);
    // Two that only touch at a corner stay apart: their box is mostly neither.
    region.Add(121, 130, 141, 160);
    EXPECT_EQ(region.Rects().size(), 2u);
}