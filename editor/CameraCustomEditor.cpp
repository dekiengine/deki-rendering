#ifdef DEKI_EDITOR

#include <deki-editor/EditorRegistry.h>
#include <deki-editor/CustomEditor.h>
#include <deki-editor/EditorUI.h>
#include <deki-editor/SceneView.h>
#include <deki/Engine.h>
#include <nlohmann/json.hpp>
#include "../CameraComponent.h"
#include <cmath>
#include <cstdint>
#include <cstdio>

// Editor extensions live in DekiEditor; the package's own types are in DekiRendering.
using namespace DekiRendering;

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
        auto* cam = static_cast<CameraComponent*>(comp);
        const bool ortho = cam->projection == Deki::ProjectionMode::Orthographic;

        ui.PropertyField("clearColor");
        ui.PropertyField("projection");
        if (ortho)
        {
            ui.PropertyField("orthoHeight");
            DrawScaleBadge(ui, *cam);
            ui.PropertyField("pixelPerfect");
        }
        else
        {
            ui.PropertyField("fieldOfView");
            ui.PropertyField("nearPlane");
            ui.PropertyField("farPlane");
        }
        ui.PropertyField("clearEveryFrame");
    }

    // "x3" at the end of the Ortho Height field: the scale the previewed screen
    // draws at. Shown in px, the height is in art pixels and could be mistaken
    // for a resolution; the scale beside it makes that clear.
    static void DrawScaleBadge(EditorUI& ui, const CameraComponent& cam)
    {
        const int pw = SceneView::Get().GetPreviewWidth(), ph = SceneView::Get().GetPreviewHeight();
        const float art = Deki::EngineSettings::Global().pixelsPerMeter;
        const float ppm = cam.GetPixelsPerMeter(pw, ph);
        if (ppm <= 0.0f || art <= 0.0f)
        {
            return;
        }
        char badge[16];
        std::snprintf(badge, sizeof(badge), "x%.3g", ppm / art);
        float x0, y0, x1, y1, tw, th;
        ui.GetItemRect(&x0, &y0, &x1, &y1);
        ui.MeasureTextCss(12.0f, badge, &tw, &th);
        ui.DrawTextCss(12.0f, x1 - tw - 8.0f, y0 + (y1 - y0 - th) * 0.5f, EditorUI::Rgba(150, 150, 150), badge);
    }

    bool GetDisplaySize(Deki::Component* comp, float& outWidth, float& outHeight) override
    {
        // The world the previewed screen sees through this camera, in buffer
        // pixels at the project's art density (Deki2D::SpriteComponent's
        // convention).
        auto* cam = static_cast<CameraComponent*>(comp);
        const float art = Deki::EngineSettings::Global().pixelsPerMeter;
        const int pw = SceneView::Get().GetPreviewWidth(), ph = SceneView::Get().GetPreviewHeight();
        const float ppm = cam->GetPixelsPerMeter(pw, ph);
        if (cam->projection != Deki::ProjectionMode::Orthographic || ppm <= 0.0f || art <= 0.0f)
        {
            return false;
        }
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
        {
            return;
        }

        // DisplayWidth/Height are in buffer pixels; each is `zoom` screen pixels.
        const float zoom = view.GetZoom();
        const float halfW = w * 0.5f * zoom;
        const float halfH = h * 0.5f * zoom;
        const float cx = view.GetScreenX();
        const float cy = view.GetScreenY();

        // Deki accent (#3ac3ff, oklch(0.78 0.16 240)), the same value as
        // EditorTheme's Palette::Accent. Spelled out because a package DLL does
        // not include the editor's ImGui theme header. Opaque when selected,
        // 70% otherwise.
        const uint32_t color =
            view.IsCurrentObjectSelected() ? SceneView::Rgba(58, 195, 255, 255) : SceneView::Rgba(58, 195, 255, 180);
        view.DrawRect(cx - halfW, cy - halfH, cx + halfW, cy + halfH, color, 1.0f);
    }
};

REGISTER_EDITOR(CameraCustomEditor)

}  // namespace DekiEditor

#endif  // DEKI_EDITOR
