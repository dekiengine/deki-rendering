// Only where the platform has OpenGL: the build defines DEKI_CAPABILITY_OPENGL
// for one whose platform provides "opengl" (the desktop, a Linux handheld) and
// never for a microcontroller, where this file compiles to nothing.
#ifdef DEKI_CAPABILITY_OPENGL

#include "ScreenEffect.h"

#include <deki/Object.h>

#include <algorithm>
#include <vector>

namespace DekiGL
{
namespace
{
// Every ScreenEffect alive, in the order they were made. A scene holds a
// handful of components like this at most, so a list scanned once a frame is
// all it needs.
std::vector<ScreenEffect*>& Effects()
{
    static std::vector<ScreenEffect*> s_Effects;
    return s_Effects;
}
}  // namespace

ScreenEffect::ScreenEffect()
{
    Effects().push_back(this);
}

ScreenEffect::~ScreenEffect()
{
    auto& effects = Effects();
    effects.erase(std::remove(effects.begin(), effects.end(), this), effects.end());
}

const ScreenEffect* ScreenEffect::Current()
{
    for (const ScreenEffect* effect : Effects())
    {
        const Deki::Object* owner = effect->GetOwner();
        if (owner && owner->IsActiveInHierarchy() && effect->shader.HasGuid() && effect->shader.Get())
        {
            return effect;
        }
    }
    return nullptr;
}

}  // namespace DekiGL

#endif  // DEKI_CAPABILITY_OPENGL
