#pragma once
#include <cstdint>

// Forward declarations
enum class DekiColorFormat;

/**
 * @brief Centralized quad blitting with full 2D transforms
 *
 * This provides a unified way to blit source pixel data to a target buffer
 * with position, scale, rotation, and alpha blending. All renderers can
 * produce raw pixel data at 1x scale and use QuadBlit for transforms.
 */
namespace QuadBlit
{
    /**
     * @brief Clip rectangle for restricting rendering output
     */
    struct ClipRect
    {
        int32_t left = 0;
        int32_t top = 0;
        int32_t right = INT32_MAX;
        int32_t bottom = INT32_MAX;

        bool IsSet() const { return right != INT32_MAX; }
    };

    /**
     * @brief Push a clip rect onto the stack (intersects with parent)
     */
    void PushClipRect(int32_t left, int32_t top, int32_t right, int32_t bottom);

    /**
     * @brief Pop the current clip rect from the stack
     */
    void PopClipRect();

    /**
     * @brief Get the current active clip rect
     */
    ClipRect GetCurrentClipRect();

    /**
     * @brief Clear the clip rect stack (call at frame start)
     */
    void ClearClipStack();

    /**
     * @brief Temporarily enable/disable clip rect enforcement
     * @param enabled If false, blits ignore the clip stack until re-enabled
     */
    void SetClipEnabled(bool enabled);

    /**
     * @brief Check if clipping is currently enabled
     */
    bool IsClipEnabled();

    /**
     * @brief Get current clip stack depth (diagnostic)
     */
    int GetClipStackDepth();

    /**
     * @brief Source buffer descriptor
     */
    struct Source
    {
        const uint8_t* pixels;    // Source pixel data
        int32_t width;            // Width in pixels
        int32_t height;           // Height in pixels
        int32_t bytesPerPixel;    // Bytes per pixel (2, 3, or 4)
        bool hasAlpha;            // Whether source has alpha channel
        uint8_t alphaOffset;      // Byte offset to alpha (for RGB565A8: 2, for RGBA8888: 3)

        // Pixel format info for color extraction
        bool isRGB565;            // True if RGB565 or RGB565A8 format

        // Ownership flag - if true, caller should delete[] pixels after blitting
        // Sprites set this to false since they own their own pixel data
        bool ownsPixels;
    };

    /**
     * @brief Create source descriptor for a given texture format
     * @param ownsPixels If true, caller should delete[] pixels after use
     */
    Source MakeSource(const uint8_t* pixels, int32_t width, int32_t height,
                      int32_t bytesPerPixel, bool hasAlpha, bool isRGB565,
                      bool ownsPixels = true);

    /**
     * @brief Blit source buffer to target with full 2D transforms
     *
     * @param source Source buffer descriptor
     * @param target Target buffer pointer
     * @param targetWidth Target buffer width
     * @param targetHeight Target buffer height
     * @param targetFormat Target color format (RGB565, RGB888, ARGB8888)
     * @param screenX Screen X position (after WorldToScreen)
     * @param screenY Screen Y position (after WorldToScreen)
     * @param scaleX Horizontal scale factor (from world_scale_x)
     * @param scaleY Vertical scale factor (from world_scale_y)
     * @param rotation Rotation in radians (from world_rotation) - currently unused
     * @param pivotX Pivot X (0.0 = left, 0.5 = center, 1.0 = right)
     * @param pivotY Pivot Y (0.0 = top, 0.5 = center, 1.0 = bottom)
     * @param tintR Tint color red (255 = no tint)
     * @param tintG Tint color green (255 = no tint)
     * @param tintB Tint color blue (255 = no tint)
     * @param tintA Tint alpha (255 = opaque, 0 = invisible)
     */
    void Blit(const Source& source,
              uint8_t* target,
              int32_t targetWidth,
              int32_t targetHeight,
              DekiColorFormat targetFormat,
              int32_t screenX,
              int32_t screenY,
              float scaleX,
              float scaleY,
              float rotation,
              float pivotX,
              float pivotY,
              uint8_t tintR = 255,
              uint8_t tintG = 255,
              uint8_t tintB = 255,
              uint8_t tintA = 255);

    /**
     * @brief Simplified blit without rotation (faster path)
     */
    void BlitScaled(const Source& source,
                    uint8_t* target,
                    int32_t targetWidth,
                    int32_t targetHeight,
                    DekiColorFormat targetFormat,
                    int32_t destX,
                    int32_t destY,
                    int32_t destWidth,
                    int32_t destHeight,
                    uint8_t tintR = 255,
                    uint8_t tintG = 255,
                    uint8_t tintB = 255,
                    uint8_t tintA = 255);

    /**
     * @brief Enable byte-swapping for RGB565 target writes (for display controllers)
     * When enabled, all RGB565 pixels are written in big-endian byte order.
     */
    void SetByteSwap(bool enabled);
    bool GetByteSwap();
}
