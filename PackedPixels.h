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

    /// The runs of an RGB565A8 picture: `height` rows of `width` pixels,
    /// `stride` bytes apart from `first`, made the first time they are asked
    /// for. `revision` is the picture's (QuadBlit::Source::revision): the
    /// same pixels at the same place under another revision are another
    /// picture. Null when the picture has more than kLargestPicture pixels
    /// or there is no room for it.
    const uint8_t* RunsOf(const uint8_t* first, uint32_t revision, int32_t width, int32_t height, int32_t stride);

    /// Forgets every picture, and gives back every block but the first.
    /// Pointers RunsOf returned are dead: Generation() tells a holder so.
    void Clear();

    /// Changes whenever the pictures are forgotten.
    uint32_t Generation() const { return m_Generation; }

    /// Bytes of the blocks in use, the room left at a full block's end included.
    size_t Used() const;

    static constexpr size_t kLargestPicture = 64 * 64;  // pixels: a character's frame, not a background
    static constexpr size_t kBlockBytes = 64 * 1024;
    static constexpr size_t kMostBlocks = 8;      // half a megabyte: several hundred characters' frames
    static constexpr size_t kBlocksLeftFree = 8;  // external memory left alone, in blocks
    static constexpr size_t kMostPictures = 768;  // kept at once

private:
    // A picture and where its runs are. `first` null: no picture.
    struct Picture
    {
        const uint8_t* first = nullptr;
        uint32_t revision = 0;
        int32_t width = 0, height = 0, stride = 0;
        const uint8_t* runs = nullptr;
    };
    // A table with room to spare, so a picture is found in a step or two.
    static constexpr size_t kPlaces = 1024;
    // The picture's place in m_Pictures: its own, or the free one it would
    // take.
    Picture* PlaceOf(const uint8_t* first, uint32_t revision, int32_t width, int32_t height, int32_t stride);
    // New runs for a picture, in the last block or a new one; null when
    // there is no room.
    const uint8_t* Add(const uint8_t* first, int32_t width, int32_t height, int32_t stride);
    // The runs, in the last block; null when it has no room for them.
    const uint8_t* AddToLastBlock(const uint8_t* first, int32_t width, int32_t height, int32_t stride);
    // One more block; false at kMostBlocks or when the target cannot spare it.
    bool TakeBlock();

    Picture* m_Pictures = nullptr;  // kPlaces of them, in external memory
    size_t m_PictureCount = 0;
    uint8_t* m_Blocks[kMostBlocks] = {};
    size_t m_BlockCount = 0;
    size_t m_UsedInLast = 0;
    uint32_t m_Generation = 1;
};

}  // namespace DekiRendering
