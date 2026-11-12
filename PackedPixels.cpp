#include "PackedPixels.h"

#include <new>

#include <deki/providers/Memory.h>

#include "QuadBlit.h"

namespace DekiRendering
{

PackedPixels::~PackedPixels()
{
    for (size_t i = 0; i < m_BlockCount; ++i)
    {
        Deki::Memory::Free(m_Blocks[i]);
    }
    if (m_Pictures)
    {
        Deki::Memory::Free(m_Pictures);
    }
}

const uint8_t* PackedPixels::RunsOf(const uint8_t* first, uint32_t revision, int32_t width, int32_t height,
                                    int32_t stride)
{
    if (!first || width <= 0 || height <= 0 ||
        static_cast<size_t>(width) * static_cast<size_t>(height) > kLargestPicture)
    {
        return nullptr;
    }
    if (!m_Pictures)
    {
        m_Pictures = static_cast<Picture*>(Deki::Memory::Allocate(sizeof(Picture) * kPlaces, Deki::Memory::External));
        if (!m_Pictures)
        {
            return nullptr;
        }
        for (size_t i = 0; i < kPlaces; ++i)
        {
            new (&m_Pictures[i]) Picture();
        }
    }
    Picture* place = PlaceOf(first, revision, width, height, stride);
    if (place->first)
    {
        return place->runs;
    }
    if (m_PictureCount == kMostPictures)
    {
        return nullptr;
    }
    const uint8_t* runs = Add(first, width, height, stride);
    if (runs)
    {
        *place = Picture{ first, revision, width, height, stride, runs };
        ++m_PictureCount;
    }
    return runs;
}

PackedPixels::Picture* PackedPixels::PlaceOf(const uint8_t* first, uint32_t revision, int32_t width, int32_t height,
                                             int32_t stride)
{
    // From where the picture's address and revision point, to the first
    // place that is its own or free. There is always a free one: the table
    // holds more places than pictures.
    uint32_t at = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(first)) * 2654435761u;
    at ^= revision * 0x9E3779B1u;
    at ^= at >> 15;
    for (;; ++at)
    {
        Picture& p = m_Pictures[at % kPlaces];
        if (!p.first || (p.first == first && p.revision == revision && p.width == width && p.height == height &&
                         p.stride == stride))
        {
            return &p;
        }
    }
}

const uint8_t* PackedPixels::Add(const uint8_t* first, int32_t width, int32_t height, int32_t stride)
{
    // In the block being filled, or in a new one when it has no room left.
    if (m_BlockCount > 0)
    {
        if (const uint8_t* runs = AddToLastBlock(first, width, height, stride))
        {
            return runs;
        }
    }
    return TakeBlock() ? AddToLastBlock(first, width, height, stride) : nullptr;
}

const uint8_t* PackedPixels::AddToLastBlock(const uint8_t* first, int32_t width, int32_t height, int32_t stride)
{
    const size_t at = (m_UsedInLast + 1) & ~static_cast<size_t>(1);  // the offsets are 16-bit
    if (at >= kBlockBytes)
    {
        return nullptr;
    }
    uint8_t* block = m_Blocks[m_BlockCount - 1];
    const size_t bytes = QuadBlit::PackRuns(first, width, height, stride, block + at, kBlockBytes - at);
    if (bytes == 0)
    {
        return nullptr;
    }
    m_UsedInLast = at + bytes;
    return block + at;
}

bool PackedPixels::TakeBlock()
{
    if (m_BlockCount == kMostBlocks)
    {
        return false;
    }
    // Not the last of the target's external memory: what a game loads next
    // needs it more. (0 is a host that does not measure.)
    const size_t free = Deki::Memory::GetAvailableExternal();
    if (free != 0 && free < kBlockBytes * kBlocksLeftFree)
    {
        return false;
    }
    uint8_t* block = static_cast<uint8_t*>(Deki::Memory::Allocate(kBlockBytes, Deki::Memory::External));
    if (!block)
    {
        return false;
    }
    m_Blocks[m_BlockCount++] = block;
    m_UsedInLast = 0;
    return true;
}

size_t PackedPixels::Used() const
{
    return m_BlockCount == 0 ? 0 : (m_BlockCount - 1) * kBlockBytes + m_UsedInLast;
}

void PackedPixels::Clear()
{
    // The first block stays, to fill again; the others go back.
    while (m_BlockCount > 1)
    {
        Deki::Memory::Free(m_Blocks[--m_BlockCount]);
    }
    m_UsedInLast = 0;
    for (size_t i = 0; m_Pictures && i < kPlaces; ++i)
    {
        m_Pictures[i] = Picture{};
    }
    m_PictureCount = 0;
    if (++m_Generation == 0)
    {
        m_Generation = 1;
    }
}

}  // namespace DekiRendering
