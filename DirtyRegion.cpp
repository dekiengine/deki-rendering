#include "DirtyRegion.h"

#include <algorithm>
#include <cmath>

namespace DekiRendering
{

namespace
{
// How much area the bounding box of a and b adds to theirs.
inline int64_t GrowthOf(const Deki::Rect& a, int64_t areaA, const Deki::Rect& b, int64_t areaB)
{
    const int32_t l = std::min(a.left, b.left), t = std::min(a.top, b.top);
    const int32_t r = std::max(a.right, b.right), bt = std::max(a.bottom, b.bottom);
    return static_cast<int64_t>(r - l) * (bt - t) - areaA - areaB;
}
}  // namespace
void DirtyRegion::Reset(int32_t width, int32_t height)
{
    m_Width = width < 0 ? 0 : width;
    m_Height = height < 0 ? 0 : height;
    m_Full = false;
    m_Rects.clear();  // keeps capacity, so no allocation once warmed up
    m_Area = 0;
    UpdateFullAt();
}

void DirtyRegion::SetFullCoverageRatio(float ratio)
{
    m_FullRatio = ratio;
    m_FullAtWidth = -1;  // worked out again
    UpdateFullAt();
}

void DirtyRegion::UpdateFullAt()
{
    // Once for a size and ratio: the comparison CollapseIfCovered makes after
    // every rectangle added is then between whole numbers. (A processor with
    // no double-precision unit spent more on that product than on the rest of
    // Add.)
    if (m_Width == m_FullAtWidth && m_Height == m_FullAtHeight)
    {
        return;
    }
    m_FullAtWidth = m_Width;
    m_FullAtHeight = m_Height;
    const double total = static_cast<double>(static_cast<int64_t>(m_Width) * m_Height);
    m_FullAt = static_cast<int64_t>(std::ceil(total * static_cast<double>(m_FullRatio)));
}

void DirtyRegion::SetFull()
{
    m_Full = true;
    m_Rects.clear();
    m_Area = 0;
}

void DirtyRegion::Add(int32_t left, int32_t top, int32_t right, int32_t bottom)
{
    if (m_Full)
    {
        return;
    }
    left = std::max<int32_t>(left, 0);
    top = std::max<int32_t>(top, 0);
    right = std::min<int32_t>(right, m_Width);
    bottom = std::min<int32_t>(bottom, m_Height);
    if (right <= left || bottom <= top)
    {
        return;
    }

    // A rectangle inside an existing one adds nothing; one that contains an
    // existing one replaces it. Both are common (a sprite drawn twice, a clip
    // child inside its parent's area) and keep the set small without a merge.
    // And one whose bounding box with an existing one is no larger than the
    // two together joins it: a sprite that moved a pixel adds where it was
    // and where it is, which overlap all but a sliver. Drawing the box once
    // costs fewer pixels than drawing both, and the set stays short.
    const Deki::Rect added{ left, top, right, bottom };
    const int64_t area = AreaOf(added);
    Deki::Rect* joins = nullptr;
    for (Deki::Rect& r : m_Rects)
    {
        if (left >= r.left && top >= r.top && right <= r.right && bottom <= r.bottom)
        {
            return;
        }
        if (left <= r.left && top <= r.top && right >= r.right && bottom >= r.bottom)
        {
            m_Area += area - AreaOf(r);
            r = added;
            CollapseIfCovered();
            return;
        }
        if (!joins && GrowthOf(added, area, r, AreaOf(r)) <= 0)
        {
            joins = &r;
        }
    }
    if (joins)
    {
        const Deki::Rect both{ std::min(left, joins->left), std::min(top, joins->top), std::max(right, joins->right),
                               std::max(bottom, joins->bottom) };
        m_Area += AreaOf(both) - AreaOf(*joins);
        *joins = both;
        CollapseIfCovered();
        return;
    }

    m_Rects.push_back(added);
    m_Area += area;
    if (m_Rects.size() > m_MergeThreshold)
    {
        // Down to a quarter under the threshold, so the next few rectangles
        // are added without merging again.
        MergeWhile(INT64_MAX, m_MergeThreshold - m_MergeThreshold / 4);
    }
    CollapseIfCovered();
}

void DirtyRegion::Union(const DirtyRegion& other)
{
    if (m_Full)
    {
        return;
    }
    if (other.m_Full)
    {
        SetFull();
        return;
    }
    for (const Deki::Rect& r : other.m_Rects)
    {
        Add(r.left, r.top, r.right, r.bottom);
    }
}

void DirtyRegion::Align(int32_t granularity)
{
    if (m_Full || granularity <= 1)
    {
        return;
    }
    for (Deki::Rect& r : m_Rects)
    {
        r.left = (r.left / granularity) * granularity;
        r.top = (r.top / granularity) * granularity;
        r.right = std::min<int32_t>(((r.right + granularity - 1) / granularity) * granularity, m_Width);
        r.bottom = std::min<int32_t>(((r.bottom + granularity - 1) / granularity) * granularity, m_Height);
    }
    // Alignment can make neighbours identical or nested; fold those. The
    // rectangles are folded back from a scratch list both keep, so a region
    // aligned every frame allocates nothing once warmed up.
    m_Scratch.swap(m_Rects);
    m_Rects.clear();
    m_Area = 0;
    for (const Deki::Rect& r : m_Scratch)
    {
        Add(r.left, r.top, r.right, r.bottom);
    }
    m_Scratch.clear();
}

int64_t DirtyRegion::CoveredArea() const
{
    if (m_Full)
    {
        return static_cast<int64_t>(m_Width) * m_Height;
    }
    return m_Area;
}

bool DirtyRegion::Contains(int32_t x, int32_t y) const
{
    if (x < 0 || y < 0 || x >= m_Width || y >= m_Height)
    {
        return false;
    }
    if (m_Full)
    {
        return true;
    }
    for (const Deki::Rect& r : m_Rects)
    {
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom)
        {
            return true;
        }
    }
    return false;
}

