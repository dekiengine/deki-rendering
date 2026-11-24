#include "Splash.h"
#include "RenderingFeatures.h"

#ifdef DEKI_FEATURE_SPLASH

#include "CameraComponent.h"
#include "PixelFormat.h"
#include "RendererComponent.h"
#include "SplashShapes.h"

#include <deki/BootSplash.h>
#include <deki/Engine.h>
#include <deki/Object.h>
#include <deki/PlatformSetupComponent.h>
#include <deki/Scene.h>
#include <deki/Time.h>
#include <deki/providers/Buffer.h>
#include <deki/providers/IRenderSystem.h>
#include <deki/providers/Memory.h>
#include <deki/reflection/ComponentFactory.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>

namespace DekiRendering
{

namespace
{

// How long the logo stays at least. The startup scene loads after that, with
// the logo still on screen, so a slow load shows it longer.
constexpr float kShowSeconds = 1.5f;

// ---------------------------------------------------------------------------
// The icon, drawn from the brand's shapes. They are in brand units: the icon's
// box is 94 units square from (6, 4). At 1x a pixel is 1.25 units, and the
// drawing works in 32nds of a unit so that it is all whole numbers, as fast on
// a board without floating point.
// ---------------------------------------------------------------------------

constexpr int kSub = 32;                   // 32nds of a unit
constexpr int kPixelSub = 40;              // a pixel at 1x: 1.25 units
constexpr int kSampleSub = kPixelSub / 4;  // 4 x 4 samples a pixel
constexpr int kIconSize = 76;              // the icon's box, pixels at 1x
constexpr int kIconLeft = 11;              // where it sits in the logo's box
constexpr int kBoxLeft = 6 * kSub;         // the icon box's corner
constexpr int kBoxTop = 4 * kSub;

struct Point
{
    int x;
    int y;
};

// The block with its two ears, in units, and each edge's length (whole: the
// ears are 3-4-5 triangles).
constexpr Point kBody[] = { { 14, 10 }, { 38, 28 }, { 62, 28 }, { 86, 10 }, { 86, 88 }, { 14, 88 } };
constexpr int kBodyLength[] = { 30, 24, 30, 78, 72, 78 };
constexpr int kBodyCount = 6;
constexpr int kRound = 3 * kSub;  // the outline's rounded edge
constexpr int kDepth = 6;         // the solid side: the block swept 6 units down-right

// The screen, and the eyes in it.
constexpr int kScreenLeft = 22 * kSub;
constexpr int kScreenTop = 40 * kSub;
constexpr int kScreenRight = 78 * kSub;
constexpr int kScreenBottom = 76 * kSub;
constexpr int kScreenRound = 4 * kSub;
constexpr int kEyeTop = 48 * kSub;
constexpr int kEyeWidth = 8 * kSub;
constexpr int kEyeHeight = 16 * kSub;  // open
constexpr int kLeftEye = 34 * kSub;
constexpr int kRightEye = 58 * kSub;

// Eyes squeezed shut, > <: two strokes each, as segments in 32nds.
struct Segment
{
    Point a;
    Point b;
};
constexpr Segment kSqueeze[] = { { { 1088, 1616 }, { 1312, 1792 } },
                                 { { 1312, 1792 }, { 1088, 1968 } },
                                 { { 2112, 1616 }, { 1888, 1792 } },
                                 { { 1888, 1792 }, { 2112, 1968 } } };
constexpr int kSqueezeLength = 285;  // each stroke's length
constexpr int kSqueezeRound = 54;    // half a stroke's width

// The pixels the eyes can touch, so changing them redraws only these.
constexpr int kEyesLeft = 21;
constexpr int kEyesTop = 34;
constexpr int kEyesRight = 50;
constexpr int kEyesBottom = 50;

struct Colour
{
    int r;
    int g;
    int b;
};
constexpr Colour kLight = { 59, 195, 255 };
constexpr Colour kDark = { 43, 133, 196 };
constexpr Colour kScreen = { 11, 13, 16 };
constexpr Colour kWhite = { 232, 230, 225 };

// Whether (u, v) is within `radius` of the segment from a to b, `length` long.
bool NearSegment(int u, int v, Point a, Point b, int length, int radius)
{
    const int dx = b.x - a.x;
    const int dy = b.y - a.y;
    const int px = u - a.x;
    const int py = v - a.y;
    const int along = px * dx + py * dy;
    if (along <= 0)
    {
        return px * px + py * py <= radius * radius;
    }
    if (along >= length * length)
    {
        const int qx = u - b.x;
        const int qy = v - b.y;
        return qx * qx + qy * qy <= radius * radius;
    }
    return std::abs(dx * py - dy * px) <= radius * length;
}

// Whether (u, v), in 32nds, is on the block: inside its outline or on the
// outline's rounded edge.
bool OnBody(int u, int v)
{
    if (u < (14 - 3) * kSub || u > (86 + 3) * kSub || v < (10 - 3) * kSub || v > (88 + 3) * kSub)
    {
        return false;
    }
    bool inside = false;
    for (int i = 0; i < kBodyCount; ++i)
    {
        const Point a = { kBody[i].x * kSub, kBody[i].y * kSub };
        const Point b = { kBody[(i + 1) % kBodyCount].x * kSub, kBody[(i + 1) % kBodyCount].y * kSub };
        if ((a.y > v) != (b.y > v))
        {
            const int left = (u - a.x) * (b.y - a.y);
            const int edge = (b.x - a.x) * (v - a.y);
            if (b.y > a.y ? left < edge : left > edge)
            {
                inside = !inside;
            }
        }
    }
    if (inside)
    {
        return true;
    }
    for (int i = 0; i < kBodyCount; ++i)
    {
        const Point a = { kBody[i].x * kSub, kBody[i].y * kSub };
        const Point b = { kBody[(i + 1) % kBodyCount].x * kSub, kBody[(i + 1) % kBodyCount].y * kSub };
        if (NearSegment(u, v, a, b, kBodyLength[i] * kSub, kRound))
        {
            return true;
        }
    }
    return false;
}

bool OnScreen(int u, int v)
{
    if (u < kScreenLeft || u > kScreenRight || v < kScreenTop || v > kScreenBottom)
    {
        return false;
    }
    const int cx = std::clamp(u, kScreenLeft + kScreenRound, kScreenRight - kScreenRound);
    const int cy = std::clamp(v, kScreenTop + kScreenRound, kScreenBottom - kScreenRound);
    return (u - cx) * (u - cx) + (v - cy) * (v - cy) <= kScreenRound * kScreenRound;
}

// `open` is how tall the eyes are, in 32nds; 0 squeezes them shut, > <.
bool OnEye(int u, int v, int open)
{
    if (open == 0)
    {
        for (const Segment& stroke : kSqueeze)
        {
            if (NearSegment(u, v, stroke.a, stroke.b, kSqueezeLength, kSqueezeRound))
            {
                return true;
            }
        }
        return false;
    }
    const int top = kEyeTop + (kEyeHeight - open) / 2;
    if (v < top || v > top + open)
    {
        return false;
    }
    return (u >= kLeftEye && u <= kLeftEye + kEyeWidth) || (u >= kRightEye && u <= kRightEye + kEyeWidth);
}

// The colour at (u, v), or nullptr where the icon is not.
const Colour* IconColour(int u, int v, int open)
{
    if (OnBody(u, v))
    {
        if (OnScreen(u, v))
        {
            return OnEye(u, v, open) ? &kLight : &kScreen;
        }
        return &kLight;
    }
    for (int k = 1; k <= kDepth; ++k)
    {
        if (OnBody(u - k * kSub, v - k * kSub))
        {
            return &kDark;
        }
    }
    return nullptr;
}

// Draws the icon's pixels from (left, top) to (right, bottom), not including
// those, into `pixels`, RGB565 kIconSize wide: each pixel the average of 4 x 4
// samples, so the edges are smooth on the black behind it.
void DrawIcon(uint8_t* pixels, int open, int left, int top, int right, int bottom)
{
    for (int y = top; y < bottom; ++y)
    {
        for (int x = left; x < right; ++x)
        {
            int r = 0;
            int g = 0;
            int b = 0;
            for (int sy = 0; sy < 4; ++sy)
            {
                const int v = kBoxTop + y * kPixelSub + sy * kSampleSub + kSampleSub / 2;
                for (int sx = 0; sx < 4; ++sx)
                {
                    const int u = kBoxLeft + x * kPixelSub + sx * kSampleSub + kSampleSub / 2;
                    if (const Colour* c = IconColour(u, v, open))
                    {
                        r += c->r;
                        g += c->g;
                        b += c->b;
                    }
                }
            }
            const uint16_t colour = DekiPixel::PackRGB565(static_cast<uint8_t>(r / 16), static_cast<uint8_t>(g / 16),
                                                          static_cast<uint8_t>(b / 16));
            DekiPixel::Store565(pixels + (y * kIconSize + x) * 2, colour);
        }
    }
}

// ---------------------------------------------------------------------------
// The motion. Times in seconds from when the logo is first on screen; lengths
// in pixels at 1x. The icon falls as a body does, faster and faster, squashes
// as it lands and hops once; its eyes are squeezed shut, > <, until it has
// landed, then open, and blink once. The landing jolts DEKI: E and K jump
// first, then D and I.
// ---------------------------------------------------------------------------

constexpr float kLand = 0.42f;
constexpr float kPi = 3.14159265f;

struct Pose
{
    float lift;  // above where it rests
    float scaleX;
    float scaleY;
};

Pose IconPose(float t, float fall)
{
    if (t < kLand)
    {
        const float k = t / kLand;
        return { (1.0f - k * k) * fall, 1.0f - 0.07f * k, 1.0f + 0.12f * k };  // speeds up, stretches
    }
    const float u = t - kLand;
    if (u < 0.07f)  // squashes
    {
        const float s = std::sin(u / 0.07f * kPi / 2.0f);
        return { 0.0f, 1.0f + 0.2f * s, 1.0f - 0.25f * s };
    }
    if (u < 0.14f)  // springs back
    {
        const float k = (u - 0.07f) / 0.07f;
        return { 0.0f, 1.2f - 0.2f * k, 0.75f + 0.3f * k };
    }
    if (u < 0.34f)  // a small hop
    {
        const float k = (u - 0.14f) / 0.2f;
        return { 10.0f * 4.0f * k * (1.0f - k), 1.0f, 1.05f - 0.05f * k };
    }
    if (u < 0.4f)  // and lands again
    {
        const float s = std::sin((u - 0.34f) / 0.06f * kPi);
        return { 0.0f, 1.0f + 0.05f * s, 1.0f - 0.06f * s };
    }
    return { 0.0f, 1.0f, 1.0f };
}

// How open the eyes are at `t`, in 32nds of a unit; 0 is squeezed shut.
int EyesAt(float t)
{
    const float opened = kLand + 0.1f;
    if (t < opened)
    {
        return 0;
    }
    float units = 16.0f;
    if (t < opened + 0.07f)
    {
        units = 2.0f + 14.0f * (t - opened) / 0.07f;
    }
    else if (t > 1.05f && t < 1.2f)  // a blink
    {
        units = std::max(2.0f, 16.0f * std::fabs(t - 1.125f) / 0.075f);
    }
    return static_cast<int>(units) * kSub;
}

// Whether letter `index` (D, E, K, I) shows at `t`, and how far it has jumped.
bool LetterLift(int index, float t, float& outLift)
{
    const float start = kLand + ((index == 1 || index == 2) ? 0.04f : 0.10f);
    outLift = 0.0f;
    if (t < start)
    {
        return false;
    }
    const float k = (t - start) / 0.22f;
    if (k < 1.0f)
    {
        outLift = 14.0f * 4.0f * k * (1.0f - k);
    }
    return true;
}

}  // namespace

/// The icon, drawn into a picture of its own from the brand's shapes, again
/// only where the eyes are when they change.
class SplashIconComponent : public RendererComponent
{
public:
    SplashIconComponent()
    {
        static const Deki::ComponentTypeInfo kInfo =
            Deki::MakeHandWrittenTypeInfo<SplashIconComponent, RendererComponent>(/*declaresUpdate=*/false);
        SetTypeInfo(&kInfo);
        if (m_Pixels.Allocate(static_cast<size_t>(kIconSize) * kIconSize * 2, Deki::Memory::External))
        {
            DrawIcon(m_Pixels.Data(), m_Open, 0, 0, kIconSize, kIconSize);
        }
    }

