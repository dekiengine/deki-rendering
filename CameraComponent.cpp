#include "CameraComponent.h"
#include <algorithm>
#include <deki/Object.h>
#include <deki/Engine.h>
#include <deki/ICamera.h>
#include <deki/ScreenScale.h>
#include <deki/ComponentInterfaceAdapters.h>

namespace DekiRendering
{

// Registers the ICamera adapter so the editor can use FindInterface<ICamera>().
static struct CameraInterfaceRegistrar
{
    CameraInterfaceRegistrar()
    {
        Deki::ComponentInterfaceAdapters::Register(
            Deki::ICamera::kInterfaceID, ::Deki::TypeId<CameraComponent>(),
            [](Deki::Component* c) -> void* { return static_cast<Deki::ICamera*>(static_cast<CameraComponent*>(c)); });
    }
} s_CameraInterfaceReg;

CameraComponent::CameraComponent()
{
}

float CameraComponent::GetPixelsPerMeter(int bufferWidth, int bufferHeight) const
{
    if (m_FixedPixelsPerMeter > 0.0f)
    {
        return m_FixedPixelsPerMeter;
    }

    (void)bufferWidth;  // the height is fixed; the width follows the screen
    return Deki::OrthoPixelsPerMeter(bufferHeight, orthoHeight, pixelPerfect,
                                     Deki::EngineSettings::Global().pixelsPerMeter);
}

float CameraComponent::GetPositionX() const
{
    Deki::Object* owner = GetOwner();
    return owner ? owner->GetWorldX() : 0.0f;
}

float CameraComponent::GetPositionY() const
{
    Deki::Object* owner = GetOwner();
    return owner ? owner->GetWorldY() : 0.0f;
}

float CameraComponent::GetVisibleWidth(int32_t bufferWidth, int32_t bufferHeight) const
{
    const float ppm = GetPixelsPerMeter(bufferWidth, bufferHeight);
    return (ppm > 0.0f) ? (static_cast<float>(bufferWidth) / ppm) : 0.0f;
}

float CameraComponent::GetVisibleHeight(int32_t bufferWidth, int32_t bufferHeight) const
{
    const float ppm = GetPixelsPerMeter(bufferWidth, bufferHeight);
    return (ppm > 0.0f) ? (static_cast<float>(bufferHeight) / ppm) : 0.0f;
}

FrameCamera CameraComponent::CaptureFrameCamera(int screenWidth, int screenHeight) const
{
    // World: meters, centre origin, Y up. Screen: top-left origin, Y down.
    // The camera position is the world point that maps to the screen centre.
    FrameCamera fc;
    fc.ppm = GetPixelsPerMeter(screenWidth, screenHeight);
    fc.camX = GetPositionX();
    fc.camY = GetPositionY();
    fc.halfW = static_cast<float>(screenWidth) * 0.5f;
    fc.halfH = static_cast<float>(screenHeight) * 0.5f;

    // Pixel Perfect: the camera sits on its grid and the centre on a whole
    // screen pixel, so what is on the art-pixel grid lands on whole pixels
    // however the camera moves. The grid is the screen's pixels, or the art's
    // (Pixel Grid). The scene view's fixed scale is not a screen and is not
    // snapped.
    const float art = Deki::EngineSettings::Global().pixelsPerMeter;
    if (pixelPerfect && m_FixedPixelsPerMeter <= 0.0f && art > 0.0f && fc.ppm > 0.0f)
    {
        const float grid = pixelGrid == PixelGrid::Art ? art : fc.ppm;
        fc.camX = std::round(fc.camX * grid) / grid;
        fc.camY = std::round(fc.camY * grid) / grid;
        fc.halfW = std::floor(fc.halfW);
        fc.halfH = std::floor(fc.halfH);
        fc.snapStep = std::max<int32_t>(1, static_cast<int32_t>(std::lround(fc.ppm / grid)));
    }
    fc.valid = fc.ppm > 0.0f;
    return fc;
}

Deki::Mat4 CameraComponent::GetProjectionMatrix(int bufferWidth, int bufferHeight) const
{
    if (bufferWidth <= 0 || bufferHeight <= 0)
    {
        return Deki::Mat4::Identity();
    }

    if (projection == Deki::ProjectionMode::Perspective)
    {
        constexpr float kDegToRad = 3.14159265358979f / 180.0f;
        return Deki::Mat4::Perspective(fieldOfView * kDegToRad,
                                       static_cast<float>(bufferWidth) / static_cast<float>(bufferHeight), nearPlane,
                                       farPlane);
    }

    const float ppm = GetPixelsPerMeter(bufferWidth, bufferHeight);
    if (ppm <= 0.0f)
    {
        return Deki::Mat4::Identity();
    }
    const float halfW = (static_cast<float>(bufferWidth) * 0.5f) / ppm;
    const float halfH = (static_cast<float>(bufferHeight) * 0.5f) / ppm;
    return Deki::Mat4::Ortho(-halfW, halfW, -halfH, halfH, -1.0f, 1.0f);
}

void CameraComponent::WorldToScreen(float worldX, float worldY, int screenWidth, int screenHeight, float& screenX,
                                    float& screenY) const
{
    CaptureFrameCamera(screenWidth, screenHeight).WorldToScreen(worldX, worldY, screenX, screenY);
}

void CameraComponent::ScreenToWorld(float screenX, float screenY, int screenWidth, int screenHeight, float& worldX,
                                    float& worldY) const
{
    // Inverse of WorldToScreen, through the same snapshot.
    const FrameCamera fc = CaptureFrameCamera(screenWidth, screenHeight);
    const float inv = (fc.ppm > 0.0f) ? (1.0f / fc.ppm) : 0.0f;
    worldX = (screenX - fc.halfW) * inv + fc.camX;
    worldY = -(screenY - fc.halfH) * inv + fc.camY;  // screen Y down -> world Y up
}

}  // namespace DekiRendering
