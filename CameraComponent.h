#pragma once

#include <cstdint>
#include <cmath>
#include "DekiComponent.h"
#include "ICamera.h"
#include "reflection/DekiProperty.h"
#include "Color.h"

/**
 * @brief 2D Camera component for prefab rendering.
 *
 * The camera owns viewport-related state: clear color, pixels-per-meter
 * (controls zoom), and optional pixel snap. Internal world coords are
 * always in meters; the camera's pixelsPerMeter is the only knob that
 * scales world→screen.
 *
 * World → screen pixels:
 *   screen_px = (world_meters * pixelsPerMeter) + buffer_center
 *
 * If pixelsPerMeter == 0, the camera resolves to the project default
 * (DekiEngineSettings::Global().pixelsPerMeter) at runtime — this keeps
 * untouched cameras tracking project changes. Setting an explicit value
 * (e.g. via inspector or SetPixelsPerMeter) makes the camera sticky.
 */
class CameraComponent : public DekiComponent, public ICamera
{
public:
    DEKI_COMPONENT(CameraComponent, DekiComponent, "Core", "146999a7-398f-4e52-a7c7-1e6a78bfb9c4", "")

    DEKI_EXPORT
    deki::Color clear_color = deki::Color(49, 77, 121);  // Background clear color

    // Framebuffer pixels per world meter. 0 = inherit project default.
    DEKI_EXPORT
    float pixelsPerMeter = 0.0f;

    // Pixel-perfect rendering. When true, the camera's contribution is
    // rounded to whole logical pixels (suppresses sub-pixel camera shake);
    // per-renderer pixel_snap still applies on top of this.
    DEKI_EXPORT
    bool pixel_snap = false;

    // Projection mode (forward-compat hook). Hidden in inspector — only
    // Orthographic is meaningful for the 2D software renderer today.
    ProjectionMode projection_mode = ProjectionMode::Orthographic;

    CameraComponent();
    virtual ~CameraComponent() = default;

    // ICamera: pixels per meter. Resolves the 0-sentinel to the project
    // default. Setter clamps to a positive value or stores 0 (inherit).
    float GetPixelsPerMeter() const override;
    void SetPixelsPerMeter(float ppm) override;

    // ICamera: Projection mode (forward-compat hook).
    ProjectionMode GetProjectionMode() const override { return projection_mode; }
    void SetProjectionMode(ProjectionMode mode) override { projection_mode = mode; }

    // ICamera: Clear color
    void GetClearColor(uint8_t& r, uint8_t& g, uint8_t& b) const override { r = clear_color.r; g = clear_color.g; b = clear_color.b; }
    void SetClearColor(uint8_t r, uint8_t g, uint8_t b) override { clear_color.r = r; clear_color.g = g; clear_color.b = b; }

    // Camera world position (meters), from owner DekiObject transform
    float GetPositionX() const;
    float GetPositionY() const;

    // Visible world size (meters) for a given screen size in pixels.
    float GetVisibleWidth(int32_t screen_width) const;
    float GetVisibleHeight(int32_t screen_height) const;

    // ICamera: Coordinate conversion (float in, float out)
    void WorldToScreen(float world_x, float world_y,
                       int screen_width, int screen_height,
                       float& screen_x, float& screen_y) const override;

    void ScreenToWorld(float screen_x, float screen_y,
                       int screen_width, int screen_height,
                       float& world_x, float& world_y) const override;
};

// Generated property metadata (after class definition for offsetof)
#include "generated/CameraComponent.gen.h"
