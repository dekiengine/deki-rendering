#ifdef DEKI_EDITOR

// The Light component in the editor: its inspector, which says when the
// platform the editor targets cannot draw its shadows; its gizmos (a point's
// reach, a spot's cone); and the check a build runs over the scenes it ships,
// which lists each light whose shadows its target cannot draw.

#include <deki-editor/AssetPipeline.h>
#include <deki-editor/CustomEditor.h>
#include <deki-editor/EditorRegistry.h>
#include <deki-editor/EditorUI.h>
#include <deki-editor/SceneView.h>
#include <deki-editor/build/PlatformConfig.h>
#include <deki/Object.h>
#include <deki/Scene.h>

#include "../Light.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <string>

using namespace DekiRendering;

namespace DekiEditor
{
namespace
{

bool CastsShadows(const Light& light)
{
    return light.castShadows && (light.type == LightType::Directional || light.type == LightType::Spot);
}

/// Whether the platform the editor stores assets for has OpenGL, and its name.
bool TargetHasOpenGL(std::string& name)
{
    AssetPipeline* pipeline = AssetPipeline::Instance();
    const std::string id = pipeline ? pipeline->GetEditorTarget().platformId : std::string();
    if (id.empty())
    {
        return true;  // no platform chosen: the editor itself, which has
    }
    const PlatformConfig* platform = PlatformRegistry::Instance().GetPlatform(id);
    name = platform && !platform->displayName.empty() ? platform->displayName : id;
    return platform && platform->Capabilities().count("opengl") != 0;
}

void VisitLights(const Deki::Object* object, const std::function<void(const Deki::Object&, const Light&)>& visit)
{
    for (const Deki::Component* component : object->GetComponents())
    {
        if (component->GetType() == Deki::TypeId<Light>())
        {
            visit(*object, *static_cast<const Light*>(component));
        }
    }
    for (const Deki::Object* child : object->GetChildren())
    {
        VisitLights(child, visit);
    }
}

/// The build's look at a scene it ships: a light that casts shadows, for a
/// target without OpenGL, draws without them.
void CheckShadows(const Deki::Scene& scene, const std::string& scenePath, const std::set<std::string>& capabilities,
                  std::vector<std::string>& warnings)
{
    if (capabilities.count("opengl") != 0)
    {
        return;
    }
    for (const Deki::Object* root : scene.GetObjects())
    {
        VisitLights(root,
                    [&](const Deki::Object& object, const Light& light)
                    {
                        if (CastsShadows(light))
                        {
                            warnings.push_back(scenePath + ": the light '" + object.GetName() +
                                               "' casts shadows, and this board has no OpenGL to draw them; it "
                                               "lights without them");
                        }
                    });
    }
}

struct LightEditorRegistrar
{
    LightEditorRegistrar()
    {
        AssetPipeline::OnStarted([](AssetPipeline* pipeline)
                                 { pipeline->RegisterSceneExportCheck("deki-rendering: light shadows", CheckShadows); });
    }
};
LightEditorRegistrar s_LightEditorRegistrar;

}  // namespace

class LightCustomEditor : public CustomEditor
{
public:
    const char* GetComponentName() const override { return "Light"; }

    bool WantsInspectorOverride(Deki::Component* comp) override { return comp != nullptr; }

    void OnInspectorGUI(Deki::Component* comp) override
    {
        auto& ui = EditorUI::Get();
        auto* light = static_cast<Light*>(comp);
        ui.PropertyField("type");
        ui.PropertyField("color");
        ui.PropertyField("intensity");
        switch (light->type)
        {
            case LightType::Ambient:
                break;
            case LightType::Directional:
                ui.PropertyField("yaw");
                ui.PropertyField("pitch");
                break;
            case LightType::Point:
                ui.PropertyField("range");
                ui.PropertyField("bands");
                break;
            case LightType::Spot:
                ui.PropertyField("range");
                ui.PropertyField("spotAngle");
                ui.PropertyField("spotSoftness");
                ui.PropertyField("bands");
                break;
        }
        if (light->type == LightType::Directional || light->type == LightType::Spot)
        {
            ui.PropertyField("castShadows");
            if (light->castShadows)
            {
                ui.PropertyField("shadowResolution");
                ui.PropertyField("shadowBias");
                std::string platform;
                if (!TargetHasOpenGL(platform))
                {
                    ui.TextWrapped((platform + " has no OpenGL: it draws this light without its shadows.").c_str());
                }
            }
        }
        else if (light->type == LightType::Point)
        {
            ui.TextWrapped("Point lights cast no shadows yet.");
        }
    }

    void OnDrawGizmos(Deki::Component* comp) override
    {
        auto* light = static_cast<Light*>(comp);
        if (light->type != LightType::Point && light->type != LightType::Spot)
        {
            return;
        }
        auto& view = SceneView::Get();
        const float cx = view.GetScreenX();
        const float cy = view.GetScreenY();
        const float r = light->range * view.GetWorldToScreenScale();
        if (r <= 0.0f)
        {
            return;
        }
        // The light's own colour, faint unless selected.
        const uint8_t a = view.IsCurrentObjectSelected() ? 230 : 110;
        const uint32_t color = SceneView::Rgba(light->color.r, light->color.g, light->color.b, a);
        if (light->type == LightType::Point)
        {
            view.DrawCircle(cx, cy, r, color, 1.0f);
            return;
        }
        // A spot: its two edges and the arc between, along its facing (the
        // object's rotation; the screen's Y runs down).
        const Deki::Object* owner = light->GetOwner();
        const float facing = owner ? owner->GetWorldRotation() : 0.0f;
        const float half = light->spotAngle * 0.5f * 3.14159265f / 180.0f;
        auto at = [&](float angle, float& x, float& y)
        {
            x = cx + std::cos(angle) * r;
            y = cy - std::sin(angle) * r;
        };
        float x0, y0, x1, y1;
        at(facing - half, x0, y0);
        at(facing + half, x1, y1);
        view.DrawLine(cx, cy, x0, y0, color, 1.0f);
        view.DrawLine(cx, cy, x1, y1, color, 1.0f);
        const int steps = 16;
        float px = x0, py = y0;
        for (int i = 1; i <= steps; ++i)
        {
            float x, y;
            at(facing - half + 2.0f * half * static_cast<float>(i) / steps, x, y);
            view.DrawLine(px, py, x, y, color, 1.0f);
            px = x;
            py = y;
        }
    }
};

REGISTER_EDITOR(LightCustomEditor)

}  // namespace DekiEditor

#endif  // DEKI_EDITOR
