#include "DirtyRegion.h"

#include <algorithm>

void DirtyRegion::Reset(int32_t width, int32_t height)
{
    m_Width = width < 0 ? 0 : width;
    m_Height = height < 0 ? 0 : height;
    m_Full = false;
    m_Rects.clear();  // keeps capacity: no allocation once warm
}

void DirtyRegion::SetFull()
{
    m_Full = true;
    m_Rects.clear();
}

void DirtyRegion::Add(int32_t left, int32_t top, int32_t right, int32_t bottom)
{
    if (m_Full) return;
    left = std::max<int32_t>(left, 0);
    top = std::max<int32_t>(top, 0);
    right = std::min<int32_t>(right, m_Width);
    bottom = std::min<int32_t>(bottom, m_Height);
    if (right <= left || bottom <= top) return;

    // A rectangle inside one we already have adds nothing; one that swallows
    // an existing rectangle replaces it. Both are common (a sprite drawn
    // twice, a clip child inside its parent's area) and keep the set small
    // without a merge.
    for (DekiRect& r : m_Rects)
    {
        if (left >= r.left && top >= r.top && right <= r.right && bottom <= r.bottom)
            return;
        if (left <= r.left && top <= r.top && right >= r.right && bottom >= r.bottom)
        {
            r = { left, top, right, bottom };
            CollapseIfCovered();
            return;
        }
    }

    m_Rects.push_back({ left, top, right, bottom });
    while (m_Rects.size() > m_MergeThreshold)
        MergeOnePair();
    CollapseIfCovered();
}

void DirtyRegion::Union(const DirtyRegion& other)
{
    if (m_Full) return;
    if (other.m_Full)
    {
        SetFull();
        return;
    }
    for (const DekiRect& r : other.m_Rects)
        Add(r.left, r.top, r.right, r.bottom);
}

void DirtyRegion::Align(int32_t granularity)
{
    if (m_Full || granularity <= 1) return;
    for (DekiRect& r : m_Rects)
    {
        r.left = (r.left / granularity) * granularity;
        r.top = (r.top / granularity) * granularity;
        r.right = std::min<int32_t>(((r.right + granularity - 1) / granularity) * granularity, m_Width);
        r.bottom = std::min<int32_t>(((r.bottom + granularity - 1) / granularity) * granularity, m_Height);
    }
    // Alignment can make neighbours identical or nested; fold those.
    std::vector<DekiRect> aligned;
    aligned.swap(m_Rects);
    for (const DekiRect& r : aligned)
        Add(r.left, r.top, r.right, r.bottom);
}

int64_t DirtyRegion::CoveredArea() const
{
    if (m_Full) return static_cast<int64_t>(m_Width) * m_Height;
    int64_t area = 0;
    for (const DekiRect& r : m_Rects)
        area += static_cast<int64_t>(r.Width()) * r.Height();
    return area;
}

bool DirtyRegion::Contains(int32_t x, int32_t y) const
{
    if (x < 0 || y < 0 || x >= m_Width || y >= m_Height) return false;
    if (m_Full) return true;
    for (const DekiRect& r : m_Rects)
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom)
            return true;
    return false;
}

void DirtyRegion::MergeOnePair()
{
    // Merge the pair whose bounding union adds the least new area. Quadratic
    // in the (small, threshold-bounded) rectangle count.
    const size_t n = m_Rects.size();
    if (n < 2) return;
    size_t bestA = 0, bestB = 1;
    int64_t bestGrowth = INT64_MAX;
    for (size_t i = 0; i < n; ++i)
    {
        const DekiRect& a = m_Rects[i];
        const int64_t areaA = static_cast<int64_t>(a.Width()) * a.Height();
        for (size_t j = i + 1; j < n; ++j)
        {
            const DekiRect& b = m_Rects[j];
            const int32_t l = std::min(a.left, b.left), t = std::min(a.top, b.top);
            const int32_t r = std::max(a.right, b.right), bt = std::max(a.bottom, b.bottom);
            const int64_t unionArea = static_cast<int64_t>(r - l) * (bt - t);
            const int64_t growth = unionArea - areaA - static_cast<int64_t>(b.Width()) * b.Height();
            if (growth < bestGrowth)
            {
                bestGrowth = growth;
                bestA = i;
                bestB = j;
            }
        }
    }
    DekiRect& a = m_Rects[bestA];
    const DekiRect b = m_Rects[bestB];
    a = { std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right), std::max(a.bottom, b.bottom) };
    m_Rects.erase(m_Rects.begin() + static_cast<std::ptrdiff_t>(bestB));
}

void DirtyRegion::CollapseIfCovered()
{
    if (m_Full || m_Width <= 0 || m_Height <= 0) return;
    const int64_t total = static_cast<int64_t>(m_Width) * m_Height;
    if (static_cast<double>(CoveredArea()) >= static_cast<double>(total) * m_FullRatio)
        SetFull();
}
