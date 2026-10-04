#pragma once

#include <cstdint>

namespace DekiRendering
{

/// For components that send their object's rendering to another target.
///
/// The component gives a tag that a RenderPass can use to redirect the
/// object's blits to a scratch buffer (a darkness world buffer, a light-mask
/// buffer). The engine does not interpret the tag; each pass defines its own.
/// An empty or null tag means the default target, the current ctx.buffer.
///
///   class MyTagComponent : public Deki::Component, public IRenderTargetProvider {
///       std::string tag;
///       const char* GetRenderTargetTag() const override { return tag.c_str(); }
///   };
///
///   // In MyTagComponent.cpp:
///   Deki::ComponentInterfaceAdapters::Register(
///       IRenderTargetProvider::kInterfaceID, ::Deki::TypeId<MyTagComponent>(),
///       [](Deki::Component* c) -> void* {
///           return static_cast<IRenderTargetProvider*>(static_cast<MyTagComponent*>(c));
///       });

class IRenderTargetProvider
{
public:
    static constexpr uint32_t kInterfaceID = 0x52544754;  // "RTGT"

    virtual ~IRenderTargetProvider() = default;
    virtual const char* GetRenderTargetTag() const = 0;
};

}  // namespace DekiRendering
