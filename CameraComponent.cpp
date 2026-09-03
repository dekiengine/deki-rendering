#include "CameraComponent.h"
#include "DekiObject.h"
#include "DekiEngine.h"
#include "ICamera.h"
#include "ComponentInterfaceAdapters.h"

// ============================================================================
// Component Registration
// ============================================================================
// NOTE: s_Properties[] and s_ComponentMeta are now auto-generated in
// CameraComponent.gen.h (included at end of CameraComponent.h)

// Register ICamera adapter so editor can use FindInterface<ICamera>()
static struct CameraInterfaceRegistrar {
    CameraInterfaceRegistrar() {
        ComponentInterfaceAdapters::Register(
            ICamera::InterfaceID, CameraComponent::StaticType,
            [](DekiComponent* c) -> void* {
                return static_cast<ICamera*>(static_cast<CameraComponent*>(c));
            });
    }
} s_cameraInterfaceReg;


// ============================================================================

CameraComponent::CameraComponent()
{
}

float CameraComponent::GetPixelsPerMeter() const
{
    if (pixelsPerMeter > 0.0f)
        return pixelsPerMeter;
    const float global = DekiEngineSettings::Global().pixelsPerMeter;
    return global > 0.0f ? global : 1.0f;
}

void CameraComponent::SetPixelsPerMeter(float ppm)
{
    pixelsPerMeter = (ppm > 0.0f) ? ppm : 0.0f;
}

float CameraComponent::GetPositionX() const
{
    DekiObject* owner = GetOwner();
    return owner ? owner->GetWorldX() : 0.0f;
}

float CameraComponent::GetPositionY() const
{
    DekiObject* owner = GetOwner();
    return owner ? owner->GetWorldY() : 0.0f;
}

float CameraComponent::GetVisibleWidth(int32_t screenWidth) const
{
    const float ppm = GetPixelsPerMeter();
    return (ppm > 0.0f) ? (static_cast<float>(screenWidth) / ppm) : 0.0f;
}

float CameraComponent::GetVisibleHeight(int32_t screenHeight) const
{
    const float ppm = GetPixelsPerMeter();
    return (ppm > 0.0f) ? (static_cast<float>(screenHeight) / ppm) : 0.0f;
}

FrameCamera CameraComponent::CaptureFrameCamera(int screenWidth, int screenHeight) const
{
    // World: meters, center origin, Y UP (positive Y = up)
    // Screen: top-left origin, Y down
    // Camera position is the world point that maps to screen center.
    //
    // When pixelSnap is on, the camera's own contribution is rounded to
    // whole pixels (cam_x_px = round(cam_x * ppm) / ppm) so smooth camera
    // tweens / shake quantize at the camera level. The per-renderer
    // pixelSnap still applies on top of this.
    FrameCamera fc;
    fc.ppm = GetPixelsPerMeter();
    fc.camX = GetPositionX();
    fc.camY = GetPositionY();
    if (pixelSnap && fc.ppm > 0.0f)
    {
        fc.camX = std::round(fc.camX * fc.ppm) / fc.ppm;
        fc.camY = std::round(fc.camY * fc.ppm) / fc.ppm;
    }
    fc.halfW = static_cast<float>(screenWidth) * 0.5f;
    fc.halfH = static_cast<float>(screenHeight) * 0.5f;
    fc.valid = true;
    return fc;
}

void CameraComponent::WorldToScreen(float worldX, float worldY,
                                     int screenWidth, int screenHeight,
                                     float& screenX, float& screenY) const
{
    CaptureFrameCamera(screenWidth, screenHeight).WorldToScreen(worldX, worldY, screenX, screenY);
}

void CameraComponent::ScreenToWorld(float screenX, float screenY,
                                     int screenWidth, int screenHeight,
                                     float& worldX, float& worldY) const
{
    // Inverse of WorldToScreen.
    const float ppm = GetPixelsPerMeter();
    const float inv = (ppm > 0.0f) ? (1.0f / ppm) : 0.0f;
    const float rel_x = (screenX - static_cast<float>(screenWidth) * 0.5f) * inv;
    const float rel_y = (screenY - static_cast<float>(screenHeight) * 0.5f) * inv;

    worldX = rel_x + GetPositionX();
    worldY = -rel_y + GetPositionY(); // Negate Y: screen Y down -> world Y up
}
