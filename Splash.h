#pragma once

namespace DekiRendering
{

/// The splash screen: the Deki logo on black, shown after the boot scene while
/// the game's startup scene loads, for at least Splash.cpp's kShowSeconds.
/// It is the "splash" feature (DEKI_FEATURE_SPLASH), off until the project
/// turns it on (the editor's set_build_feature tool, or the Package Manager);
/// while off, it and its 24 KB picture are left out of the build. Called at
/// init.
void RegisterSplash();

/// Takes the splash away again, before the package unloads.
void UnregisterSplash();

}  // namespace DekiRendering
