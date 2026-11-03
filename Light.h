#pragma once

#include <cstdint>
#include <vector>

#include <deki/Color.h>
#include <deki/Component.h>
#include <deki/reflection/Property.h>

#include "DekiRenderingPackage.h"

namespace Deki
{
class Scene;
}

namespace DekiRendering
{

/// What a Light lights, and from where.
///
/// Ambient      everywhere the same, from no direction.
/// Directional  from far away, all from one direction: the sun, the moon.
/// Point        round its object, out to its range.
/// Spot         from its object, in a cone along its facing, out to its range.
enum class LightType : uint8_t
{
    Ambient = 0,
    Directional = 1,
    Point = 2,
    Spot = 3,
};

/// A light. 3D meshes that are lit (MeshComponent's shading) and the 2D world
/// are lit by every active Light in the scene, on the CPU and on the GPU.
///
/// A pixel is its colour times the light reaching it: the ambient lights,
/// plus each other light by how near it is and, on a mesh, how much the
/// surface faces it. Light adds up to twice a colour, so a lamp can brighten
/// as well as reveal. A scene with no Light is not lit at all: everything
/// draws in its own colours, as without this component.
///
/// In 2D a light sits at its object's position and a spot points along its
/// rotation; a directional light lights 2D evenly, as ambient does. What the
/// camera draws above its litUpToSortOrder (the UI) is not lit.
///
/// Shadows: directional and spot lights cast them where the display draws
/// through OpenGL, at most two lights a frame. A board without OpenGL draws
/// the light without its shadows, and its build says so.
DEKI_CATEGORY("Rendering")
DEKI_DESCRIPTION("A light: ambient, the sun, round a point or in a cone. It lights meshes and the 2D world.")
class DEKI_RENDERING_API Light : public Deki::Component
{
public:
    Light();
    ~Light() override;

    DEKI_TOOLTIP("Ambient lights everything the same. Directional lights from one far direction, like the sun. Point "
                 "lights round the object, out to the range. Spot lights a cone along the object's facing.")
    DEKI_EXPORT
    LightType type = LightType::Point;

    DEKI_TOOLTIP("The light's colour.")
    DEKI_EXPORT
    Deki::Color color = Deki::Color(255, 255, 255);

    DEKI_TOOLTIP("How strong the light is: 1 lights a colour as it is where the light is strongest, 0.5 half as "
                 "much. Lights add up, to at most twice a colour.")
    DEKI_RANGE(0, 8)
    DEKI_EXPORT
    float intensity = 1.0f;

    DEKI_TOOLTIP("How far the light reaches, in meters. It fades to nothing there.")
    DEKI_UNIT(Distance)
    DEKI_RANGE(0.01f, 100000.0f)
    DEKI_VISIBLE_WHEN(type, Point, Spot)
    DEKI_EXPORT
    float range = 3.0f;

    DEKI_TOOLTIP("The cone's full width, in degrees.")
    DEKI_RANGE(1, 179)
    DEKI_VISIBLE_WHEN(type, Spot)
    DEKI_EXPORT
    float spotAngle = 45.0f;

    DEKI_TOOLTIP("How much of the cone fades out toward its edge: 0 is a hard edge, 1 fades from the centre.")
    DEKI_SLIDER(0, 1)
    DEKI_VISIBLE_WHEN(type, Spot)
    DEKI_EXPORT
    float spotSoftness = 0.25f;

    DEKI_TOOLTIP("Where the light comes from, swept around the vertical axis, in degrees. 0 puts it behind the "
                 "camera, 90 to the right.")
    DEKI_RANGE(-180, 180)
    DEKI_VISIBLE_WHEN(type, Directional)
    DEKI_EXPORT
    float yaw = -25.0f;

    DEKI_TOOLTIP("How high the light is, in degrees: 90 straight down, 0 level with the horizon.")
    DEKI_RANGE(-90, 90)
    DEKI_VISIBLE_WHEN(type, Directional)
    DEKI_EXPORT
    float pitch = 55.0f;

    DEKI_TOOLTIP("2D only: 0 fades smoothly. 2 to 8 lights in that many hard rings, the brightest in the middle, "
                 "for pixel art.")
    DEKI_RANGE(0, 8)
    DEKI_VISIBLE_WHEN(type, Point, Spot)
    DEKI_EXPORT
    int bands = 0;

    DEKI_TOOLTIP("Meshes in its light cast shadows, where the display draws through OpenGL; at most two lights in a "
                 "frame cast them. A board without OpenGL draws the light without its shadows, and its build says "
                 "so. Point lights cast none yet.")
    DEKI_VISIBLE_WHEN(type, Directional, Spot)
    DEKI_EXPORT
    bool castShadows = false;

    DEKI_TOOLTIP("Pixels on each side of the shadow map. More is sharper and costs GPU memory and time.")
    DEKI_RANGE(128, 4096)
    DEKI_VISIBLE_WHEN(type, Directional, Spot)
    DEKI_EXPORT
    int shadowResolution = 1024;

    DEKI_TOOLTIP("Pushes shadows back from the surfaces that cast them. Raise it when a lit surface shows stripes "
                 "of shadow on itself; too much lets shadows come loose from their casters.")
    DEKI_RANGE(0, 0.1f)
    DEKI_VISIBLE_WHEN(type, Directional, Spot)
    DEKI_EXPORT
    float shadowBias = 0.002f;

    /// Whether the light reaches anything: its object is active, it has some
    /// intensity and colour, and a point or spot has a range.
    bool Shines() const;

    /// Every Light whose object is active and in `scene` (or persistent), in
    /// the order they were made. Lighting is on while there is one, even one
    /// that does not shine: a light dimmed to nothing leaves the dark.
    static void InScene(const Deki::Scene* scene, std::vector<const Light*>& out);

    /// Every Light alive, shining or not, in the order they were made.
    static const std::vector<Light*>& All();

    /// A number that changes whenever a Light is made or destroyed.
    static uint32_t ListVersion();
};

}  // namespace DekiRendering