    void SetEyes(int open)
    {
        if (open == m_Open || !m_Pixels)
        {
            return;
        }
        m_Open = open;
        DrawIcon(m_Pixels.Data(), m_Open, kEyesLeft, kEyesTop, kEyesRight, kEyesBottom);
        ++m_Version;
    }

    bool GetContentExtents(float& outWidth, float& outHeight) const override
    {
        outWidth = static_cast<float>(kIconSize);
        outHeight = static_cast<float>(kIconSize);
        return true;
    }

    bool RenderContent(const Deki::Object* owner, QuadBlit::Source& outSource, float& outPivotX, float& outPivotY,
                       uint8_t& outTintR, uint8_t& outTintG, uint8_t& outTintB, uint8_t& outTintA) override
    {
        (void)owner;
        if (!m_Pixels)
        {
            return false;
        }
        outSource = QuadBlit::Source{};
        outSource.pixels = m_Pixels.Data();
        outSource.width = kIconSize;
        outSource.height = kIconSize;
        outSource.bytesPerPixel = 2;
        outSource.isRGB565 = true;
        // Its feet: it squashes and stretches from where it stands.
        outPivotX = 0.5f;
        outPivotY = 1.0f;
        outTintR = outTintG = outTintB = outTintA = 255;
        return true;
    }

