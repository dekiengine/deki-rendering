#pragma once

namespace DekiRendering
{

/// The splash screen: the Deki logo on black, shown after the boot scene and
/// before the game's startup scene. The icon falls in and lands, the landing
/// jolts the letters, and the logo fades out; the startup scene opens
/// Splash.cpp's kShowSeconds after the logo first shows. It is drawn from the
/// brand's shapes and the letters' masks (SplashShapes.h, about half a
/// kilobyte), into about 19 KB of RAM that is freed with the splash.
/// It is the "splash" feature (DEKI_FEATURE_SPLASH), off until the project
/// turns it on (the editor's set_build_feature tool, or the Package Manager);
/// while off, it is left out of the build. Called at init.
void RegisterSplash();

/// Takes the splash away again, before the package unloads.
void UnregisterSplash();

}  // namespace DekiRendering
