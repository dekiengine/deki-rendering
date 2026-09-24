#include "CameraComponent.h"
#include <deki/Object.h>
#include <deki/Engine.h>
#include <deki/ICamera.h>
#include <deki/ScreenScale.h>
#include <deki/ComponentInterfaceAdapters.h>

namespace DekiRendering
{

// Register ICamera adapter so editor can use FindInterface<ICamera>()
static struct CameraInterfaceRegistrar {
    CameraInterfaceRegistrar() {
        Deki::ComponentInterfaceAdapters::Register(
            Deki::ICamera::InterfaceID, ::Deki::TypeId<CameraComponent>(),
            [](Deki::Component* c) -> void* {
                return static_cast<Deki::ICamera*>(static_cast<CameraComponent*>(c));
            });
    }
} s_cameraInterfaceReg;

CameraComponent::CameraComponent()
{
}

float CameraComponent::GetPixelsPerMeter(int bufferWidth, int bufferHeight) const
{
    if (m_FixedPixelsPerMeter > 0.0f)
        return m_FixedPixelsPerMeter;

    const Deki::EngineSettings& s = Deki::EngineSettings::Global();
    const float fit = Deki::ResolveScreenPixelsPerMeter(bufferWidth, bufferHeight, s);
    if (fit <= 0.0f || zoom <= 0.0f)
        return 0.0f;
    if (!s.pixelPerfect)
        return fit * zoom;

    // Whole art-pixel multiples only: the fit is already one; round the zoomed
    // multiple to the nearest whole number, never below 1.
    const float multiple = std::round((fit / s.pixelsPerMeter) * zoom);
    return s.pixelsPerMeter * (multiple < 1.0f ? 1.0f : multiple);
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
    // World: meters, center origin, Y UP (positive Y = up)
    // Screen: top-left origin, Y down
    // Camera position is the world point that maps to screen center.
    FrameCamera fc;
    fc.ppm = GetPixelsPerMeter(screenWidth, screenHeight);
    fc.camX = GetPositionX();
    fc.camY = GetPositionY();
    fc.halfW = static_cast<float>(screenWidth) * 0.5f;
    fc.halfH = static_cast<float>(screenHeight) * 0.5f;

    // Pixel Perfect: the camera sits on the art-pixel grid and the centre on a
    // whole screen pixel, so every art pixel covers the same block of screen
    // pixels however the camera moves. The scene view's fixed scale is not a
    // screen and is left alone.
    const Deki::EngineSettings& s = Deki::EngineSettings::Global();
    if (s.pixelPerfect && m_FixedPixelsPerMeter <= 0.0f && s.pixelsPerMeter > 0.0f && fc.ppm > 0.0f)
    {
        const float art = s.pixelsPerMeter;
        fc.camX = std::round(fc.camX * art) / art;
        fc.camY = std::round(fc.camY * art) / art;
        fc.halfW = std::floor(fc.halfW);
        fc.halfH = std::floor(fc.halfH);
        fc.snapStep = static_cast<int32_t>(std::lround(fc.ppm / art));
    }
    fc.valid = fc.ppm > 0.0f;
    return fc;
}

Deki::Mat4 CameraComponent::GetProjectionMatrix(int bufferWidth, int bufferHeight) const
{
    if (bufferWidth <= 0 || bufferHeight <= 0)
        return Deki::Mat4::Identity();

    if (projection == Deki::ProjectionMode::Perspective)
    {
        constexpr float kDegToRad = 3.14159265358979f / 180.0f;
        const float fov = Deki::ResolveVerticalFieldOfView(fieldOfView, bufferWidth, bufferHeight);
        return Deki::Mat4::Perspective(fov * kDegToRad,
                                       static_cast<float>(bufferWidth) / static_cast<float>(bufferHeight),
                                       nearPlane, farPlane);
    }

    const float ppm = GetPixelsPerMeter(bufferWidth, bufferHeight);
    if (ppm <= 0.0f)
        return Deki::Mat4::Identity();
    const float halfW = (static_cast<float>(bufferWidth) * 0.5f) / ppm;
    const float halfH = (static_cast<float>(bufferHeight) * 0.5f) / ppm;
    return Deki::Mat4::Ortho(-halfW, halfW, -halfH, halfH, -1.0f, 1.0f);
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
    // Inverse of WorldToScreen, through the same snapshot.
    const FrameCamera fc = CaptureFrameCamera(screenWidth, screenHeight);
    const float inv = (fc.ppm > 0.0f) ? (1.0f / fc.ppm) : 0.0f;
    worldX = (screenX - fc.halfW) * inv + fc.camX;
    worldY = -(screenY - fc.halfH) * inv + fc.camY;  // screen Y down -> world Y up
}

}  // namespace DekiRendering
