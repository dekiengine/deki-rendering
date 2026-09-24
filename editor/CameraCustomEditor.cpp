#ifdef DEKI_EDITOR

#include <deki-editor/EditorRegistry.h>
#include <deki-editor/CustomEditor.h>
#include <deki-editor/EditorUI.h>
#include <deki-editor/SceneView.h>
#include <deki/Engine.h>
#include <deki/SceneMigration.h>
#include <deki/ScreenScale.h>
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

// Before 0.18 the camera's zoom was its own pixels per meter, 0 meaning the
// project's. It is now a zoom over the project's design area, so p becomes
// p / projectPpm (0 becomes 1): on the design screen the picture is identical.
// The camera's pixel snap became the project's Pixel Perfect and is dropped.
void MigrateCameraZoom(nlohmann::json& components)
{
    for (auto& comp : components)
    {
        if (!comp.is_object() || !IsCameraType(comp.value("type", "")) || !comp.contains("properties"))
            continue;
        nlohmann::json& props = comp["properties"];
        if (!props.is_object())
            continue;
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
        props.erase("pixelSnap");
    }
}

Deki::ComponentsMigrationRegistrar s_CameraZoomMigration(&MigrateCameraZoom);

void DrawDashedRect(DekiEditor::SceneView& view, float x0, float y0, float x1, float y1, uint32_t color)
{
    constexpr float kDash = 6.0f;
    auto dashed = [&](float ax, float ay, float bx, float by)
    {
        const float len = std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
        if (len <= 0.0f)
            return;
        const float dx = (bx - ax) / len, dy = (by - ay) / len;
        for (float t = 0.0f; t < len; t += 2.0f * kDash)
        {
            const float e = std::fmin(t + kDash, len);
            view.DrawLine(ax + dx * t, ay + dy * t, ax + dx * e, ay + dy * e, color, 1.0f);
        }
    };
    dashed(x0, y0, x1, y0);
    dashed(x1, y0, x1, y1);
    dashed(x1, y1, x0, y1);
    dashed(x0, y1, x0, y0);
}
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
        if (cam->projection != Deki::ProjectionMode::Orthographic || cam->zoom <= 0.0f)
            return;

        // What this camera shows, in both units, so the numbers in Project
        // Settings and the picture in the Play view can be matched up.
        const Deki::EngineSettings& s = Deki::EngineSettings::Global();
        char line[160];
        const float w = s.designWidth / cam->zoom, h = s.designHeight / cam->zoom;
        std::snprintf(line, sizeof(line), "Shows %g x %g m (%d x %d px) on the design screen", w, h,
                      (int)std::lround(w * s.pixelsPerMeter), (int)std::lround(h * s.pixelsPerMeter));
        ui.Spacing();
        ui.TextDisabled(line);

        const int pw = SceneView::Get().GetPreviewWidth(), ph = SceneView::Get().GetPreviewHeight();
        const float ppm = cam->GetPixelsPerMeter(pw, ph);
        if (ppm > 0.0f)
        {
            std::snprintf(line, sizeof(line), "Shows %.3g x %.3g m on the previewed %d x %d screen",
                          pw / ppm, ph / ppm, pw, ph);
            ui.TextDisabled(line);
        }
    }

    bool GetDisplaySize(Deki::Component* comp, float& outWidth, float& outHeight) override
    {
        // In buffer pixels, the Deki2D::SpriteComponent convention: the design
        // area over the zoom, at the project's art density.
        auto* cam = static_cast<CameraComponent*>(comp);
        const Deki::EngineSettings& s = Deki::EngineSettings::Global();
        if (cam->zoom <= 0.0f || s.pixelsPerMeter <= 0.0f)
            return false;
        outWidth = s.designWidth / cam->zoom * s.pixelsPerMeter;
        outHeight = s.designHeight / cam->zoom * s.pixelsPerMeter;
        return outWidth > 0.0f && outHeight > 0.0f;
    }

    void OnDrawGizmos(Deki::Component* comp) override
    {
        auto& view = SceneView::Get();
        auto* cam = static_cast<CameraComponent*>(comp);

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

        // Deki accent (#3ac3ff, oklch(0.78 0.16 240)) — the same value as
        // EditorTheme's Palette::Accent, spelled out here because a package DLL
        // does not pull in the editor's ImGui theme header. Selected draws it
        // opaque, unselected at 70%.
        const uint32_t color = view.IsCurrentObjectSelected()
            ? SceneView::Rgba(58, 195, 255, 255)
            : SceneView::Rgba(58, 195, 255, 180);
        view.DrawRect(cx - halfW, cy - halfH, cx + halfW, cy + halfH, color, 1.0f);

        // What the previewed screen sees, dashed, where it differs from the
        // design area: the extra world under Show All, the crop under Fill.
        const int pw = view.GetPreviewWidth(), ph = view.GetPreviewHeight();
        const float ppm = cam->GetPixelsPerMeter(pw, ph);
        if (cam->projection != Deki::ProjectionMode::Orthographic || ppm <= 0.0f)
            return;
        const float scale = view.GetWorldToScreenScale();
        const float vHalfW = pw / ppm * 0.5f * scale;
        const float vHalfH = ph / ppm * 0.5f * scale;
        if (std::fabs(vHalfW - halfW) < 0.5f && std::fabs(vHalfH - halfH) < 0.5f)
            return;
        DrawDashedRect(view, cx - vHalfW, cy - vHalfH, cx + vHalfW, cy + vHalfH, color);
    }
};

REGISTER_EDITOR(CameraCustomEditor)

} // namespace DekiEditor

#endif // DEKI_EDITOR
