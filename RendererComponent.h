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

    /// Redrawing only what changed (RenderingProjectSettings::redrawChanges):
    /// a number that changes whenever the pixels RenderContent hands over
    /// change in place, behind the same buffer, size, pivot and tint (a
    /// label's buffer filled with new text, a gradient baked again at its old
    /// size). The renderer compares all of those itself. Return false when
    /// the component cannot tell (the default): it then counts as changed in
    /// every frame it is drawn, which is always right, only slower.
    virtual bool GetContentVersion(uint32_t& outVersion) const
    {
        (void)outVersion;
        return false;
    }

    /// Redrawing only what changed: a value that changes whenever anything
    /// GetContentExtents, RenderContent or GetContentVersion would give could
    /// change, read from the component's own fields (not by producing the
    /// content). While it stays the same, and the object's scale and rotation
    /// do, the renderer reuses what it measured of the content the last
    /// frame instead of asking for it again: only where the object is, is
    /// worked out anew. Assets are taken as they are while loaded; a reload
    /// is seen without the key. Return false when the component cannot tell
    /// (the default): it is then measured in full every frame, which is
    /// always right, only slower.
    virtual bool GetDrawKey(uint64_t& outKey) const
    {
        (void)outKey;
        return false;
    }

    /// A renderer that draws many pieces, such as a batch of sprites, each
    /// placed and sorted on its own as if it were a child object of its own:
    /// how many. Pieces cost far less than objects each, since they are read
    /// from the component instead of each from an object and a component of
    /// its own. 0 (the default) is one drawing, at the object's place and in
    /// its sortingOrder, through the calls above; with pieces, the calls
    /// below are used instead.
    virtual uint32_t GetPieceCount() const { return 0; }

    /// Where piece `index` is from the object, in meters, as a child object
    /// there would be (the object's scale and turn apply to it), and its
    /// sorting order.
    virtual void GetPiecePlace(uint32_t index, float& outX, float& outY, int32_t& outSortingOrder) const
    {
        (void)index;
        outX = outY = 0.0f;
        outSortingOrder = sortingOrder;
    }

    /// GetContentExtents, RenderContent, GetContentVersion and GetDrawKey, for
    /// piece `index`.
    virtual bool GetPieceExtents(uint32_t index, float& outWidth, float& outHeight) const
    {
        (void)index;
        (void)outWidth;
        (void)outHeight;
        return false;
    }
    virtual bool RenderPieceContent(uint32_t index, const Deki::Object* owner, QuadBlit::Source& outSource,
                                    float& outPivotX, float& outPivotY, uint8_t& outTintR, uint8_t& outTintG,
                                    uint8_t& outTintB, uint8_t& outTintA)
    {
        (void)index;
        (void)owner;
        (void)outSource;
        (void)outPivotX;
        (void)outPivotY;
        outTintR = outTintG = outTintB = outTintA = 255;
        return false;
    }
    virtual bool GetPieceVersion(uint32_t index, uint32_t& outVersion) const
    {
        (void)index;
        (void)outVersion;
        return false;
    }
    virtual bool GetPieceDrawKey(uint32_t index, uint64_t& outKey) const
    {
        (void)index;
        (void)outKey;
        return false;
    }

    /// Whether this renderer lights what it draws itself (a lit 3D mesh).
    /// The 2D light, which lights the frame's pixels, is then laid over the
    /// frame before it draws, so its pixels are not lit twice. The default is
    /// no.
    virtual bool LightsItself() const { return false; }
};

}  // namespace DekiRendering
