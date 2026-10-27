#pragma once

#include <cstdint>
#include <cstring>

namespace DekiRendering
{

/// A 64-bit fingerprint of a few values, to tell whether something changed
/// since the last frame: a component's bake settings, an object's draw.
///
/// Two 32-bit FNV-1a lanes over 32-bit words, with other starting values and
/// multipliers, read as one 64-bit value. A word at a time, and 32-bit
/// multiplies only: a 32-bit microcontroller (Xtensa, RISC-V, Cortex-M) has
/// no 64-bit multiply, so each one is several instructions or a library call,
/// and hashing a few dozen bytes one at a time that way costs tens of
/// microseconds a frame. Not for anything adversarial.
class Fingerprint
{
public:
    void Add(uint32_t w)
    {
        m_A = (m_A ^ w) * 16777619u;
        m_B = (m_B ^ w) * 0x2545F491u + 0x6D2B79F5u;
    }
    void Add(int32_t v) { Add(static_cast<uint32_t>(v)); }
    void Add(float v)
    {
        uint32_t w;
        std::memcpy(&w, &v, sizeof(w));
        Add(w);
    }
    void Add(const void* p)
    {
        const uint64_t v = reinterpret_cast<uintptr_t>(p);
        Add(static_cast<uint32_t>(v));
        Add(static_cast<uint32_t>(v >> 32));
    }

    uint64_t Value() const { return (static_cast<uint64_t>(m_A) << 32) | m_B; }

private:
    uint32_t m_A = 2166136261u;
    uint32_t m_B = 0x9E3779B9u;
};

}  // namespace DekiRendering
