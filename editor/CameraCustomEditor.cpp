#ifdef DEKI_EDITOR

#include <deki-editor/EditorRegistry.h>
#include <deki-editor/CustomEditor.h>
#include <deki-editor/PrefabView.h>
#include "../CameraComponent.h"
#include "imgui.h"
#include <cmath>

namespace DekiEditor
{

class CameraCustomEditor : public CustomEditor
{
public:
    const char* GetComponentName() const override { return "CameraComponent"; }

    bool GetDisplaySize(DekiComponent* comp, float& outWidth, float& outHeight) override
    {
        // Match the SpriteComponent convention: return size in BUFFER pixels
        // (= source pixels at unit sprite scale). The camera's "size" in the
        // gizmo is the project render-target resolution; it's drawn at 1
        // screen pixel per buffer pixel * editor zoom by both
        // DrawComponentOutlines and OnDrawGizmos.
        int targetW = PrefabView::Get().GetTargetWidth();
        int targetH = PrefabView::Get().GetTargetHeight();
        if (targetW <= 0 || targetH <= 0)
            return false;

        outWidth = static_cast<float>(targetW);
        outHeight = static_cast<float>(targetH);
        return true;
    }

    void OnDrawGizmos(DekiComponent* comp) override
    {
        auto& view = PrefabView::Get();
        ImDrawList* dl = view.GetDrawList();
        if (!dl)
            return;

        float w = view.GetDisplayWidth();
        float h = view.GetDisplayHeight();
        if (w <= 0 || h <= 0)
            return;

        // DisplayWidth/Height are buffer pixels (target render size); each
        // buffer pixel is editor zoom screen pixels.
        const float zoom = view.GetZoom();
        float halfW = w * 0.5f * zoom;
        float halfH = h * 0.5f * zoom;
        float cx = view.GetScreenX();
        float cy = view.GetScreenY();

        ImU32 color = view.IsCurrentObjectSelected()
            ? IM_COL32(255, 165, 0, 255)
            : IM_COL32(255, 165, 0, 180);

        dl->AddRect(ImVec2(cx - halfW, cy - halfH), ImVec2(cx + halfW, cy + halfH),
                    color, 0.0f, 0, 1.0f);
    }
};

REGISTER_EDITOR(CameraCustomEditor)

} // namespace DekiEditor

#endif // DEKI_EDITOR
