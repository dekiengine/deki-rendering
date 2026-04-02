#pragma once

#include <cstdint>
#include <cmath>
#include "DekiComponent.h"
#include "reflection/DekiProperty.h"
#include "Color.h"

/**
 * @brief 2D Camera component for prefab rendering
 *
 * Everything is in pixels. A 64×64 sprite = 64×64 pixels on screen at zoom 1×.
 * Pixel-perfect mode is controlled at the project level (Display Settings).
 */
class CameraComponent : public DekiComponent
{
public:
    DEKI_COMPONENT(CameraComponent, DekiComponent, "Core", "146999a7-398f-4e52-a7c7-1e6a78bfb9c4", "")

    DEKI_EXPORT
    deki::Color clear_color = deki::Color(49, 77, 121);  // Background clear color

    DEKI_EXPORT
    int32_t zoom = 1;                    // Integer zoom multiplier (1, 2, 3, etc.)

    CameraComponent();
    virtual ~CameraComponent() = default;

    // Zoom accessors
    int32_t GetZoom() const { return zoom; }
    void SetZoom(int32_t z) { zoom = (z < 1) ? 1 : z; }

    // Get camera world position from owner object (pixels)
    float GetPositionX() const;
    float GetPositionY() const;

    // Get render position (snapped if pixel-perfect)
    float GetRenderX() const;
    float GetRenderY() const;

    // Get visible world size in pixels
    float GetVisibleWidth(int32_t screen_width) const;
    float GetVisibleHeight(int32_t screen_height) const;

    // Coordinate conversion
    void WorldToScreen(float world_x, float world_y,
                       int32_t screen_width, int32_t screen_height,
                       int32_t& screen_x, int32_t& screen_y) const;

    void ScreenToWorld(int32_t screen_x, int32_t screen_y,
                       int32_t screen_width, int32_t screen_height,
                       float& world_x, float& world_y) const;
};

// Generated property metadata (after class definition for offsetof)
#include "generated/CameraComponent.gen.h"