    bool GetContentVersion(uint32_t& outVersion) const override
    {
        outVersion = m_Version;
        return true;
    }

private:
    Deki::Buffer<uint8_t> m_Pixels;
    int m_Open = 0;
    uint32_t m_Version = 1;
};

/// One of the letters D, E, K and I: its face from SplashShapes and its blue
/// side, the face swept 3 pixels down-right, in a picture with alpha so the
/// leaning letters can overlap.
class SplashLetterComponent : public RendererComponent
{
public:
    static constexpr int kSide = 3;

    explicit SplashLetterComponent(int index)
        : m_Letter(SplashShapes::kLetters[index])
    {
        static const Deki::ComponentTypeInfo kInfo =
            Deki::MakeHandWrittenTypeInfo<SplashLetterComponent, RendererComponent>(/*declaresUpdate=*/false);
        SetTypeInfo(&kInfo);
        const int width = m_Letter.width + kSide;
        const int height = m_Letter.height + kSide;
        if (!m_Pixels.Allocate(static_cast<size_t>(width) * height * 3, Deki::Memory::External))
        {
            return;
        }
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const int face = Face(x, y);
                int side = 0;
                for (int k = 1; k <= kSide; ++k)
                {
                    side = std::max(side, Face(x - k, y - k));
                }
                // Out of 9: the face covers face/3 of the pixel, the side
                // side/3 of what the face leaves.
                const int faceWeight = face * 3;
                const int sideWeight = side * (3 - face);
                const int weight = faceWeight + sideWeight;
                uint8_t* pixel = m_Pixels.Data() + (y * width + x) * 3;
                if (weight == 0)
                {
                    DekiPixel::Store565(pixel, 0);
                    pixel[2] = 0;
                    continue;
                }
                const auto mix = [&](int white, int dark)
                { return static_cast<uint8_t>((white * faceWeight + dark * sideWeight) / weight); };
                DekiPixel::Store565(pixel, DekiPixel::PackRGB565(mix(kWhite.r, kDark.r), mix(kWhite.g, kDark.g),
                                                                 mix(kWhite.b, kDark.b)));
                pixel[2] = static_cast<uint8_t>(weight * 255 / 9);
            }
        }
    }

    bool GetContentExtents(float& outWidth, float& outHeight) const override
    {
        outWidth = static_cast<float>(m_Letter.width + kSide);
        outHeight = static_cast<float>(m_Letter.height + kSide);
        return true;
    }

    bool RenderContent(const Deki::Object* owner, QuadBlit::Source& outSource, float& outPivotX, float& outPivotY,
                       uint8_t& outTintR, uint8_t& outTintG, uint8_t& outTintB, uint8_t& outTintA) override
    {
        (void)owner;
        if (!m_Pixels)
        {
            return false;
        }
        outSource = QuadBlit::Source{};
        outSource.pixels = m_Pixels.Data();
        outSource.width = m_Letter.width + kSide;
        outSource.height = m_Letter.height + kSide;
        outSource.bytesPerPixel = 3;
        outSource.hasAlpha = true;
        outSource.alphaOffset = 2;
        outSource.isRGB565 = true;
        outPivotX = 0.0f;  // its top left
        outPivotY = 0.0f;
        outTintR = outTintG = outTintB = outTintA = 255;
        return true;
    }

    bool GetContentVersion(uint32_t& outVersion) const override
    {
        outVersion = 1;
        return true;
    }

