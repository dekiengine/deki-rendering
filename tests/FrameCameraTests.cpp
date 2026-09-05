/**
 * @file FrameCameraTests.cpp
 * @brief The per-frame camera snapshot must map world to screen exactly like
 *        CameraComponent::WorldToScreen, for every camera setting: inherited
 *        and explicit pixels-per-meter, pixel snap on and off, negative and
 *        fractional camera positions, odd target sizes.
 */

#include <gtest/gtest.h>
#include <cstdint>

#include "DekiObject.h"
#include "CameraComponent.h"
#include "FrameCamera.h"

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
    EXPECT_EQ(fc.ppm, camera.GetPixelsPerMeter());
    EXPECT_EQ(fc.halfW, static_cast<float>(w) * 0.5f);
    EXPECT_EQ(fc.halfH, static_cast<float>(h) * 0.5f);

    const float samples[] = { -137.25f, -3.0f, -0.51f, -0.03f, 0.0f, 0.03f, 0.49f, 1.0f, 2.5f, 99.875f };
    for (float wx : samples)
        for (float wy : samples)
        {
            float sx1, sy1, sx2, sy2;
            camera.WorldToScreen(wx, wy, w, h, sx1, sy1);
            fc.WorldToScreen(wx, wy, sx2, sy2);
            EXPECT_EQ(sx1, sx2) << "world (" << wx << ", " << wy << ")";
            EXPECT_EQ(sy1, sy2) << "world (" << wx << ", " << wy << ")";
        }
}

}  // namespace

TEST(FrameCamera, DefaultIsNotValid)
{
    FrameCamera fc;
    EXPECT_FALSE(fc.valid);
}

TEST(FrameCamera, MatchesCameraWithInheritedPpm)
{
    CameraFixture f;
    f.owner->SetX(0.37f);
    f.owner->SetY(-2.125f);
    ExpectSnapshotMatchesCamera(*f.camera, 320, 240);
    ExpectSnapshotMatchesCamera(*f.camera, 63, 47);
}

TEST(FrameCamera, MatchesCameraWithExplicitPpm)
{
    CameraFixture f;
    f.camera->pixelsPerMeter = 24.0f;
    f.owner->SetX(-5.5f);
    f.owner->SetY(0.01f);
    ExpectSnapshotMatchesCamera(*f.camera, 128, 96);
}

TEST(FrameCamera, MatchesCameraWithPixelSnap)
{
    CameraFixture f;
    f.camera->pixelSnap = true;
    f.camera->pixelsPerMeter = 16.0f;
    f.owner->SetX(0.04f);   // 0.64 px: snaps to 1 px
    f.owner->SetY(-0.02f);  // -0.32 px: snaps to 0
    const FrameCamera fc = f.camera->CaptureFrameCamera(64, 48);
    EXPECT_EQ(fc.camX, 1.0f / 16.0f);
    EXPECT_EQ(fc.camY, 0.0f);
    ExpectSnapshotMatchesCamera(*f.camera, 64, 48);
}

TEST(FrameCamera, ParentedCameraUsesWorldPosition)
{
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
