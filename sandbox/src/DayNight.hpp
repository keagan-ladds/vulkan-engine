#pragma once

// =============================================================================
// DayNightCycle - moves the sun and moon (two shadow-casting directional
// lights with cascades), colours the sunlight with the atmosphere's
// transmittance (orange sunsets!), dims for weather, and adjusts exposure.
// =============================================================================

#include <ve/scene/GameObject.hpp>

namespace ve {
class DirectionalLight;
}
namespace vfx {
class Weather;
}

namespace savanna {

class DayNightCycle : public ve::GameObject {
public:
    DayNightCycle();

    void start() override;
    void update(float dt) override;

    float timeOfDay = 17.3f;     // hours, 0..24
    float dayLengthMinutes = 16.0f; // real minutes per in-game day
    float sunIntensity = 3.2f;
    float moonIntensity = 0.14f;
    vfx::Weather* weather = nullptr;

    ve::DirectionalLight* sun() const { return m_sun; }
    ve::DirectionalLight* moon() const { return m_moon; }
    glm::vec3 towardsSun() const { return m_towardsSun; }
    static glm::vec3 sunDirection(float hours);

private:
    ve::DirectionalLight* m_sun = nullptr;
    ve::DirectionalLight* m_moon = nullptr;
    ve::GameObject* m_sunObject = nullptr;
    ve::GameObject* m_moonObject = nullptr;
    glm::vec3 m_towardsSun{0, 1, 0};
};

} // namespace savanna
