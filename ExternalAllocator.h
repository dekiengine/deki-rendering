#pragma once

#include <cstddef>
#include <cstdlib>

#include <deki/providers/Memory.h>

namespace DekiRendering
{

/// Puts a standard container's elements in the engine's External memory
/// (Deki::Memory::External), which a target without any serves from its
/// internal RAM.
///
/// A plain std::vector takes its memory from the C library, which on a board
/// decides by size alone where it comes from: a list of a hundred entries may
/// land in the few kilobytes of internal RAM a game has left, and a list that
/// grows leaves each size it passed through there for a moment. A list read
/// once a frame costs little in slower memory and nothing of the internal RAM.
template <typename T>
struct ExternalAllocator
{
    using value_type = T;

    ExternalAllocator() = default;
    template <typename U>
    ExternalAllocator(const ExternalAllocator<U>&)
    {
    }

    T* allocate(size_t count)
    {
        void* memory = Deki::Memory::Allocate(count * sizeof(T), Deki::Memory::External);
        if (!memory)
        {
            std::abort();  // as operator new does where there are no exceptions
        }
        return static_cast<T*>(memory);
    }
    void deallocate(T* memory, size_t) { Deki::Memory::Free(memory); }

    template <typename U>
    bool operator==(const ExternalAllocator<U>&) const
    {
        return true;
    }
    template <typename U>
    bool operator!=(const ExternalAllocator<U>&) const
    {
        return false;
    }
};

}  // namespace DekiRendering
