// The camera on screens of every size. The per-frame snapshot must map world
// to screen exactly like CameraComponent::WorldToScreen, and the ortho height
// must frame each screen the same way: a fixed height, a width that follows
// the screen, and whole-number scaling under Pixel Perfect.

#include <gtest/gtest.h>
#include <cmath>
#include <cstdint>

#include <deki/Object.h>
#include "CameraComponent.h"
#include "FrameCamera.h"
#include "ScopedArtDensity.h"

// Tests name the package's types unqualified.
using namespace DekiRendering;

namespace
{

struct CameraFixture
{
    Deki::Object* owner;
    CameraComponent* camera;
    CameraFixture()
        : owner(new Deki::Object("cam")),
          camera(owner->AddComponent<CameraComponent>())
    {
    }
    ~CameraFixture() { delete owner; }
};

void ExpectSnapshotMatchesCamera(const CameraComponent& camera, int w, int h)
{
    const FrameCamera fc = camera.CaptureFrameCamera(w, h);
    ASSERT_TRUE(fc.valid);
    EXPECT_EQ(fc.ppm, camera.GetPixelsPerMeter(w, h));

    const float samples[] = { -137.25f, -3.0f, -0.51f, -0.03f, 0.0f, 0.03f, 0.49f, 1.0f, 2.5f, 99.875f };
    for (float wx : samples)
    {
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
}

float VisibleWidth(const CameraComponent& c, int w, int h)
{
    return c.GetVisibleWidth(w, h);
}
float VisibleHeight(const CameraComponent& c, int w, int h)
{
    return c.GetVisibleHeight(w, h);
}

}  // namespace

TEST(FrameCamera, DefaultIsNotValid)
{
    FrameCamera fc;
    EXPECT_FALSE(fc.valid);
}

TEST(FrameCamera, OrthoHeightFillsTheScreenHeight)
{
    ScopedArtDensity art;
    CameraFixture f;
    f.camera->orthoHeight = 45.0f;  // 1280 x 720 at 16 px/m
    EXPECT_EQ(f.camera->GetPixelsPerMeter(1280, 720), 16.0f);
    const FrameCamera fc = f.camera->CaptureFrameCamera(1280, 720);
    EXPECT_EQ(fc.halfW, 640.0f);
    EXPECT_EQ(fc.halfH, 360.0f);
    EXPECT_EQ(fc.snapStep, 0);
}

TEST(FrameCamera, BiggerScreenShowsTheSameWorldBigger)
{
    ScopedArtDensity art;
    CameraFixture f;
    f.camera->orthoHeight = 15.0f;
    // Same shape, 3.2x the pixels: the same 20 x 15 m, drawn 3.2x larger.
    EXPECT_FLOAT_EQ(VisibleWidth(*f.camera, 320, 240), 20.0f);
    EXPECT_FLOAT_EQ(VisibleHeight(*f.camera, 320, 240), 15.0f);
    EXPECT_FLOAT_EQ(VisibleWidth(*f.camera, 1024, 768), 20.0f);
    EXPECT_FLOAT_EQ(VisibleHeight(*f.camera, 1024, 768), 15.0f);
    EXPECT_FLOAT_EQ(f.camera->GetPixelsPerMeter(1024, 768), 3.2f * f.camera->GetPixelsPerMeter(320, 240));
}

TEST(FrameCamera, WidthFollowsTheScreenShape)
{
    ScopedArtDensity art;
    CameraFixture f;
    f.camera->orthoHeight = 11.25f;
    // The height never changes; a wider screen shows more at the sides, a
    // narrower one less.
    EXPECT_FLOAT_EQ(VisibleHeight(*f.camera, 1280, 720), 11.25f);
    EXPECT_FLOAT_EQ(VisibleWidth(*f.camera, 1280, 720), 20.0f);
    EXPECT_FLOAT_EQ(VisibleHeight(*f.camera, 320, 240), 11.25f);
    EXPECT_FLOAT_EQ(VisibleWidth(*f.camera, 320, 240), 15.0f);
    EXPECT_FLOAT_EQ(VisibleHeight(*f.camera, 2560, 1080), 11.25f);
    EXPECT_GT(VisibleWidth(*f.camera, 2560, 1080), 20.0f);
    EXPECT_FLOAT_EQ(VisibleHeight(*f.camera, 240, 320), 11.25f);  // portrait
}

TEST(FrameCamera, SmallerOrthoHeightIsCloser)
{
    ScopedArtDensity art;
    CameraFixture f;
    f.camera->orthoHeight = 7.5f;
    EXPECT_EQ(f.camera->GetPixelsPerMeter(320, 240), 32.0f);
    EXPECT_FLOAT_EQ(VisibleWidth(*f.camera, 320, 240), 10.0f);
}

TEST(FrameCamera, PixelPerfectScalesByWholeNumbers)
{
    // 15 m of art at 16 px/m: 240 art pixels tall.
    ScopedArtDensity art;
    CameraFixture f;
    f.camera->orthoHeight = 15.0f;
    f.camera->pixelPerfect = true;
    EXPECT_EQ(f.camera->GetPixelsPerMeter(320, 240), 16.0f);   // 1x
    EXPECT_EQ(f.camera->GetPixelsPerMeter(640, 480), 32.0f);   // 2x
    EXPECT_EQ(f.camera->GetPixelsPerMeter(1280, 720), 48.0f);  // 3x
    EXPECT_EQ(f.camera->GetPixelsPerMeter(400, 300), 16.0f);   // 1.25 rounds down: a little more world
    EXPECT_FLOAT_EQ(VisibleHeight(*f.camera, 400, 300), 18.75f);
    EXPECT_EQ(f.camera->GetPixelsPerMeter(200, 150), 16.0f);  // smaller: stays 1x and crops
    EXPECT_FLOAT_EQ(VisibleHeight(*f.camera, 200, 150), 9.375f);

    f.camera->orthoHeight = 10.0f;  // 1.5x at 240 rounds down to 1x
    EXPECT_EQ(f.camera->GetPixelsPerMeter(320, 240), 16.0f);
}

TEST(FrameCamera, PixelPerfectPutsTheCameraOnTheArtGrid)
{
    ScopedArtDensity art;
    CameraFixture f;
    f.camera->orthoHeight = 15.0f;
    f.camera->pixelPerfect = true;
    f.owner->SetX(0.04f);                                           // 0.64 art px: snaps to 1
    f.owner->SetY(-0.02f);                                          // -0.32 art px: snaps to 0
    const FrameCamera fc = f.camera->CaptureFrameCamera(641, 481);  // 2x, odd size
    EXPECT_EQ(fc.camX, 1.0f / 16.0f);
    EXPECT_EQ(fc.camY, 0.0f);
    EXPECT_EQ(fc.snapStep, 2);
    EXPECT_EQ(fc.halfW, 320.0f);  // whole, so the grid lands on whole pixels
    EXPECT_EQ(fc.halfH, 240.0f);
    EXPECT_EQ(fc.SnapX(fc.halfW + 3.2f), fc.halfW + 4.0f);
    ExpectSnapshotMatchesCamera(*f.camera, 641, 481);
}

TEST(FrameCamera, FixedPixelsPerMeterIgnoresTheOrthoHeight)
{
    ScopedArtDensity art;
    CameraFixture f;
    f.camera->pixelPerfect = true;
    f.camera->SetFixedPixelsPerMeter(16.0f);
    EXPECT_EQ(f.camera->GetPixelsPerMeter(333, 777), 16.0f);
    EXPECT_EQ(f.camera->CaptureFrameCamera(333, 777).snapStep, 0);  // the scene view is not a screen
}

TEST(FrameCamera, EmptyScreenDrawsNothing)
{
    ScopedArtDensity art;
    CameraFixture f;
    EXPECT_EQ(f.camera->GetPixelsPerMeter(320, 0), 0.0f);
    EXPECT_FALSE(f.camera->CaptureFrameCamera(320, 0).valid);
    f.camera->orthoHeight = 0.0f;
    EXPECT_EQ(f.camera->GetPixelsPerMeter(320, 240), 0.0f);
}

TEST(FrameCamera, MatchesCameraAtManySizes)
{
    ScopedArtDensity art;
    CameraFixture f;
    f.owner->SetX(0.37f);
    f.owner->SetY(-2.125f);
    ExpectSnapshotMatchesCamera(*f.camera, 320, 240);
    ExpectSnapshotMatchesCamera(*f.camera, 63, 47);
    f.camera->orthoHeight = 10.0f;
    ExpectSnapshotMatchesCamera(*f.camera, 128, 96);
}

TEST(FrameCamera, ParentedCameraUsesWorldPosition)
{
    ScopedArtDensity art;
    CameraFixture f;
    auto* rig = new Deki::Object("rig");
    rig->SetX(10.0f);
    rig->SetScale(2.0f, 2.0f);
    // Re-parent: the fixture deletes `owner` through `rig` afterwards, so
    // hand ownership over and clear the fixture's pointer.
    Deki::Object* cam = f.owner;
    f.owner = rig;
    rig->AddChild(cam);
    cam->SetX(0.5f);  // world 11.0
    const FrameCamera fc = f.camera->CaptureFrameCamera(100, 100);
    EXPECT_EQ(fc.camX, 11.0f);
    ExpectSnapshotMatchesCamera(*f.camera, 100, 100);
}

TEST(FrameCamera, PerspectiveFieldOfViewIsVertical)
{
    CameraFixture f;
    f.camera->projection = Deki::ProjectionMode::Perspective;
    f.camera->fieldOfView = 60.0f;
    // The vertical field of view is the same on every screen; only the width
    // (the aspect) changes. Mat4 is m[column][row].
    const Deki::Mat4 wide = f.camera->GetProjectionMatrix(1920, 1080);
    const Deki::Mat4 small = f.camera->GetProjectionMatrix(320, 240);
    const Deki::Mat4 big = f.camera->GetProjectionMatrix(640, 480);
    EXPECT_FLOAT_EQ(wide.m[1][1], small.m[1][1]);  // y scale = 1 / tan(fov / 2)
    EXPECT_NE(wide.m[0][0], small.m[0][0]);        // x scale follows the aspect
    EXPECT_FLOAT_EQ(small.m[0][0], big.m[0][0]);   // same shape, same picture
}
