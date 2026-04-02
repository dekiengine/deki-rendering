#pragma once

#include <cstdint>
#include "DekiEngine.h"

// Forward declarations
class Prefab;
class CameraComponent;

/**
 * @brief Context passed through the render pipeline
 *
 * Contains everything a renderer or render pass needs to produce output.
 */
struct RenderContext
{
    CameraComponent* camera;
    uint8_t* buffer;
    int32_t width;
    int32_t height;
    DekiColorFormat format;
};

/**
 * @brief Abstract base class for all renderers
 *
 * Subclass this to create a custom rendering strategy.
 * The engine provides Standard2DRenderer as the default implementation.
 *
 * A custom renderer has full control over how rendering happens —
 * it can use RenderPass objects, compose other renderers, or
 * implement a completely custom approach.
 *
 * Usage:
 * @code
 * class MyRenderer : public DekiRenderer {
 *     void Render(Prefab* prefab, const RenderContext& ctx) override {
 *         // Custom rendering logic
 *     }
 * };
 *
 * renderSystem.SetRenderer(&myRenderer);
 * @endcode
 */
class DekiRenderer
{
public:
    virtual ~DekiRenderer() = default;

    /**
     * @brief Get the renderer type ID (for safe downcasting without RTTI)
     * Each renderer subclass defines a unique static constexpr uint32_t RendererTypeID.
     */
    virtual uint32_t GetRendererType() const = 0;

    /**
     * @brief Render a prefab to a buffer
     * @param prefab The prefab to render
     * @param ctx Render context with camera, buffer, and format info
     */
    virtual void Render(Prefab* prefab, const RenderContext& ctx) = 0;
};
