#include "Splash.h"
#include "RenderingFeatures.h"

#ifdef DEKI_FEATURE_SPLASH

#include "CameraComponent.h"
#include "RendererComponent.h"
#include "SplashImage.h"

#include <deki/BootSplash.h>
#include <deki/Engine.h>
#include <deki/Object.h>
#include <deki/PlatformSetupComponent.h>
#include <deki/Scene.h>
#include <deki/Time.h>
#include <deki/providers/IRenderSystem.h>
#include <deki/reflection/ComponentFactory.h>

#include <algorithm>

namespace DekiRendering
{

namespace
{

// How long the logo stays at least. The startup scene loads after that, with
// the logo still on screen, so a slow load shows it longer.
constexpr float kShowSeconds = 1.5f;

}  // namespace

/// Draws the logo from flash, centred, and opens the startup scene once it has
/// been on screen for kShowSeconds. Made only by MakeSplashScene, so the
/// reflection codegen never sees it: it is in no menu and in no scene file.
class SplashComponent : public RendererComponent
{
public:
    SplashComponent()
    {
        static const Deki::ComponentTypeInfo kInfo =
            Deki::MakeHandWrittenTypeInfo<SplashComponent, RendererComponent>(/*declaresUpdate=*/true);
        SetTypeInfo(&kInfo);
    }

    bool GetContentExtents(float& outWidth, float& outHeight) const override
    {
        outWidth = static_cast<float>(SplashImage::kWidth);
        outHeight = static_cast<float>(SplashImage::kHeight);
        return true;
    }

    bool RenderContent(const Deki::Object* owner, QuadBlit::Source& outSource, float& outPivotX, float& outPivotY,
                       uint8_t& outTintR, uint8_t& outTintG, uint8_t& outTintB, uint8_t& outTintA) override
    {
        (void)owner;
        outSource = QuadBlit::Source{};
        outSource.pixels = reinterpret_cast<const uint8_t*>(SplashImage::kPixels);
        outSource.width = SplashImage::kWidth;
        outSource.height = SplashImage::kHeight;
        outSource.bytesPerPixel = 2;
        outSource.isRGB565 = true;
        outPivotX = 0.5f;
        outPivotY = 0.5f;
        outTintR = outTintG = outTintB = outTintA = 255;
        return true;
    }

    bool GetContentVersion(uint32_t& outVersion) const override
    {
        outVersion = 1;
        return true;
    }

    void Update() override
    {
        if (m_Done)
        {
            return;
        }
        // The first update comes before the first frame is drawn: count from
        // the second, when the logo is on screen.
        if (++m_Frames > 1)
        {
            m_Shown += Deki::Time::GetDeltaTimeF();
        }
        if (m_Shown >= kShowSeconds)
        {
            m_Done = true;
            Deki::PlatformSetupComponent::LoadStartupScene();
        }
    }

private:
    int m_Frames = 0;
    float m_Shown = 0.0f;
    bool m_Done = false;
};

namespace
{

// A camera over black and the logo at its centre. The logo is drawn 1:1, at a
// whole multiple on a screen at least twice the 320 x 240 it was made for, and
// smaller only on a screen too small for it, with a margin round it there.
Deki::Scene* MakeSplashScene()
{
    const Deki::IRenderSystem* renderSystem = Deki::Engine::GetInstance().GetRenderSystem();
    const int width = renderSystem ? renderSystem->GetScreenWidth() : 0;
    const int height = renderSystem ? renderSystem->GetScreenHeight() : 0;
    if (width <= 0 || height <= 0)
    {
        return nullptr;
    }

    float scale = static_cast<float>(
        std::max(1, std::min(width / SplashImage::kDesignWidth, height / SplashImage::kDesignHeight)));
    const float fit =
        std::min(static_cast<float>(width) / SplashImage::kWidth, static_cast<float>(height) / SplashImage::kHeight);
    if (fit < 1.0f)
    {
        scale = fit * 0.9f;  // a margin all round
    }

    auto* scene = new Deki::Scene();
    Deki::Object* cameraObject = scene->CreateDekiObject("Camera");
    auto* camera = cameraObject->AddComponent<CameraComponent>();
    camera->clearColor = Deki::Color(0, 0, 0);
    camera->orthoHeight = static_cast<float>(height) / scale;

    Deki::Object* logo = scene->CreateDekiObject("Deki");
    logo->AddComponent<SplashComponent>();
    return scene;
}

}  // namespace

void RegisterSplash()
{
    Deki::BootSplash::SetMaker(&MakeSplashScene);
}

void UnregisterSplash()
{
    if (Deki::BootSplash::GetMaker() == &MakeSplashScene)
    {
        Deki::BootSplash::SetMaker(nullptr);
    }
}

}  // namespace DekiRendering

#else

namespace DekiRendering
{

void RegisterSplash()
{
}

void UnregisterSplash()
{
}

}  // namespace DekiRendering

#endif  // DEKI_FEATURE_SPLASH
