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
    if (m_Kept)
    {
        Deki::Memory::Free(m_Kept);
    }
}

const uint8_t* PackedPixels::RunsOf(const Picture& picture)
{
    if (!picture.first || picture.width <= 0 || picture.height <= 0 ||
        static_cast<size_t>(picture.width) * static_cast<size_t>(picture.height) > kLargestPicture)
    {
        return nullptr;
    }
    if (!m_Kept)
    {
        m_Kept = static_cast<Kept*>(Deki::Memory::Allocate(sizeof(Kept) * kPlaces, Deki::Memory::External));
        if (!m_Kept)
        {
            return nullptr;
        }
        for (size_t i = 0; i < kPlaces; ++i)
        {
            new (&m_Kept[i]) Kept();
        }
    }
    Kept* place = PlaceOf(picture);
    if (place->runs)
    {
        return place->runs;
    }
    if (m_Full || m_KeptCount == kMostPictures)
    {
        return nullptr;
    }
    const uint8_t* runs = Add(picture);
    if (runs)
    {
        *place = Kept{ picture, runs };
        ++m_KeptCount;
    }
    return runs;
}

PackedPixels::Kept* PackedPixels::PlaceOf(const Picture& picture)
{
    // From where the picture's address and revision point, to the first
    // place that is its own or free. There is always a free one: the table
    // holds more places than pictures.
    uint32_t at = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(picture.first)) * 2654435761u;
    at ^= picture.revision * 0x9E3779B1u;
    at ^= at >> 15;
    for (;; ++at)
    {
        Kept& kept = m_Kept[at % kPlaces];
        if (!kept.runs || kept.picture == picture)
        {
            return &kept;
        }
    }
}

const uint8_t* PackedPixels::Add(const Picture& picture)
{
    // In the block being filled, or in a new one when it has no room left.
    if (m_BlockCount > 0)
    {
        if (const uint8_t* runs = AddToLastBlock(picture))
        {
            return runs;
        }
    }
    if (!TakeBlock())
    {
        m_Full = true;
        return nullptr;
    }
    return AddToLastBlock(picture);
}

const uint8_t* PackedPixels::AddToLastBlock(const Picture& picture)
{
    const size_t at = (m_UsedInLast + 1) & ~static_cast<size_t>(1);  // the offsets are 16-bit
    if (at >= kBlockBytes)
    {
        return nullptr;
    }
    uint8_t* block = m_Blocks[m_BlockCount - 1];
    const size_t bytes = QuadBlit::PackRuns(picture.first, picture.width, picture.height, picture.stride, picture.flipH,
                                            picture.flipV, block + at, kBlockBytes - at);
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
    for (size_t i = 0; m_Kept && i < kPlaces; ++i)
    {
        m_Kept[i] = Kept{};
    }
    m_KeptCount = 0;
    m_Full = false;
}

}  // namespace DekiRendering