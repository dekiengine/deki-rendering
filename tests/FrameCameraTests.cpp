/**
 * @file FrameCameraTests.cpp
 * @brief The camera on screens of every size: the per-frame snapshot must map
 *        world to screen exactly like CameraComponent::WorldToScreen, and the
 *        design area must fit each screen per the project's Screen Fit and
 *        Pixel Perfect settings.
 */

#include <gtest/gtest.h>
#include <cmath>
#include <cstdint>

#include <deki/Object.h>
#include <deki/ScreenScale.h>
#include "CameraComponent.h"
#include "FrameCamera.h"
#include "ScopedDesignArea.h"

// The package's types moved into its namespace; tests name them unqualified.
using namespace DekiRendering;

namespace
{

struct CameraFixture
{
    Deki::Object* owner;
    CameraComponent* camera;
    CameraFixture() : owner(new Deki::Object("cam")), camera(owner->AddComponent<CameraComponent>()) {}
    ~CameraFixture() { delete owner; }
};

void ExpectSnapshotMatchesCamera(const CameraComponent& camera, int w, int h)
{
    const FrameCamera fc = camera.CaptureFrameCamera(w, h);
    ASSERT_TRUE(fc.valid);
    EXPECT_EQ(fc.ppm, camera.GetPixelsPerMeter(w, h));

    const float samples[] = { -137.25f, -3.0f, -0.51f, -0.03f, 0.0f, 0.03f, 0.49f, 1.0f, 2.5f, 99.875f };
    for (float wx : samples)
        for (float wy : samples)
        {
            float sx1, sy1, sx2, sy2;
            camera.WorldToScreen(wx, wy, w, h, sx1, sy1);
            fc.WorldToScreen(wx, wy, sx2, sy2);
            EXPECT_EQ(sx1, sx2) << "world (" << wx << ", " << wy << ")";
            EXPECT_EQ(sy1, sy2) << "world (" << wx << ", " << wy << ")";

            // And ScreenToWorld undoes it.
            float bx, by;
            camera.ScreenToWorld(sx1, sy1, w, h, bx, by);
            EXPECT_NEAR(bx, wx, 1e-3f * (1.0f + std::fabs(wx)));
            EXPECT_NEAR(by, wy, 1e-3f * (1.0f + std::fabs(wy)));
        }
}

float VisibleWidth(const CameraComponent& c, int w, int h) { return c.GetVisibleWidth(w, h); }
float VisibleHeight(const CameraComponent& c, int w, int h) { return c.GetVisibleHeight(w, h); }

}  // namespace

TEST(FrameCamera, DefaultIsNotValid)
{
    FrameCamera fc;
    EXPECT_FALSE(fc.valid);
}

TEST(FrameCamera, DesignScreenIsOneToOne)
{
    ScopedDesignArea design(80.0f, 45.0f);  // deki-demo: 1280 x 720 at 16 px/m
    CameraFixture f;
    EXPECT_EQ(f.camera->GetPixelsPerMeter(1280, 720), 16.0f);
    const FrameCamera fc = f.camera->CaptureFrameCamera(1280, 720);
    EXPECT_EQ(fc.halfW, 640.0f);
    EXPECT_EQ(fc.halfH, 360.0f);
    EXPECT_EQ(fc.snapStep, 0);
}

TEST(FrameCamera, ShowAllKeepsTheWholeDesignAreaVisible)
{
    ScopedDesignArea design(80.0f, 45.0f);
    CameraFixture f;
    // 4:3 is narrower: the full 80 m width, and more height than designed.
    EXPECT_FLOAT_EQ(VisibleWidth(*f.camera, 320, 240), 80.0f);
    EXPECT_FLOAT_EQ(VisibleHeight(*f.camera, 320, 240), 60.0f);
    // Portrait: still the full width, a lot more height.
    EXPECT_FLOAT_EQ(VisibleWidth(*f.camera, 240, 320), 80.0f);
    EXPECT_NEAR(VisibleHeight(*f.camera, 240, 320), 106.667f, 1e-3f);
    // Wider than designed: the full height, more width.
    EXPECT_FLOAT_EQ(VisibleHeight(*f.camera, 2560, 1080), 45.0f);
    EXPECT_GT(VisibleWidth(*f.camera, 2560, 1080), 80.0f);
    // Same shape, any resolution: exactly the design area.
    EXPECT_FLOAT_EQ(VisibleWidth(*f.camera, 1920, 1080), 80.0f);
    EXPECT_FLOAT_EQ(VisibleHeight(*f.camera, 1920, 1080), 45.0f);
}

TEST(FrameCamera, FillCoversTheScreenAndCrops)
{
    ScopedDesignArea design(80.0f, 45.0f, Deki::ScreenFit::Fill);
    CameraFixture f;
    // 4:3: the full height, the sides cropped.
    EXPECT_FLOAT_EQ(VisibleHeight(*f.camera, 320, 240), 45.0f);
    EXPECT_FLOAT_EQ(VisibleWidth(*f.camera, 320, 240), 60.0f);
    // Wider: the full width, top and bottom cropped.
    EXPECT_FLOAT_EQ(VisibleWidth(*f.camera, 2560, 1080), 80.0f);
    EXPECT_LT(VisibleHeight(*f.camera, 2560, 1080), 45.0f);
}

