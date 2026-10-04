#include "RendererComponent.h"
#include <deki/Engine.h>
#include <deki/ComponentInterfaceAdapters.h>

namespace DekiRendering
{

// Registers the ISortableProvider adapter, so sorting can read sortingOrder.
static struct RendererSortableRegistrar
{
    RendererSortableRegistrar()
    {
        Deki::ComponentInterfaceAdapters::Register(
            Deki::ISortableProvider::kInterfaceID, ::Deki::TypeId<RendererComponent>(), [](Deki::Component* c) -> void*
            { return static_cast<Deki::ISortableProvider*>(static_cast<RendererComponent*>(c)); });
    }
} s_RendererSortableReg;

// A pure virtual destructor still needs a definition.
RendererComponent::~RendererComponent() = default;

void RendererComponent::SetSortingOrder(int order)
{
    sortingOrder = order;
}

#ifdef V_ENGINE_ENABLE_MASK
void RendererComponent::SetMaskMode(MaskRenderMode mode, uint8_t stencilId)
{
    maskMode = mode;
    stencilId = stencilId;
}

void RendererComponent::ClearMask()
{
    maskMode = MaskRenderMode::None;
    stencilId = 0;
}
#endif

}  // namespace DekiRendering
