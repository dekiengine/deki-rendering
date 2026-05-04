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

        // Per-row opaque span data (optional, nullptr if not available)
        // Packed int16_t pairs: [opaqueStart, opaqueEnd] per row
        const int16_t* alphaRowSpans;

        // Ownership flag - if true, caller should delete[] pixels after blitting
        // Sprites set this to false since they own their own pixel data
        bool ownsPixels;

        // Bytes per row in source memory. 0 means tightly packed
        // (width * bytesPerPixel). Set non-zero to point Source at a sub-rect
        // of a larger buffer (e.g. an atlas tile) without copying.
        int32_t stride = 0;

        // Optional chroma-key transparency. When hasChromaKey is true, blits
        // skip pixels whose RGB matches (keyR, keyG, keyB). For RGB565 sources
        // the key must be pre-quantized to 5/6/5 precision (low bits zeroed)
        // since the source pixels are extracted at that precision.
        bool    hasChromaKey = false;
        uint8_t keyR = 0;
        uint8_t keyG = 0;
        uint8_t keyB = 0;

        // Optional per-row non-key column spans. Same layout as alphaRowSpans:
        // packed int16 pairs [opaqueStart, opaqueEnd] per source row. When
        // present, blits in the 1:1 chroma path skip the per-pixel compare —
        // pixels in [start, end) are guaranteed non-key (direct write); pixels
        // outside the range are guaranteed key (skip without reading source).
        const int16_t* chromaRowSpans = nullptr;

        // Source pixels per world unit. The renderer divides the world→screen
        // scale by this value so a 32×32 sprite with pixelsPerMeter=16 covers
        // 2×2 world units instead of 32×32. Default 1 means "treat source
        // pixels as world units" (legacy pixel mode).
        float pixelsPerMeter = 1.0f;
    };

    /**
     * @brief Create source descriptor for a given texture format
     * @param ownsPixels If true, caller should delete[] pixels after use
     */
    Source MakeSource(const uint8_t* pixels, int32_t width, int32_t height,
                      int32_t bytesPerPixel, bool hasAlpha, bool isRGB565,
                      bool ownsPixels = true,
                      const int16_t* alphaRowSpans = nullptr);

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
     * @param rotation Rotation in DEGREES (from world_rotation). QuadBlit::Blit
     *                 converts to radians internally for trig math.
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

    // ============================================================================
    // Per-row kernel dispatch
    // ============================================================================
    //
    // Platform modules can register hand-tuned SIMD kernels for specific
    // format pairs. The blit dispatcher checks preconditions (format, no
    // rotation, no scale, alignment, no tint) up-front; when they hold AND
    // a kernel is registered, the SIMD kernel runs. Otherwise the existing
    // scalar inner loop runs. This is a predicate-gated dispatch, not a
    // runtime-failure fallback — the choice is deterministic.

    enum class KernelOp : uint8_t
    {
        // RGB565 source → RGB565 dest, 1:1, opaque copy. src/dst point at RGB565
        // pixels (2 bytes each). Used by both the plain opaque copy path and the
        // opaque-middle of the chroma-key row-span path.
        RGB565_Copy_Row,

        // RGB565A8 source → RGB565 dest, 1:1 alpha blend. src is 3 bytes/pixel
        // (RGB565 + 1 byte alpha), dst is 2 bytes/pixel. Kernel handles a==0 skip
        // and a==255 fast-path internally. Tint and chroma-key MUST be absent
        // (caller checks; kernel does not).
        RGB565A8_Blend_Row,

        Count
    };

    // Per-row kernel signature.
    //   src, dst       — 16-byte-aligned source/destination row pointers
    //   pixelCount     — number of pixels to process (NOT bytes)
    //   tintR/G/B/A    — passed through for kernels that opt to use them; the
    //                    no-tint kernel slots receive (255,255,255,255)
    //
    // Alignment, format, scale=1, rotation=0, and tint-identity (where
    // applicable) are guaranteed by the caller. The kernel is free to assume
    // them and use unrolled / SIMD loads.
    using RowKernelFn = void (*)(const uint8_t* src, uint8_t* dst, int32_t pixelCount,
                                 uint8_t tintR, uint8_t tintG, uint8_t tintB, uint8_t tintA);

    // Register a kernel for the given op. Pass nullptr to clear.
    // Typically called once at module init from a platform integration module.
    void RegisterKernel(KernelOp op, RowKernelFn fn);

    // Returns the registered kernel for the op, or nullptr if none.
    // Exposed primarily for tests; the dispatcher calls this internally.
    RowKernelFn GetKernel(KernelOp op);

}
