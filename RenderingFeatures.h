#pragma once

// deki-rendering's features (package.json), for code that tests them.
//
// "lights" (DEKI_FEATURE_LIGHTS): the Light component, and what the 2D light
// and deki-3d gather from it. A device build turns it off when none of its
// scenes has a Light; then Light.cpp is left out, nothing is lit, and the
// light loops stay in flash instead of taking internal RAM. Code that uses
// Light includes Light.h only inside #ifdef DEKI_FEATURE_LIGHTS, so the build
// can tell it may leave it out.
//
// A build that does not configure features (the editor's) has them all.
#if !defined(DEKI_PACKAGE_FEATURES_CONFIGURED) && !defined(DEKI_FEATURE_LIGHTS)
#define DEKI_FEATURE_LIGHTS
#endif
