#pragma once

#include <cstdint>
#include <cmath>
#include "DekiComponent.h"
#include "ICamera.h"
#include "reflection/DekiProperty.h"
#include "Color.h"

/**
 * @brief 2D Camera component for prefab rendering
 *
 * Everything is in pixels. A 64×64 sprite = 64×64 pixels on screen at zoom 1×.
 * Pixel-perfect mode is controlled at the project level (Display Settings).
 */
class CameraComponent : public DekiComponent, public ICamera
{
public:
    DEKI_COMPONENT(CameraComponent, DekiComponent, "Core", "146999a7-398f-4e52-a7c7-1e6a78bfb9c4", "")

    DEKI_EXPORT
    deki::Color clear_color = deki::Color(49, 77, 121);  // Background clear color

    DEKI_EXPORT
    int32_t zoom = 1;                    // Integer zoom multiplier (1, 2, 3, etc.)

    CameraComponent();
    virtual ~CameraComponent() = default;

    // ICamera: Zoom
    int32_t GetZoom() const override { return zoom; }
    void SetZoom(int32_t z) override { zoom = (z < 1) ? 1 : z; }

    // ICamera: Clear color
    void GetClearColor(uint8_t& r, uint8_t& g, uint8_t& b) const override { r = clear_color.r; g = clear_color.g; b = clear_color.b; }
    void SetClearColor(uint8_t r, uint8_t g, uint8_t b) override { clear_color.r = r; clear_color.g = g; clear_color.b = b; }

    // Get camera world position from owner object (pixels)
    float GetPositionX() const;
    float GetPositionY() const;

    // Get render position (snapped if pixel-perfect)
    float GetRenderX() const;
    float GetRenderY() const;

    // Get visible world size in pixels
    float GetVisibleWidth(int32_t screen_width) const;
    float GetVisibleHeight(int32_t screen_height) const;

    // ICamera: Coordinate conversion
    void WorldToScreen(float world_x, float world_y,
                       int32_t screen_width, int32_t screen_height,
                       int32_t& screen_x, int32_t& screen_y) const override;

    void ScreenToWorld(int32_t screen_x, int32_t screen_y,
                       int32_t screen_width, int32_t screen_height,
                       float& world_x, float& world_y) const override;
};

// Generated property metadata (after class definition for offsetof)
#include "generated/CameraComponent.gen.h"
