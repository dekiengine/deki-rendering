#pragma once

#include <cstddef>
#include <cstdint>

namespace DekiRendering
{

/// Small pictures kept as runs (QuadBlit::PackRuns), to draw them from.
///
/// An animation frame is a small box in a wide sheet, and most of the box is
/// transparent. Drawn from the sheet, every pixel of the box is read, three
/// bytes each, to find the ones to draw; where the sheet is in memory that
/// is slow to reach (a board's external RAM) that reading is most of what a
/// small sprite costs. Its runs hold only the pixels drawn.
///
/// The runs live in blocks of external memory, each taken when the one before
/// is full, up to kMostBlocks and never the last of the target's external
/// memory. A picture there is no room for is simply not added, and is drawn
/// from the sheet as before. Clear() empties the store: pictures no longer
/// drawn (another scene's) would otherwise keep their room for good.
///
/// Whether to add at all is the caller's to say: on a target with no
/// external memory pictures are quick to read, and the blocks would come out
/// of its internal RAM.
class PackedPixels
{
public:
    PackedPixels() = default;
    ~PackedPixels();
    PackedPixels(const PackedPixels&) = delete;
    PackedPixels& operator=(const PackedPixels&) = delete;

    /// An RGB565A8 picture: `height` rows of `width` pixels, `stride` bytes
    /// apart from `first`, drawn mirrored across (`flipH`) or down (`flipV`)
    /// or as it is. `revision` is the picture's (QuadBlit::Source::revision):
    /// the same pixels at the same place under another revision are another
    /// picture, as is its mirror.
    struct Picture
    {
        const uint8_t* first = nullptr;
        uint32_t revision = 0;
        int32_t width = 0, height = 0, stride = 0;
        bool flipH = false, flipV = false;

        bool operator==(const Picture& o) const
        {
            return first == o.first && revision == o.revision && width == o.width && height == o.height &&
                   stride == o.stride && flipH == o.flipH && flipV == o.flipV;
        }
    };

    /// The picture's runs, made the first time they are asked for. Null when
    /// it has more than kLargestPicture pixels or there is no room for it.
    const uint8_t* RunsOf(const Picture& picture);

    /// Forgets every picture, and gives back every block but the first.
    /// Pointers RunsOf returned are dead.
    void Clear();

    /// Bytes of the blocks in use, the room left at a full block's end included.
    size_t Used() const;

    static constexpr size_t kLargestPicture = 128 * 128;  // pixels: a character or a tree, not a background
    static constexpr size_t kBlockBytes = 64 * 1024;
    static constexpr size_t kMostBlocks = 8;      // half a megabyte: several hundred characters' frames
    static constexpr size_t kBlocksLeftFree = 8;  // external memory left alone, in blocks
    static constexpr size_t kMostPictures = 768;  // kept at once

private:
    // A picture kept and where its runs are. No picture: `runs` null.
    struct Kept
    {
        Picture picture;
        const uint8_t* runs = nullptr;
    };
    // A table with room to spare, so a picture is found in a step or two.
    static constexpr size_t kPlaces = 1024;
    // The picture's place in m_Kept: its own, or the free one it would take.
    Kept* PlaceOf(const Picture& picture);
    // New runs for a picture, in the last block or a new one; null when
    // there is no room.
    const uint8_t* Add(const Picture& picture);
    // The runs, in the last block; null when it has no room for them.
    const uint8_t*
    AddToLastBlock(const Picture& picture);  // One more block; false at kMostBlocks or when the target cannot spare it.
    bool TakeBlock();

    Kept* m_Kept = nullptr;  // kPlaces of them, in external memory
    size_t m_KeptCount = 0;
    // No block could be taken for a picture: none is tried again until
    // Clear(), so a full store costs a draw nothing.
    bool m_Full = false;
    uint8_t* m_Blocks[kMostBlocks] = {};
    size_t m_BlockCount = 0;
    size_t m_UsedInLast = 0;
};

}  // namespace DekiRendering
