#pragma once

#include <cstdint>

/**
 * @file IRenderTargetProvider.h
 * @brief Interface for components that route rendering to a non-default target
 *
 * A component implementing this interface advertises a tag string that
 * a RenderPass can use to redirect that object's blits to a scratch
 * buffer (e.g. a darkness world buffer, a light-mask buffer). The tag
 * is opaque to the engine: each pass defines its own conventions.
 *
 * Empty / null tag means "default" (use the current ctx.buffer).
 *
 * Usage in your component:
 * @code
 * class MyTagComponent : public DekiComponent, public IRenderTargetProvider {
 *     std::string tag;
 *     const char* GetRenderTargetTag() const override { return tag.c_str(); }
 * };
 *
 * // In MyTagComponent.cpp:
 * ComponentInterfaceAdapters::Register(
 *     IRenderTargetProvider::InterfaceID, MyTagComponent::StaticType,
 *     [](DekiComponent* c) -> void* {
 *         return static_cast<IRenderTargetProvider*>(static_cast<MyTagComponent*>(c));
 *     });
 * @endcode
 */

class IRenderTargetProvider
{
public:
    static constexpr uint32_t InterfaceID = 0x52544754; // "RTGT"

    virtual ~IRenderTargetProvider() = default;
    virtual const char* GetRenderTargetTag() const = 0;
};
