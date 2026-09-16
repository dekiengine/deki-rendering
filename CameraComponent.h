#pragma once

#include <cstdint>
#include <cmath>
#include <deki/Component.h>
#include <deki/ICamera.h>
#include "FrameCamera.h"
#include <deki/reflection/Property.h>
#include <deki/Color.h>

namespace DekiRendering
{

/**
 * @brief 2D Camera component for scene rendering.
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
 * (Deki::EngineSettings::Global().pixelsPerMeter) at runtime — this keeps
 * untouched cameras tracking project changes. Setting an explicit value
 * (e.g. via inspector or SetPixelsPerMeter) makes the camera sticky.
 */
DEKI_CATEGORY("Core")
DEKI_DESCRIPTION("The view: clear color, zoom (pixels per meter) and pixel snap.")
DEKI_FORMER_NAME("CameraComponent")
class CameraComponent : public Deki::Component, public Deki::ICamera
{
public:

    DEKI_EXPORT
    DEKI_TOOLTIP("Colour the screen is filled with before anything is drawn. What shows wherever nothing covers it.")
    Deki::Color clearColor = Deki::Color(49, 77, 121);  // Background clear color

    // Framebuffer pixels per world meter. 0 = inherit project default.
    DEKI_EXPORT
    DEKI_TOOLTIP("How many framebuffer pixels make up one meter of the world, which is what sets the zoom. Left at 0 the project's own setting is used.")
    float pixelsPerMeter = 0.0f;

    // Pixel-perfect rendering. When true, the camera's contribution is
    // rounded to whole logical pixels (suppresses sub-pixel camera shake);
    // per-renderer pixelSnap still applies on top of this.
    DEKI_EXPORT
    bool pixelSnap = false;

    // Clear the framebuffer to clearColor before each frame. Turn off when the
    // first thing drawn covers the whole screen (a full-screen background
    // sprite or tilemap): the clear is a full framebuffer write per frame.
    DEKI_TOOLTIP("Clear the framebuffer to the clear color before drawing. Turn off when a full-screen background covers everything: saves a full framebuffer write per frame.")
    DEKI_EXPORT
    DEKI_TOOLTIP("Clear before each frame. Off, the previous frame stays underneath, which is faster and occasionally what you want for trails.")
    bool clearEveryFrame = true;

    // Projection mode (forward-compat hook). Hidden in inspector — only
    // Orthographic is meaningful for the 2D software renderer today.
    Deki::ProjectionMode projectionMode = Deki::ProjectionMode::Orthographic;

    CameraComponent();
    virtual ~CameraComponent() = default;

    // ICamera: pixels per meter. Resolves the 0-sentinel to the project
    // default. Setter clamps to a positive value or stores 0 (inherit).
    float GetPixelsPerMeter() const override;
    void SetPixelsPerMeter(float ppm) override;

    // ICamera: Projection mode (forward-compat hook).
    Deki::ProjectionMode GetProjectionMode() const override { return projectionMode; }
    void SetProjectionMode(Deki::ProjectionMode mode) override { projectionMode = mode; }

    // ICamera: Clear color
    void GetClearColor(uint8_t& r, uint8_t& g, uint8_t& b) const override { r = clearColor.r; g = clearColor.g; b = clearColor.b; }
    void SetClearColor(uint8_t r, uint8_t g, uint8_t b) override { clearColor.r = r; clearColor.g = g; clearColor.b = b; }

    // Camera world position (meters), from owner Deki::Object transform
    float GetPositionX() const;
    float GetPositionY() const;

    // Visible world size (meters) for a given screen size in pixels.
    float GetVisibleWidth(int32_t screenWidth) const;
    float GetVisibleHeight(int32_t screenHeight) const;

    // Snapshot of this camera's world-to-screen mapping for a target of the
    // given size: position (camera-pixel-snapped), pixels per meter, centre.
    // WorldToScreen below is this snapshot's WorldToScreen, so the two agree
    // exactly; the renderer captures it once per frame (RenderContext::cam).
    FrameCamera CaptureFrameCamera(int screenWidth, int screenHeight) const;

    // ICamera: Coordinate conversion (float in, float out)
    void WorldToScreen(float worldX, float worldY,
                       int screenWidth, int screenHeight,
                       float& screenX, float& screenY) const override;

    void ScreenToWorld(float screenX, float screenY,
                       int screenWidth, int screenHeight,
                       float& worldX, float& worldY) const override;
};

// Generated property metadata (after class definition for offsetof)

}  // namespace DekiRendering
