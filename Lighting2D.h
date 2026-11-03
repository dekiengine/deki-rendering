#pragma once

#include <cstdint>
#include <vector>

#include <deki/Engine.h>
#include <deki/providers/Rect.h>

#include "DekiRenderingPackage.h"
#include "FrameCamera.h"

namespace Deki
{
class Scene;
}

namespace DekiRendering
{
class DirtyRegion;

/// One light as the 2D world sees it this frame, in pixels of the whole view.
struct Light2D
{
    float x = 0.0f;
    float y = 0.0f;
    float radius = 0.0f;
    // Colour times intensity, 1 per channel lighting a colour as it is.
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    // A spot's facing on screen (unit), and the cosines of its cone's edge
    // and of where it starts to fade. Not a spot: spot false.
    bool spot = false;
    float dirX = 1.0f;
    float dirY = 0.0f;
    float cosOuter = 0.0f;
    float cosInner = 0.0f;
    int32_t bands = 0;  // 0 smooth, else that many hard rings

    bool operator==(const Light2D& o) const;
    bool operator!=(const Light2D& o) const { return !(*this == o); }

    /// The pixels it can reach, rounded out to whole blocks.
    Deki::Rect Bounds() const;
};

/// The 2D light of one frame: what the scene's Lights (Light.h) add up to over
/// the view, and the pass that lights the frame's pixels with it.
///
/// The light is worked out once per block of kBlock x kBlock pixels, at the
/// block's centre, on a grid fixed to the whole view, so a strip or a redraw
/// of part of the frame lights each pixel exactly as a whole frame does. Each
/// pixel's colour is then multiplied by that light, per channel, in steps of
/// 1/32 up to twice the colour: the block's own light where it and the blocks
/// round it are lit alike (or in hard rings), and elsewhere the nearest block
/// centres' blended for the pixel, dithered, so lights fade out smoothly.
class DEKI_RENDERING_API Lighting2D
{
public:
    static constexpr int32_t kBlock = 4;
    // Light in fixed point: kUnit is a colour as it is, kMax twice that.
    static constexpr uint32_t kUnit = 32;
    static constexpr uint32_t kMax = 64;

    /// Works out the frame's light: off when `scene` has no active Light.
    /// `cam` maps world to the whole view (not moved to a strip).
    void Build(const Deki::Scene* scene, const FrameCamera& cam, int32_t viewWidth, int32_t viewHeight,
               int32_t litUpToSortOrder);

    /// Whether the frame is lit at all.
    bool On() const { return m_On; }
    int32_t LitUpToSortOrder() const { return m_LitUpTo; }
    const std::vector<Light2D>& Lights() const { return m_Lights; }
    void Ambient(float& r, float& g, float& b) const;

    /// Lights `area` of `buffer` (its own pixels; empty for all of it), a
    /// buffer holding the view's rows from viewTop and columns from viewLeft.
    void Apply(uint8_t* buffer, int32_t width, int32_t height, Deki::ColorFormat format, const Deki::Rect& area,
               int32_t viewLeft, int32_t viewTop);

    /// The light of one block (view pixels / kBlock), per channel in kUnit.
    void BlockLight(int32_t bx, int32_t by, uint32_t& r, uint32_t& g, uint32_t& b);

    /// The light per block of the whole view, for the threads that light.
    struct Grid
    {
        const uint8_t* r;
        const uint8_t* g;
        const uint8_t* b;
        const uint8_t* even;  // 1 where a block is lit the same all over
        int32_t width;        // blocks a row
        int32_t height;       // rows of blocks
    };

    /// Redrawing only what changed: adds to `changed` where this frame's
    /// light differs from `last`'s, and makes it this frame's.
    void AddChanges(Lighting2D& last, DirtyRegion& changed) const;

private:
    bool m_On = false;
    int32_t m_LitUpTo = 0;
    int32_t m_ViewWidth = 0;
    int32_t m_ViewHeight = 0;
    float m_AmbientR = 0.0f;
    float m_AmbientG = 0.0f;
    float m_AmbientB = 0.0f;
    std::vector<Light2D> m_Lights;

    // The light per block over the whole view, in kUnit, worked out a row of
    // blocks at a time as a frame needs it, and kept for the next frame while
    // the lights stay as they were.
    std::vector<uint8_t> m_GridR, m_GridG, m_GridB;
    std::vector<uint8_t> m_GridHard;  // 1 where a light with hard rings reaches
    std::vector<uint8_t> m_GridEven;  // 1 where a block is lit the same all over (EvenRow)
    std::vector<uint8_t> m_GridRowDone, m_GridEvenDone;
    int32_t m_GridWidth = 0, m_GridHeight = 0;
    float m_GridAmbientR = -1.0f, m_GridAmbientG = -1.0f, m_GridAmbientB = -1.0f;
    std::vector<Light2D> m_GridLights;
    // One row of blocks as floats, where lights reach (m_RowTouched holds
    // m_RowGeneration there).
    std::vector<float> m_RowR, m_RowG, m_RowB;
    std::vector<uint32_t> m_RowTouched, m_RowHard;
    uint32_t m_RowGeneration = 0;

    void KeepOrForgetGrid();
    void LightBlockRow(int32_t by);
    void EvenRow(int32_t by);
};

}  // namespace DekiRendering
