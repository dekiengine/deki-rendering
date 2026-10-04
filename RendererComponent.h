#pragma once
#include <cstdint>

#include <deki/Component.h>
#include <deki/ISortableProvider.h>
#include <deki/reflection/Property.h>
#include "QuadBlit.h"

namespace Deki
{
class Object;
}

namespace DekiRendering
{
class CameraComponent;

#ifdef V_ENGINE_ENABLE_MASK
// Mask render modes, for stencil buffer use.
enum class MaskRenderMode : uint8_t
{
    None = 0,           // No masking
    RenderOutside = 1,  // Render only outside the mask
    RenderInside = 2    // Render only inside the mask
};
#endif

/// How partial-alpha pixels are rendered.
///
/// Blend         standard alpha blend (the default; smooth, slower).
/// OrderedDither ordered dither, or screen door (faster, visible stipple):
///               QuadBlit's Bayer paths, chosen per object by
///               Standard2DRenderer.
enum class AlphaMode : uint8_t
{
    Blend = 0,
    OrderedDither = 1,
};

DEKI_CATEGORY("Core")
/// The view being drawn: its pixels per world meter and its size.
///
/// Set by the renderer before it asks components for their content, so a
/// component that bakes pixels (a gradient) can bake them at the density they
/// will be drawn at and land 1:1 on the screen, instead of being scaled by a
/// fractional factor. pixelsPerMeter is 0 outside a frame.
struct DrawView
{
    float pixelsPerMeter = 0.0f;
    int32_t width = 0;
    int32_t height = 0;
};
const DrawView& CurrentDrawView();
void SetCurrentDrawView(const DrawView& view);

/// Base class for renderable components (sprites, particles). A subclass
/// produces its pixels at 1x in RenderContent; the renderer applies position,
/// scale and rotation with QuadBlit.
class RendererComponent : public Deki::Component, public Deki::ISortableProvider
{
public:
    // Pure virtual, to make the class abstract.
    virtual ~RendererComponent() = 0;

    DEKI_EXPORT
    DEKI_TOOLTIP("Draw order against other renderers. Higher draws on top.")
    int sortingOrder = 0;

    // If true, this renderer ignores the bounds of ancestor Deki2D::ClipComponents.
    DEKI_EXPORT
    DEKI_TOOLTIP(
        "Draw even when an ancestor clips its children. For something that must escape its container, like a dropdown.")
    bool ignoreClip = false;

    // If true, the final screen position is rounded to the nearest whole
    // pixel at draw time, which keeps pixel art aligned. Turn it off for
    // renderers that should move smoothly by sub-pixels (particles, camera
    // shake). It is per renderer, so pixel-art sprites and smooth particles
    // can share a scene.
    DEKI_EXPORT
    DEKI_TOOLTIP("Round the final position to whole pixels. Keeps pixel art crisp; leave it off for something that "
                 "should move smoothly at small steps.")
    bool pixelSnap = true;

    DEKI_TOOLTIP(
        "How partial-alpha pixels are rendered. Blend = smooth alpha blend (slower, no artifacts). OrderedDither = "
        "stippling pattern (much faster, visible dither — best for fades and retro pixel art). ")
    DEKI_EXPORT
    AlphaMode alphaMode = AlphaMode::Blend;

#ifdef V_ENGINE_ENABLE_MASK
    // Mask support, 2 bytes in all.
    MaskRenderMode maskMode = MaskRenderMode::None;
    uint8_t stencilId = 0;  // 0 = no stencil test, 1-255 = stencil value
#endif

    void SetSortingOrder(int order);
    int32_t GetSortingOrder() const override { return sortingOrder; }

#ifdef V_ENGINE_ENABLE_MASK
    void SetMaskMode(MaskRenderMode mode, uint8_t stencilId = 1);
    void ClearMask();
#endif

    /// A conservative world-space size of what RenderContent draws, in meters,
    /// before the object's scale. The renderer skips RenderContent and the
    /// blit for objects entirely outside the target and the current clip,
    /// allowing for pivot and rotation itself. Return false when the size is
    /// unknown; the object is then always drawn.
    virtual bool GetContentExtents(float& outWidth, float& outHeight) const
    {
        (void)outWidth;
        (void)outHeight;
        return false;
    }

    /// Produces the component's pixels at 1x scale, without transforms; the
    /// renderer applies position, scale and rotation with QuadBlit. Fills
    /// `outSource`, the pivot (0-1, 0.5 is the centre) and the tint (255 means
    /// no tint; outTintA 255 is opaque). Returns false when there is nothing
    /// to draw.
    ///
    /// outSource.ownsPixels says whether the renderer takes over
    /// outSource.pixels and frees it through Deki::Memory after the blit.
    /// Components that keep their buffers (sprites, baked text and gradients,
    /// anything reusing one composite across frames) leave it false, the
    /// default. Set it only for a buffer allocated this frame through
    /// Deki::Memory, normally Deki::Buffer<T>::Release().
    virtual bool RenderContent(const Deki::Object* owner, QuadBlit::Source& outSource, float& outPivotX,
                               float& outPivotY, uint8_t& outTintR, uint8_t& outTintG, uint8_t& outTintB,
                               uint8_t& outTintA)
    {
        outTintR = outTintG = outTintB = outTintA = 255;
        return false;
    }
};

}  // namespace DekiRendering
