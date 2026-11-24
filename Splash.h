#pragma once

namespace DekiRendering
{

/// The splash screen: the Deki logo on black, shown after the boot scene while
/// the game's startup scene loads, for at least Splash.cpp's kShowSeconds.
/// The icon falls in and lands, and the landing jolts the letters. It is drawn
/// from the brand's shapes and the letters' masks (SplashShapes.h, about half
/// a kilobyte), into about 19 KB of RAM that is freed with the splash.
/// It is the "splash" feature (DEKI_FEATURE_SPLASH), off until the project
/// turns it on (the editor's set_build_feature tool, or the Package Manager);
/// while off, it is left out of the build. Called at init.
void RegisterSplash();

/// Takes the splash away again, before the package unloads.
void UnregisterSplash();

}  // namespace DekiRendering
