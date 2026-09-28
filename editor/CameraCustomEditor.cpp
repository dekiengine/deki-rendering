#ifdef DEKI_EDITOR

#include <deki-editor/EditorRegistry.h>
#include <deki-editor/CustomEditor.h>
#include <deki-editor/EditorUI.h>
#include <deki-editor/SceneView.h>
#include <deki/Engine.h>
#include <deki/SceneMigration.h>
#include <nlohmann/json.hpp>
#include "../CameraComponent.h"
#include <cmath>
#include <cstdint>
#include <cstdio>

// Editor extensions live in DekiEditor; the package's own types are in DekiRendering.
using namespace DekiRendering;

namespace
{
bool IsCameraType(const std::string& type)
{
    return type == "DekiRendering::CameraComponent" || type == "CameraComponent";
}

// Older cameras did not frame anything themselves. Before 0.18 a camera had
// its own pixels per meter (0 = the project's) and a pixel snap; in 0.18 it had
// a zoom over the project's design area, and Pixel Perfect was a project
// setting. A camera now shows orthoHeight meters top to bottom and carries its
// own Pixel Perfect, so:
//
//   pixelsPerMeter p  ->  zoom = p / projectPpm (0 -> 1)
//   zoom z            ->  orthoHeight = the old design area's height / z
//   pixelSnap, the project's Pixel Perfect  ->  pixelPerfect
//
// On a screen the shape of the old design area the picture is identical. The
// old design height and Pixel Perfect come from the project's framebuffer.json
// (SceneView::GetLegacyFraming), which the editor reads when the project opens.
void MigrateCameraFraming(nlohmann::json& components)
{
    const DekiEditor::SceneView::LegacyFraming& legacy = DekiEditor::SceneView::Get().GetLegacyFraming();
    for (auto& comp : components)
    {
        if (!comp.is_object() || !IsCameraType(comp.value("type", "")) || !comp.contains("properties"))
            continue;
        nlohmann::json& props = comp["properties"];
        if (!props.is_object())
            continue;

        bool pixelPerfect = legacy.pixelPerfect;
        if (props.contains("pixelSnap"))
        {
            pixelPerfect = pixelPerfect || (props["pixelSnap"].is_boolean() && props["pixelSnap"].get<bool>());
            props.erase("pixelSnap");
        }

        if (props.contains("pixelsPerMeter"))
        {
            if (!props.contains("zoom"))
            {
                const float p = props["pixelsPerMeter"].is_number() ? props["pixelsPerMeter"].get<float>() : 0.0f;
                const float project = Deki::EngineSettings::Global().pixelsPerMeter;
                props["zoom"] = (p > 0.0f && project > 0.0f) ? p / project : 1.0f;
            }
            props.erase("pixelsPerMeter");
        }

        // A camera saved with an ortho height is current; anything else is
        // older, including one saved at every default (no zoom key = zoom 1).
        if (props.contains("orthoHeight"))
            continue;
        float zoom = 1.0f;
        if (props.contains("zoom"))
        {
            if (props["zoom"].is_number() && props["zoom"].get<float>() > 0.0f)
                zoom = props["zoom"].get<float>();
            props.erase("zoom");
        }
        props["orthoHeight"] = legacy.designHeight / zoom;
        if (!props.contains("pixelPerfect"))
            props["pixelPerfect"] = pixelPerfect;
    }
}

Deki::ComponentsMigrationRegistrar s_CameraFramingMigration(&MigrateCameraFraming);
}  // namespace

namespace DekiEditor
{

class CameraCustomEditor : public CustomEditor
{
public:
    const char* GetComponentName() const override { return "CameraComponent"; }

    bool WantsInspectorOverride(Deki::Component* comp) override { return comp != nullptr; }

    void OnInspectorGUI(Deki::Component* comp) override
    {
        auto& ui = EditorUI::Get();
        ui.DrawDefaultInspector();

        auto* cam = static_cast<CameraComponent*>(comp);
        if (cam->projection != Deki::ProjectionMode::Orthographic)
            return;

        // What this camera shows on the screen the Play view previews, and how
        // big it is drawn there. The px the inspector shows for Ortho Height
        // are art pixels (meters at the project's pixels per meter), not screen
        // pixels: every screen scales them to fill its height, and this says by
        // how much, so 240 px on a 720 px screen does not read as a crop.
        const int pw = SceneView::Get().GetPreviewWidth(), ph = SceneView::Get().GetPreviewHeight();
        const float ppm = cam->GetPixelsPerMeter(pw, ph);
        const float art = Deki::EngineSettings::Global().pixelsPerMeter;
        if (ppm <= 0.0f || art <= 0.0f)
            return;
        const float w = pw / ppm, h = ph / ppm;
        char line[200];
        std::snprintf(line, sizeof(line), "Shows %.4g x %.4g m (%d x %d art px), drawn %.3gx on the previewed %d x %d screen",
                      w, h, (int)std::lround(w * art), (int)std::lround(h * art), ppm / art, pw, ph);
        ui.Spacing();
        ui.TextDisabled(line);
    }

    bool GetDisplaySize(Deki::Component* comp, float& outWidth, float& outHeight) override
    {
        // In buffer pixels at the project's art density, the
        // Deki2D::SpriteComponent convention: the world the previewed screen
        // sees through this camera.
        auto* cam = static_cast<CameraComponent*>(comp);
        const float art = Deki::EngineSettings::Global().pixelsPerMeter;
        const int pw = SceneView::Get().GetPreviewWidth(), ph = SceneView::Get().GetPreviewHeight();
        const float ppm = cam->GetPixelsPerMeter(pw, ph);
        if (cam->projection != Deki::ProjectionMode::Orthographic || ppm <= 0.0f || art <= 0.0f)
            return false;
        outWidth = pw / ppm * art;
        outHeight = ph / ppm * art;
        return outWidth > 0.0f && outHeight > 0.0f;
    }

    void OnDrawGizmos(Deki::Component* comp) override
    {
        (void)comp;
        auto& view = SceneView::Get();

        const float w = view.GetDisplayWidth();
        const float h = view.GetDisplayHeight();
        if (w <= 0 || h <= 0)
            return;

        // DisplayWidth/Height are buffer pixels; each is editor zoom screen pixels.
        const float zoom = view.GetZoom();
        const float halfW = w * 0.5f * zoom;
        const float halfH = h * 0.5f * zoom;
        const float cx = view.GetScreenX();
        const float cy = view.GetScreenY();

        // Deki accent (#3ac3ff, oklch(0.78 0.16 240)) - the same value as
        // EditorTheme's Palette::Accent, spelled out here because a package DLL
        // does not pull in the editor's ImGui theme header. Selected draws it
        // opaque, unselected at 70%.
        const uint32_t color = view.IsCurrentObjectSelected()
            ? SceneView::Rgba(58, 195, 255, 255)
            : SceneView::Rgba(58, 195, 255, 180);
        view.DrawRect(cx - halfW, cy - halfH, cx + halfW, cy + halfH, color, 1.0f);
    }
};

REGISTER_EDITOR(CameraCustomEditor)

} // namespace DekiEditor

#endif // DEKI_EDITOR
