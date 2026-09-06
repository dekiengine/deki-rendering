/**
 * @file DirtyRegionTests.cpp
 * @brief DirtyRegion: clipping, containment folding, the merge and
 *        full-coverage policies, alignment, union.
 */

#include <gtest/gtest.h>

#include "DirtyRegion.h"

namespace
{
bool Covers(const DirtyRegion& r, int32_t l, int32_t t, int32_t rt, int32_t b)
{
    for (int32_t y = t; y < b; ++y)
        for (int32_t x = l; x < rt; ++x)
            if (!r.Contains(x, y)) return false;
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
    r.Add(0, 0, 30, 30);    // swallows the first
    EXPECT_EQ(r.Rects().size(), 1u);
    EXPECT_EQ(r.Rects()[0].right, 30);
}

TEST(DirtyRegion, MergeThresholdKeepsTheSetSmallAndStillCovering)
{
    DirtyRegion r;
    r.Reset(200, 200);
    r.SetMergeThreshold(3);
    r.SetFullCoverageRatio(2.0f);  // never collapse in this test
    const Deki::Rect added[] = { { 0, 0, 10, 10 }, { 12, 0, 22, 10 }, { 100, 100, 110, 110 }, { 150, 150, 160, 160 }, { 14, 12, 20, 18 } };
    for (const Deki::Rect& a : added) r.Add(a);
    EXPECT_LE(r.Rects().size(), 3u);
    for (const Deki::Rect& a : added)
        EXPECT_TRUE(Covers(r, a.left, a.top, a.right, a.bottom));
    EXPECT_FALSE(r.IsFull());
    // The two neighbours at the top-left merged with each other, not with a far one.
    EXPECT_FALSE(r.Contains(60, 60));
}

TEST(DirtyRegion, CollapsesToFullAboveTheCoverageRatio)
{
    DirtyRegion r;
    r.Reset(100, 100);
    r.SetFullCoverageRatio(0.5f);
    r.Add(0, 0, 60, 60);   // 36%
    EXPECT_FALSE(r.IsFull());
    r.Add(50, 50, 100, 100);  // +25% (overlap counted twice: conservative)
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
    EXPECT_EQ(r.Rects().back().right, 50);   // clamped to the target
    EXPECT_EQ(r.Rects().back().bottom, 40);
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