private:
    int Face(int x, int y) const
    {
        if (x < 0 || y < 0 || x >= m_Letter.width || y >= m_Letter.height)
        {
            return 0;
        }
        return SplashShapes::Cover(m_Letter, x, y);
    }

    SplashShapes::Letter m_Letter;
    Deki::Buffer<uint8_t> m_Pixels;
};

/// Plays the splash: moves the icon and the letters, and opens the startup
/// scene once the logo has been on screen for kShowSeconds. Made only by
/// MakeSplashScene, so the reflection codegen never sees it: it is in no menu
/// and in no scene file.
class SplashComponent : public Deki::Component
{
public:
    SplashComponent()
    {
        static const Deki::ComponentTypeInfo kInfo =
            Deki::MakeHandWrittenTypeInfo<SplashComponent>(/*declaresUpdate=*/true);
        SetTypeInfo(&kInfo);
    }

    void Setup(Deki::Scene& scene, float viewHeight)
    {
        // The logo's box, centred: world y points up.
        const float left = -SplashShapes::kWidth * 0.5f;
        const float top = SplashShapes::kHeight * 0.5f;
        m_FeetX = left + kIconLeft + kIconSize * 0.5f;
        m_FeetY = top - kIconSize;
        // It starts just above the top of the screen.
        m_Fall = viewHeight * 0.5f - m_FeetY + 1.0f;

        m_Icon = scene.CreateDekiObject("Icon");
        m_IconArt = m_Icon->AddComponent<SplashIconComponent>();
        for (int i = 0; i < 4; ++i)
        {
            const SplashShapes::Letter& letter = SplashShapes::kLetters[i];
            m_Letters[i] = scene.CreateDekiObject("Letter");
            m_Letters[i]->AddComponent<SplashLetterComponent>(i);
            m_LetterX[i] = left + letter.x;
            m_LetterY[i] = top - letter.y;
        }
        Show(0.0f);
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
            m_Shown += Deki::Time::GetDeltaTimeF() / 1000.0f;
        }
        Show(m_Shown);
        if (m_Shown >= kShowSeconds)
        {
            m_Done = true;
            Deki::PlatformSetupComponent::LoadStartupScene();
        }
    }

