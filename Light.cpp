#include "Light.h"

#include <deki/Engine.h>
#include <deki/Object.h>
#include <deki/SceneSystem.h>

#include <algorithm>

namespace DekiRendering
{
namespace
{
// Every Light alive, in the order they were made. A scene holds tens of
// lights at most, so the renderers scan this once a frame instead of walking
// the scene for them.
std::vector<Light*>& Lights()
{
    static std::vector<Light*> s_Lights;
    return s_Lights;
}

uint32_t s_ListVersion = 0;

bool IsPersistent(const Deki::Object* obj)
{
    while (obj->GetParent())
    {
        obj = obj->GetParent();
    }
    const auto& persistent = Deki::Engine::GetInstance().GetSceneSystem().GetPersistentObjects();
    return std::find(persistent.begin(), persistent.end(), obj) != persistent.end();
}
}  // namespace

Light::Light()
{
    Lights().push_back(this);
    s_ListVersion++;
}

Light::~Light()
{
    auto& lights = Lights();
    lights.erase(std::remove(lights.begin(), lights.end(), this), lights.end());
    s_ListVersion++;
}

bool Light::Shines() const
{
    const Deki::Object* owner = GetOwner();
    if (!owner || !owner->IsActiveInHierarchy() || !(intensity > 0.0f))
    {
        return false;
    }
    if (color.r == 0 && color.g == 0 && color.b == 0)
    {
        return false;
    }
    if ((type == LightType::Point || type == LightType::Spot) && !(range > 0.0f))
    {
        return false;
    }
    return true;
}

void Light::InScene(const Deki::Scene* scene, std::vector<const Light*>& out)
{
    out.clear();
    for (const Light* light : Lights())
    {
        const Deki::Object* owner = light->GetOwner();
        if (!owner || !owner->IsActiveInHierarchy())
        {
            continue;
        }
        if (owner->GetOwnerScene() == scene || IsPersistent(owner))
        {
            out.push_back(light);
        }
    }
}

const std::vector<Light*>& Light::All()
{
    return Lights();
}

uint32_t Light::ListVersion()
{
    return s_ListVersion;
}

}  // namespace DekiRendering
