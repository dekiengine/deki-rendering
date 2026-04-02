#pragma once

#include <cstdint>

/**
 * @file ISortableProvider.h
 * @brief Interface for components that provide sorting order
 *
 * Implement this interface on any component to have Standard2DRenderer
 * automatically include the object in sorting. No callback registration needed.
 *
 * Usage in your component:
 * @code
 * void* QueryInterface(uint32_t id) override {
 *     if (id == ISortableProvider::InterfaceID) return static_cast<ISortableProvider*>(this);
 *     return DekiComponent::QueryInterface(id);
 * }
 * @endcode
 */

class ISortableProvider
{
public:
    static constexpr uint32_t InterfaceID = 0x534F5254; // "SORT"

    virtual ~ISortableProvider() = default;
    virtual int32_t GetSortingOrder() const = 0;
};