void DirtyRegion::FindPartner(size_t i)
{
    // The first of the rectangles after i whose union with it grows least.
    const size_t n = m_Rects.size();
    const Deki::Rect& a = m_Rects[i];
    const int64_t areaA = AreaOf(a);
    int64_t best = INT64_MAX;
    uint16_t partner = 0;
    for (size_t j = i + 1; j < n; ++j)
    {
        const int64_t growth = GrowthOf(a, areaA, m_Rects[j], AreaOf(m_Rects[j]));
        if (growth < best)
        {
            best = growth;
            partner = static_cast<uint16_t>(j);
        }
    }
    m_PartnerGrowth[i] = best;
    m_Partner[i] = partner;
}

void DirtyRegion::MergeCheaperThan(int64_t pixels)
{
    MergeWhile(pixels, 0);
}

void DirtyRegion::MergeWhile(int64_t maxGrowth, size_t above)
{
    // Each merge takes the pair whose bounding union adds the least new area,
    // the first such pair in the list's order. Every rectangle keeps the
    // partner after it that it grows least with, so a merge only looks again
    // at the pairs it changed: comparing every pair again after each merge
    // took a millisecond a frame on a board with twenty sprites moving.
    size_t n = m_Rects.size();
    if (m_Full || n < 2 || n <= above)
    {
        return;
    }
    if (n > UINT16_MAX)
    {
        return;  // partners are kept as 16-bit places
    }
    m_Partner.resize(n);
    m_PartnerGrowth.resize(n);
    for (size_t i = 0; i + 1 < n; ++i)
    {
        FindPartner(i);
    }
    while (n >= 2 && n > above)
    {
        size_t a = 0;
        for (size_t i = 1; i + 1 < n; ++i)
        {
            if (m_PartnerGrowth[i] < m_PartnerGrowth[a])
            {
                a = i;
            }
        }
        if (m_PartnerGrowth[a] > maxGrowth)
        {
            break;
        }
        const size_t b = m_Partner[a];
        Deki::Rect& ra = m_Rects[a];
        const Deki::Rect rb = m_Rects[b];
        m_Area -= AreaOf(ra) + AreaOf(rb);
        ra = { std::min(ra.left, rb.left), std::min(ra.top, rb.top), std::max(ra.right, rb.right),
               std::max(ra.bottom, rb.bottom) };
        m_Area += AreaOf(ra);
        const auto at = static_cast<std::ptrdiff_t>(b);
        m_Rects.erase(m_Rects.begin() + at);
        m_Partner.erase(m_Partner.begin() + at);
        m_PartnerGrowth.erase(m_PartnerGrowth.begin() + at);
        --n;
        // A rectangle before `a` whose partner was a or b looks for another;
        // any other only has the grown `a` to compare with its partner. One
        // after `a` only lost b. The last has no partner.
        for (size_t i = 0; i + 1 < n; ++i)
        {
            const uint16_t partner = m_Partner[i];
            if (i == a || partner == b || (i < a && partner == a))
            {
                FindPartner(i);
                continue;
            }
            if (partner > b)
            {
                m_Partner[i] = static_cast<uint16_t>(partner - 1);
            }
            if (i < a)
            {
                const int64_t growth = GrowthOf(m_Rects[i], AreaOf(m_Rects[i]), m_Rects[a], AreaOf(m_Rects[a]));
                if (growth < m_PartnerGrowth[i] || (growth == m_PartnerGrowth[i] && a < m_Partner[i]))
                {
                    m_PartnerGrowth[i] = growth;
                    m_Partner[i] = static_cast<uint16_t>(a);
                }
            }
        }
    }
}
void DirtyRegion::CollapseIfCovered()
{
    if (m_Full || m_Width <= 0 || m_Height <= 0)
    {
        return;
    }
    if (m_Area >= m_FullAt)
    {
        SetFull();
    }
}

}  // namespace DekiRendering
