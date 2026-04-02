#include "RendererComponent.h"
#include "DekiEngine.h"

// ============================================================================
// Component Registration
// ============================================================================
// NOTE: s_Properties[] and s_ComponentMeta are now auto-generated in
// RendererComponent.gen.h (included at end of RendererComponent.h)


// ============================================================================

// Pure virtual destructor still needs a definition
RendererComponent::~RendererComponent() = default;

void RendererComponent::SetSortingOrder(int order)
{
    sortingOrder = order;
}

int RendererComponent::GetSortingOrder() const
{
    return sortingOrder;
}

#ifdef V_ENGINE_ENABLE_MASK
void RendererComponent::SetMaskMode(MaskRenderMode mode, uint8_t stencilId)
{
    mask_mode = mode;
    stencil_id = stencilId;
}

void RendererComponent::ClearMask()
{
    mask_mode = MaskRenderMode::None;
    stencil_id = 0;
}
#endif
