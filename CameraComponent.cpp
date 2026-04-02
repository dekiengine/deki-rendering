#include "CameraComponent.h"
#include "DekiObject.h"
#include "DekiEngine.h"

// ============================================================================
// Component Registration
// ============================================================================
// NOTE: s_Properties[] and s_ComponentMeta are now auto-generated in
// CameraComponent.gen.h (included at end of CameraComponent.h)


// ============================================================================

CameraComponent::CameraComponent()
    : zoom(1)
{
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

float CameraComponent::GetRenderX() const
{
    // Engine is always pixel-perfect - floor camera position
    return std::floor(GetPositionX());
}

float CameraComponent::GetRenderY() const
{
    // Engine is always pixel-perfect - floor camera position
    return std::floor(GetPositionY());
}

float CameraComponent::GetVisibleWidth(int32_t screen_width) const
{
    return static_cast<float>(screen_width) / static_cast<float>(zoom);
}

float CameraComponent::GetVisibleHeight(int32_t screen_height) const
{
    return static_cast<float>(screen_height) / static_cast<float>(zoom);
}

void CameraComponent::WorldToScreen(float world_x, float world_y,
                                     int32_t screen_width, int32_t screen_height,
                                     int32_t& screen_x, int32_t& screen_y) const
{
    // World: center origin, Y UP (positive Y = up)
    // Screen: top-left origin, Y down
    // Camera position is the world point that maps to screen center
    float rel_x = world_x - GetRenderX();
    float rel_y = world_y - GetRenderY();

    screen_x = static_cast<int32_t>(std::floor(rel_x * zoom)) + screen_width / 2;
    screen_y = static_cast<int32_t>(std::floor(-rel_y * zoom)) + screen_height / 2;  // Negate Y: world Y+ is up
}

void CameraComponent::ScreenToWorld(int32_t screen_x, int32_t screen_y,
                                     int32_t screen_width, int32_t screen_height,
                                     float& world_x, float& world_y) const
{
    // Screen: top-left origin, Y down
    // World: center origin, Y UP (positive Y = up)
    float rel_x = static_cast<float>(screen_x - screen_width / 2) / static_cast<float>(zoom);
    float rel_y = static_cast<float>(screen_y - screen_height / 2) / static_cast<float>(zoom);

    world_x = rel_x + GetRenderX();
    world_y = -rel_y + GetRenderY();  // Negate Y: screen Y down -> world Y up
}
