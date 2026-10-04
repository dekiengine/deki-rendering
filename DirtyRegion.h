#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <deki/providers/Rect.h>

namespace DekiRendering
{

/// The pixels a frame touched, as a small set of rectangles.
///
/// QuadBlit adds every clipped blit rectangle to the region the renderer gives
/// it (see QuadBlit::SetDirtyTracking); DekiRenderSystem combines regions
/// across frames into the rectangles the display must push and the rectangles
/// the next frame must clear.
///
/// Nothing here is a limit. The two numbers are policies:
/// - merge threshold: above this many rectangles, the pair whose union grows
///   least is merged, so the set stays small even with hundreds of sprites;
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
    /// rectangles are ignored.
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
    void SetFullCoverageRatio(float ratio) { m_FullRatio = ratio; }
    float FullCoverageRatio() const { return m_FullRatio; }

    /// True when (x, y) lies inside the region; a full region contains every
    /// pixel of the target. For tests and diagnostics.
    bool Contains(int32_t x, int32_t y) const;

private:
    void MergeOnePair();
    void CollapseIfCovered();

    int32_t m_Width = 0;
    int32_t m_Height = 0;
    bool m_Full = false;
    std::vector<Deki::Rect> m_Rects;
    size_t m_MergeThreshold = 24;
    float m_FullRatio = 0.8f;
};

}  // namespace DekiRendering