TEST(FrameCamera, ZoomScalesTheDesignArea)
{
    ScopedDesignArea design(20.0f, 15.0f);
    CameraFixture f;
    f.camera->zoom = 2.0f;
    EXPECT_FLOAT_EQ(VisibleWidth(*f.camera, 320, 240), 10.0f);
    EXPECT_FLOAT_EQ(VisibleHeight(*f.camera, 320, 240), 7.5f);
    f.camera->zoom = 0.5f;
    EXPECT_FLOAT_EQ(VisibleWidth(*f.camera, 320, 240), 40.0f);
}

TEST(FrameCamera, PixelPerfectScalesByWholeNumbers)
{
    // 320 x 240 of art at 16 px/m.
    ScopedDesignArea design(20.0f, 15.0f, Deki::ScreenFit::ShowAll, /*pixelPerfect=*/true);
    CameraFixture f;
    EXPECT_EQ(f.camera->GetPixelsPerMeter(320, 240), 16.0f);   // 1x
    EXPECT_EQ(f.camera->GetPixelsPerMeter(640, 480), 32.0f);   // 2x
    EXPECT_EQ(f.camera->GetPixelsPerMeter(1280, 720), 48.0f);  // 3x, extra world at the sides
    EXPECT_EQ(f.camera->GetPixelsPerMeter(400, 300), 16.0f);   // 1.25 rounds down to 1x
    EXPECT_EQ(f.camera->GetPixelsPerMeter(200, 150), 16.0f);   // smaller: stays 1x and crops
    EXPECT_FLOAT_EQ(VisibleWidth(*f.camera, 200, 150), 12.5f);

    f.camera->zoom = 1.5f;  // 1x * 1.5 rounds to 2x
    EXPECT_EQ(f.camera->GetPixelsPerMeter(320, 240), 32.0f);
}

TEST(FrameCamera, PixelPerfectPutsTheCameraOnTheArtGrid)
{
    ScopedDesignArea design(20.0f, 15.0f, Deki::ScreenFit::ShowAll, /*pixelPerfect=*/true);
    CameraFixture f;
    f.owner->SetX(0.04f);   // 0.64 art px: snaps to 1
    f.owner->SetY(-0.02f);  // -0.32 art px: snaps to 0
    const FrameCamera fc = f.camera->CaptureFrameCamera(641, 481);  // 2x, odd size
    EXPECT_EQ(fc.camX, 1.0f / 16.0f);
    EXPECT_EQ(fc.camY, 0.0f);
    EXPECT_EQ(fc.snapStep, 2);
    EXPECT_EQ(fc.halfW, 320.0f);  // whole, so the grid lands on whole pixels
    EXPECT_EQ(fc.halfH, 240.0f);
    EXPECT_EQ(fc.SnapX(fc.halfW + 3.2f), fc.halfW + 4.0f);
    ExpectSnapshotMatchesCamera(*f.camera, 641, 481);
}

TEST(FrameCamera, FixedPixelsPerMeterIgnoresTheDesignArea)
{
    ScopedDesignArea design(80.0f, 45.0f, Deki::ScreenFit::ShowAll, /*pixelPerfect=*/true);
    CameraFixture f;
    f.camera->SetFixedPixelsPerMeter(16.0f);
    EXPECT_EQ(f.camera->GetPixelsPerMeter(333, 777), 16.0f);
    EXPECT_EQ(f.camera->CaptureFrameCamera(333, 777).snapStep, 0);  // the scene view is not a screen
}

TEST(FrameCamera, EmptyScreenDrawsNothing)
{
    ScopedDesignArea design(20.0f, 15.0f);
    CameraFixture f;
    EXPECT_EQ(f.camera->GetPixelsPerMeter(0, 240), 0.0f);
    EXPECT_FALSE(f.camera->CaptureFrameCamera(0, 240).valid);
}

TEST(FrameCamera, MatchesCameraAtManySizes)
{
    ScopedDesignArea design(20.0f, 15.0f);
    CameraFixture f;
    f.owner->SetX(0.37f);
    f.owner->SetY(-2.125f);
    ExpectSnapshotMatchesCamera(*f.camera, 320, 240);
    ExpectSnapshotMatchesCamera(*f.camera, 63, 47);
    f.camera->zoom = 1.5f;
    ExpectSnapshotMatchesCamera(*f.camera, 128, 96);
}

TEST(FrameCamera, ParentedCameraUsesWorldPosition)
{
    ScopedDesignArea design(20.0f, 15.0f);
    CameraFixture f;
    auto* rig = new Deki::Object("rig");
    rig->SetX(10.0f);
    rig->SetScale(2.0f, 2.0f);
    // Re-parent: the fixture deletes `owner` through `rig` afterwards, so hand
    // ownership over and clear the fixture's pointer.
    Deki::Object* cam = f.owner;
    f.owner = rig;
    rig->AddChild(cam);
    cam->SetX(0.5f);  // world 11.0
    const FrameCamera fc = f.camera->CaptureFrameCamera(100, 100);
    EXPECT_EQ(fc.camX, 11.0f);
    ExpectSnapshotMatchesCamera(*f.camera, 100, 100);
}

TEST(FrameCamera, PerspectiveFieldOfViewFollowsTheFit)
{
    ScopedDesignArea design(16.0f, 9.0f);
    // The design shape keeps the field of view.
    EXPECT_FLOAT_EQ(Deki::ResolveVerticalFieldOfView(60.0f, 1920, 1080), 60.0f);
    // Narrower under Show All: wider vertically, so the design's width still fits.
    EXPECT_GT(Deki::ResolveVerticalFieldOfView(60.0f, 320, 240), 60.0f);
    // Wider under Show All: unchanged.
    EXPECT_FLOAT_EQ(Deki::ResolveVerticalFieldOfView(60.0f, 2560, 1080), 60.0f);
}
