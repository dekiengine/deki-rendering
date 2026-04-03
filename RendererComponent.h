#pragma once
#include <cstdint>

#include "DekiBehaviour.h"
#include "ISortableProvider.h"
#include "reflection/DekiProperty.h"
#include "QuadBlit.h"

// Forward declarations
class DekiObject;
class CameraComponent;

#ifdef V_ENGINE_ENABLE_MASK
/**
 * @brief Mask render modes for stencil buffer usage
 */
enum class MaskRenderMode : uint8_t
{
    None = 0,  // No masking
    RenderOutside = 1,  // Render only outside the mask
    RenderInside = 2  // Render only inside the mask
};
#endif

/**
 * @brief Abstract base class for all renderable components (e.g., sprites, particles)
 *
 * Extends DekiBehaviour to provide lifecycle methods (Start, Update, PreRender)
 * in addition to the Render method for drawing.
 */
class RendererComponent : public DekiBehaviour, public ISortableProvider
{
   public:
    DEKI_COMPONENT(RendererComponent, DekiBehaviour, "Core", "9604fa26-8be9-428a-9c29-e67c2d52c913", "")

    // Pure virtual destructor makes this class abstract
    virtual ~RendererComponent() = 0;

    DEKI_EXPORT
    int sortingOrder = 0;

    /** @brief If true, this renderer ignores parent ClipComponent bounds */
    DEKI_EXPORT
    bool ignore_clip = false;

#ifdef V_ENGINE_ENABLE_MASK
    // Mask support - minimal memory overhead (2 bytes total)
    MaskRenderMode mask_mode = MaskRenderMode::None;
    uint8_t stencil_id = 0;  // 0 = no stencil test, 1-255 = stencil values
#endif

    void SetSortingOrder(int order);
    int32_t GetSortingOrder() const override;

#ifdef V_ENGINE_ENABLE_MASK
    // Mask configuration methods
    void SetMaskMode(MaskRenderMode mode, uint8_t stencilId = 1);
    void ClearMask();
#endif

    /**
     * @brief Render content to intermediate buffer at 1x scale (no transforms)
     *
     * Components that override this method produce raw pixel data.
     * The render loop will use QuadBlit to apply transforms (position, scale, rotation).
     *
     * @param owner The owning DekiObject
     * @param outSource Output QuadBlit::Source descriptor (buffer, dimensions, format)
     * @param outPivotX Output pivot X (0.0-1.0, where 0.5 is center)
     * @param outPivotY Output pivot Y (0.0-1.0, where 0.5 is center)
     * @param outTintR Output tint red (255 = no tint)
     * @param outTintG Output tint green (255 = no tint)
     * @param outTintB Output tint blue (255 = no tint)
     * @param outTintA Output tint alpha (255 = opaque)
     * @return true if content was rendered, false if nothing to render
     *
     * NOTE: The caller is responsible for freeing outSource.pixels if this returns true
     */
    virtual bool RenderContent(const DekiObject* owner,
                               QuadBlit::Source& outSource,
                               float& outPivotX,
                               float& outPivotY,
                               uint8_t& outTintR,
                               uint8_t& outTintG,
                               uint8_t& outTintB,
                               uint8_t& outTintA)
    {
        outTintR = outTintG = outTintB = outTintA = 255;
        return false;
    }

};

// Generated property metadata (after class definition for offsetof)
#include "generated/RendererComponent.gen.h"
