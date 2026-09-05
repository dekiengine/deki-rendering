#include "RendererComponent.h"
#include "DekiEngine.h"
#include "ComponentInterfaceAdapters.h"

// ============================================================================
// Component Registration
// ============================================================================
// NOTE: s_Properties[] and s_ComponentMeta are now auto-generated in
// RendererComponent.gen.h (included at end of RendererComponent.h)

// Register ISortableProvider adapter for sorting order queries
static struct RendererSortableRegistrar {
    RendererSortableRegistrar() {
        Deki::ComponentInterfaceAdapters::Register(
            Deki::ISortableProvider::InterfaceID, RendererComponent::StaticType,
            [](Deki::Component* c) -> void* {
                return static_cast<Deki::ISortableProvider*>(static_cast<RendererComponent*>(c));
            });
    }
} s_rendererSortableReg;


// ============================================================================

// Pure virtual destructor still needs a definition
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
