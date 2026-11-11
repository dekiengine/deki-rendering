#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <deki/providers/Rect.h>

namespace DekiRendering
{

/// The pixels a frame changed, as a small set of rectangles: what
/// Standard2DRenderer::FindChanges finds, and what DekiRenderSystem redraws
/// and sends of it.
///
/// Nothing here is a limit. The two numbers are policies:
/// - merge threshold: above this many rectangles, the pairs whose union grows
///   least are merged until a quarter fewer than that are left, so the set
///   stays small even with hundreds of sprites;
/// - full-coverage ratio: once the rectangles cover this fraction of the
///   target, the region becomes "everything", since a full push is then
///   cheaper than the bookkeeping.
/// Rectangles may overlap; treat the set as a cover, not a partition.
class DirtyRegion
{
public:
    DirtyRegion() = default;

    /// Empties the region and sets the target size. Rectangles added later
    /// are clipped to it.
    void Reset(int32_t width, int32_t height);

    void SetFull();
    bool IsFull() const { return m_Full; }
    bool IsEmpty() const { return !m_Full && m_Rects.empty(); }

    int32_t Width() const { return m_Width; }
    int32_t Height() const { return m_Height; }

    /// Adds [left, right) x [top, bottom), clipped to the target. Empty
    /// rectangles are ignored. One inside a rectangle already there adds
    /// nothing; one around it, or whose bounding box with it is no larger
    /// than the two together, takes its place.
    void Add(int32_t left, int32_t top, int32_t right, int32_t bottom);
    void Add(const Deki::Rect& r) { Add(r.left, r.top, r.right, r.bottom); }

    /// Adds every rectangle of `other`, whose target size must match. A full
    /// `other` makes this full.
    void Union(const DirtyRegion& other);

    /// Rounds every rectangle outwards to multiples of `granularity` pixels,
    /// clamped to the target. Displays push aligned rectangles faster, and a
    /// small movement then reuses the same rectangle frame to frame.
    void Align(int32_t granularity);

    /// Sum of rectangle areas. Overlaps count twice, which errs towards
    /// "full" in the full-coverage decision.
    int64_t CoveredArea() const;

    /// The rectangles. Empty when the region is empty or full: check IsFull().
    const std::vector<Deki::Rect>& Rects() const { return m_Rects; }

    // Policies (see the class comment). Defaults: 24 rectangles, 0.8.
    void SetMergeThreshold(size_t maxRects) { m_MergeThreshold = maxRects < 1 ? 1 : maxRects; }
    size_t MergeThreshold() const { return m_MergeThreshold; }
    void SetFullCoverageRatio(float ratio);
    float FullCoverageRatio() const { return m_FullRatio; }

    /// Merges rectangles while the cheapest merge adds no more than `pixels`
    /// of new area: where drawing a rectangle on its own costs about as much
    /// as that many pixels, fewer rectangles are cheaper.
    void MergeCheaperThan(int64_t pixels);

    /// True when (x, y) lies inside the region; a full region contains every
    /// pixel of the target. For tests and diagnostics.
    bool Contains(int32_t x, int32_t y) const;

private:
    // Merges the pair whose union adds the least area, again and again, while
    // that is no more than maxGrowth and there are more than bove
    // rectangles.
    void MergeWhile(int64_t maxGrowth, size_t above);
    // Finds m_Partner[i] and m_PartnerGrowth[i] among the rectangles after i.
    void FindPartner(size_t i);
    void CollapseIfCovered();
    void UpdateFullAt();
    static int64_t AreaOf(const Deki::Rect& r) { return static_cast<int64_t>(r.Width()) * r.Height(); }

    int32_t m_Width = 0;
    int32_t m_Height = 0;
    bool m_Full = false;
    std::vector<Deki::Rect> m_Rects;
    std::vector<Deki::Rect> m_Scratch;  // Align's, kept so it does not allocate
    size_t m_MergeThreshold = 24;
    float m_FullRatio = 0.8f;
    int64_t m_Area = 0;  // the sum of the rectangles' areas
    // The area from which the region is full, for the size it was worked out for.
    int64_t m_FullAt = 0;
    int32_t m_FullAtWidth = -1, m_FullAtHeight = -1;
    // MergeWhile's: for each rectangle, the one after it that it grows least
    // with, and by how much. Kept so merging allocates nothing.
    std::vector<uint16_t> m_Partner;
    std::vector<int64_t> m_PartnerGrowth;
};

}  // namespace DekiRendering
