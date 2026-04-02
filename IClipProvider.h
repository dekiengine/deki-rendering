#pragma once

#include <cstdint>

/**
 * @file IClipProvider.h
 * @brief Interface for components that provide clip rectangles
 *
 * Implement this interface on any component to have Standard2DRenderer
 * automatically push/pop clip rects during rendering. No pass registration needed.
 *
 * Usage in your component:
 * @code
 * void* QueryInterface(uint32_t id) override {
 *     if (id == IClipProvider::InterfaceID) return static_cast<IClipProvider*>(this);
 *     return DekiComponent::QueryInterface(id);
 * }
 * @endcode
 */

class IClipProvider
{
public:
    static constexpr uint32_t InterfaceID = 0x434C4950; // "CLIP"

    virtual ~IClipProvider() = default;
    virtual float GetClipWidth() const = 0;
    virtual float GetClipHeight() const = 0;
};
