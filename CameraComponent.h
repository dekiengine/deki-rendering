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
 * @brief The scene's camera: what part of the world the player sees.
 *
 * Every screen renders at its own native size; the camera decides how much
 * world that shows. It fixes the height, and the width follows the screen, so
 * a bigger screen of the same shape shows the same world drawn bigger and a
 * wider one shows more at the sides:
 *
 *   Orthographic  screen_px = (world - camera) * ppm + buffer_center
 *                 ppm = OrthoPixelsPerMeter(h, orthoHeight, ...)
 *   Perspective   3D passes (deki-3d) use fieldOfView, vertical. Sprites
 *                 still draw flat through the orthographic mapping.
 *
 * With Pixel Perfect on, the scale is a whole multiple of the art density and
 * the camera sits on the art-pixel grid.
 */
DEKI_CATEGORY("Core")
DEKI_DESCRIPTION("The view: position, how much world it shows, and clear color.")
DEKI_FORMER_NAME("CameraComponent")
class CameraComponent : public Deki::Component, public Deki::ICamera
{
public:

    DEKI_EXPORT
    DEKI_TOOLTIP("Colour the screen is filled with before anything is drawn. What shows wherever nothing covers it.")
    Deki::Color clearColor = Deki::Color(49, 77, 121);  // Background clear color

    DEKI_TOOLTIP("Orthographic shows a fixed height of world, flat. Perspective shows a field of view in depth, for 3D meshes; sprites still draw flat.")
    DEKI_EXPORT
    Deki::ProjectionMode projection = Deki::ProjectionMode::Orthographic;

    DEKI_TOOLTIP("How much world the camera shows top to bottom. Every screen scales it to fill its height (the x beside it).")
    DEKI_UNIT(Distance)
    DEKI_RANGE(0.01f, 100000.0f)
    DEKI_VISIBLE_WHEN(projection, Orthographic)
    DEKI_EXPORT
    float orthoHeight = 15.0f;

    DEKI_TOOLTIP("For pixel art. Scales by whole numbers only, so every art pixel is the same size on screen, and snaps the camera to the art-pixel grid. Rounds down: the camera shows a little more than its ortho height, and a screen too small for 1x crops.")
    DEKI_VISIBLE_WHEN(projection, Orthographic)
    DEKI_EXPORT
    bool pixelPerfect = false;

    DEKI_TOOLTIP("Off, the camera never draws art bigger than 1x: a bigger screen shows more of the world instead.")
    DEKI_VISIBLE_WHEN(projection, Orthographic)
    DEKI_EXPORT
    bool scaleUp = true;

    DEKI_TOOLTIP("Vertical field of view in degrees. A wider screen sees more at the sides. 60 is a common default; larger looks wider and more distorted at the edges.")
    DEKI_RANGE(10, 150)
    DEKI_VISIBLE_WHEN(projection, Perspective)
    DEKI_EXPORT
    float fieldOfView = 60.0f;

    DEKI_TOOLTIP("Nothing closer than this is drawn. Raising it costs nothing and buys depth precision, so keep it as large as the scene allows.")
    DEKI_VISIBLE_WHEN(projection, Perspective)
    DEKI_EXPORT
    float nearPlane = 0.1f;

    DEKI_TOOLTIP("Nothing further than this is drawn.")
    DEKI_VISIBLE_WHEN(projection, Perspective)
    DEKI_EXPORT
    float farPlane = 100.0f;

    // Clear the framebuffer to clearColor before each frame. Turn off when the
    // first thing drawn covers the whole screen (a full-screen background
    // sprite or tilemap): the clear is a full framebuffer write per frame.
    DEKI_TOOLTIP("Clear before each frame. Off, the previous frame stays underneath: faster when a full-screen background covers everything, and occasionally what you want for trails.")
    DEKI_EXPORT
    bool clearEveryFrame = true;

    CameraComponent();
    virtual ~CameraComponent() = default;

    // ICamera: scale for a buffer of this size (its height over orthoHeight).
    float GetPixelsPerMeter(int bufferWidth, int bufferHeight) const override;
    float GetOrthoHeight() const override { return orthoHeight; }
    void SetOrthoHeight(float meters) override { orthoHeight = meters; }
    void SetFixedPixelsPerMeter(float ppm) override { m_FixedPixelsPerMeter = ppm > 0.0f ? ppm : 0.0f; }

    Deki::ProjectionMode GetProjectionMode() const override { return projection; }
    void SetProjectionMode(Deki::ProjectionMode mode) override { projection = mode; }
    float GetFieldOfView() const override { return fieldOfView; }
    float GetNearPlane() const override { return nearPlane; }
    float GetFarPlane() const override { return farPlane; }
    Deki::Mat4 GetProjectionMatrix(int bufferWidth, int bufferHeight) const override;

    // ICamera: Clear color
    void GetClearColor(uint8_t& r, uint8_t& g, uint8_t& b) const override { r = clearColor.r; g = clearColor.g; b = clearColor.b; }
    void SetClearColor(uint8_t r, uint8_t g, uint8_t b) override { clearColor.r = r; clearColor.g = g; clearColor.b = b; }

    // Camera world position (meters), from owner Deki::Object transform
    float GetPositionX() const;
    float GetPositionY() const;

    // Visible world size (meters) on a buffer of this size.
    float GetVisibleWidth(int32_t bufferWidth, int32_t bufferHeight) const;
    float GetVisibleHeight(int32_t bufferWidth, int32_t bufferHeight) const;

    // Snapshot of this camera's world-to-screen mapping for a target of the
    // given size: position (snapped under Pixel Perfect), pixels per meter,
    // centre. WorldToScreen below is this snapshot's WorldToScreen, so the two
    // agree exactly; the renderer captures it once per frame (RenderContext::cam).
    FrameCamera CaptureFrameCamera(int screenWidth, int screenHeight) const;

    // ICamera: Coordinate conversion (float in, float out)
    void WorldToScreen(float worldX, float worldY,
                       int screenWidth, int screenHeight,
                       float& screenX, float& screenY) const override;

    void ScreenToWorld(float screenX, float screenY,
                       int screenWidth, int screenHeight,
                       float& worldX, float& worldY) const override;

private:
    // Not exported: set by views that are not a screen (the editor's scene view).
    float m_FixedPixelsPerMeter = 0.0f;
};

}  // namespace DekiRendering
