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

float CameraComponent::GetVisibleWidth(int32_t screen_width) const
{
    const float ppm = GetPixelsPerMeter();
    return (ppm > 0.0f) ? (static_cast<float>(screen_width) / ppm) : 0.0f;
}

float CameraComponent::GetVisibleHeight(int32_t screen_height) const
{
    const float ppm = GetPixelsPerMeter();
    return (ppm > 0.0f) ? (static_cast<float>(screen_height) / ppm) : 0.0f;
}

void CameraComponent::WorldToScreen(float world_x, float world_y,
                                     int screen_width, int screen_height,
                                     float& screen_x, float& screen_y) const
{
    // World: meters, center origin, Y UP (positive Y = up)
    // Screen: top-left origin, Y down
    // Camera position is the world point that maps to screen center.
    //
    // When pixel_snap is on, the camera's own contribution is rounded to
    // whole pixels (cam_x_px = round(cam_x * ppm) / ppm) so smooth camera
    // tweens / shake quantize at the camera level. The per-renderer
    // pixel_snap still applies on top of this.
    const float ppm = GetPixelsPerMeter();
    float cam_x = GetPositionX();
    float cam_y = GetPositionY();
    if (pixel_snap && ppm > 0.0f)
    {
        cam_x = std::round(cam_x * ppm) / ppm;
        cam_y = std::round(cam_y * ppm) / ppm;
    }
    const float rel_x = world_x - cam_x;
    const float rel_y = world_y - cam_y;

    screen_x = rel_x * ppm + static_cast<float>(screen_width) * 0.5f;
    screen_y = -rel_y * ppm + static_cast<float>(screen_height) * 0.5f; // Negate Y: world Y+ is up
}

void CameraComponent::ScreenToWorld(float screen_x, float screen_y,
                                     int screen_width, int screen_height,
                                     float& world_x, float& world_y) const
{
    // Inverse of WorldToScreen.
    const float ppm = GetPixelsPerMeter();
    const float inv = (ppm > 0.0f) ? (1.0f / ppm) : 0.0f;
    const float rel_x = (screen_x - static_cast<float>(screen_width) * 0.5f) * inv;
    const float rel_y = (screen_y - static_cast<float>(screen_height) * 0.5f) * inv;

    world_x = rel_x + GetPositionX();
    world_y = -rel_y + GetPositionY(); // Negate Y: screen Y down -> world Y up
}
