#pragma once

#include <cmath>
#include <cstdint>

namespace DekiRendering
{

/// World-to-screen mapping captured once per frame from the camera.
///
/// Standard2DRenderer fills RenderContext::cam after the passes' BeginFrame.
/// Every built-in draw, and any pass that wants to, maps through it instead of
/// asking the camera per object, which saves a virtual call across DLLs, the
/// pixels-per-meter lookup and a transform read each time.
///
/// CameraComponent::WorldToScreen calls the same function on a fresh
/// snapshot, so the two agree bit for bit. Keep the arithmetic and its order
/// exactly as written: the renderer golden test pins it. This header and the
/// camera compile with the same flags in one package, so FMA contraction
/// (relevant on Xtensa) applies to both equally.
struct FrameCamera
{
    float camX = 0.0f;  // camera world position in metres, already on its grid under Pixel Perfect
    float camY = 0.0f;
    float ppm = 1.0f;    // framebuffer pixels per world metre
    float halfW = 0.0f;  // target centre in pixels: where world (camX, camY) lands
    float halfH = 0.0f;
    // Pixel Perfect: the grid the camera and every draw sit on, in screen
    // pixels, and 0 when off. 1 is the screen's own pixels (PixelGrid::Screen):
    // the camera moves a screen pixel at a time, which is smooth on a screen
    // that shows art larger. More is the art's pixels (PixelGrid::Art), that
    // many screen pixels each. halfW/halfH are whole, so anything placed on
    // the art-pixel grid lands on whole pixels either way; renderers snap
    // their draw positions with SnapX and SnapY.
    int32_t snapStep = 0;
    bool valid = false;  // false until captured (RenderContext's default)

    /// Under Pixel Perfect, rounds a screen position to the grid around the
    /// centre; otherwise returns it unchanged.
    float SnapX(float screenX) const
    {
        return snapStep > 0 ? halfW + std::round((screenX - halfW) / snapStep) * snapStep : screenX;
    }
    float SnapY(float screenY) const
    {
        return snapStep > 0 ? halfH + std::round((screenY - halfH) / snapStep) * snapStep : screenY;
    }

    void WorldToScreen(float worldX, float worldY, float& screenX, float& screenY) const
    {
        // World: metres, centre origin, Y up. Screen: top-left origin, Y down.
        const float relX = worldX - camX;
        const float relY = worldY - camY;
        screenX = relX * ppm + halfW;
        screenY = -relY * ppm + halfH;
    }
};

}  // namespace DekiRendering
