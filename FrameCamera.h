#pragma once

#include <cmath>
#include <cstdint>

namespace DekiRendering
{

/**
 * @brief World-to-screen mapping captured once per frame from the camera.
 *
 * Standard2DRenderer fills RenderContext::cam after the passes' BeginFrame and
 * every built-in draw (and any pass that wants it) maps through this instead of
 * calling the camera per object: no virtual call, no cross-DLL call, no
 * re-resolving the project pixels-per-meter, no camera transform read.
 *
 * CameraComponent::WorldToScreen delegates to the same function on a fresh
 * snapshot, so the two agree bit for bit. Keep the arithmetic and its order
 * exactly as written: the renderer golden test pins it. Both this header and
 * the camera compile with the same flags inside one package, so FMA
 * contraction (relevant on Xtensa) applies to both equally.
 */
struct FrameCamera
{
    float camX = 0.0f;   // camera world position (metres), already camera-pixel-snapped
    float camY = 0.0f;
    float ppm = 1.0f;    // framebuffer pixels per world metre
    float halfW = 0.0f;  // target centre (pixels): screen origin of world (camX, camY)
    float halfH = 0.0f;
    // Pixel Perfect: screen pixels per art pixel (a whole number >= 1), and 0
    // when off. The camera is then on the art-pixel grid and halfW/halfH are
    // whole, so anything placed on that grid lands on whole multiples of it;
    // renderers snap their draw positions to it (SnapToArtGrid).
    int32_t snapStep = 0;
    bool valid = false;  // false until captured; RenderContext default

    /// Under Pixel Perfect, round a screen position to the art-pixel grid
    /// around the centre; otherwise return it unchanged.
    float SnapX(float screenX) const { return snapStep > 0 ? halfW + std::round((screenX - halfW) / snapStep) * snapStep : screenX; }
    float SnapY(float screenY) const { return snapStep > 0 ? halfH + std::round((screenY - halfH) / snapStep) * snapStep : screenY; }

    void WorldToScreen(float worldX, float worldY, float& screenX, float& screenY) const
    {
        // World: metres, centre origin, Y up. Screen: top-left origin, Y down.
        const float rel_x = worldX - camX;
        const float rel_y = worldY - camY;
        screenX = rel_x * ppm + halfW;
        screenY = -rel_y * ppm + halfH;
    }
};

}  // namespace DekiRendering
