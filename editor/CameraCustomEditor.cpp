#ifdef DEKI_EDITOR

#include <deki-editor/EditorRegistry.h>
#include <deki-editor/CustomEditor.h>
#include <deki-editor/PrefabView.h>
#include "../CameraComponent.h"
#include <cmath>
#include <cstdint>

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

        uint32_t color = view.IsCurrentObjectSelected()
            ? PrefabView::Rgba(255, 165, 0, 255)
            : PrefabView::Rgba(255, 165, 0, 180);

        view.DrawRect(cx - halfW, cy - halfH, cx + halfW, cy + halfH, color, 1.0f);
    }
};

REGISTER_EDITOR(CameraCustomEditor)

} // namespace DekiEditor

#endif // DEKI_EDITOR