private:
    void Show(float t)
    {
        const Pose pose = IconPose(t, m_Fall);
        m_Icon->SetLocalPosition(m_FeetX, m_FeetY + pose.lift);
        m_Icon->SetScale(pose.scaleX, pose.scaleY);
        m_IconArt->SetEyes(EyesAt(t));
        for (int i = 0; i < 4; ++i)
        {
            float lift = 0.0f;
            const bool shown = LetterLift(i, t, lift);
            if (m_Letters[i]->IsActive() != shown)
            {
                m_Letters[i]->SetActive(shown);
            }
            m_Letters[i]->SetLocalPosition(m_LetterX[i], m_LetterY[i] + std::round(lift));
        }
    }

    Deki::Object* m_Icon = nullptr;
    SplashIconComponent* m_IconArt = nullptr;
    Deki::Object* m_Letters[4] = {};
    float m_LetterX[4] = {};
    float m_LetterY[4] = {};
    float m_FeetX = 0.0f;
    float m_FeetY = 0.0f;
    float m_Fall = 0.0f;
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
        std::max(1, std::min(width / SplashShapes::kDesignWidth, height / SplashShapes::kDesignHeight)));
    const float fit =
        std::min(static_cast<float>(width) / SplashShapes::kWidth, static_cast<float>(height) / SplashShapes::kHeight);
    if (fit < 1.0f)
    {
        scale = fit * 0.9f;  // a margin all round
    }

    auto* scene = new Deki::Scene();
    Deki::Object* cameraObject = scene->CreateDekiObject("Camera");
    auto* camera = cameraObject->AddComponent<CameraComponent>();
    camera->clearColor = Deki::Color(0, 0, 0);
    camera->orthoHeight = static_cast<float>(height) / scale;

    Deki::Object* splash = scene->CreateDekiObject("Deki");
    splash->AddComponent<SplashComponent>()->Setup(*scene, camera->orthoHeight);
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
