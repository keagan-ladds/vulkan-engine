#pragma once

// =============================================================================
// DayNightCycle - moves the sun and moon (two shadow-casting directional
// lights with cascades), colours the sunlight with the atmosphere's
// transmittance (orange sunsets!), dims for weather, and adjusts exposure.
// A scene object (registered): put one in a scene to get a sky that moves.
// Uses the scene's vfx::Weather (if any) for haze and dimming.
// =============================================================================

#include <ve/scene/GameObject.hpp>
#include <ve/scene/SaveGame.hpp>

namespace ve {
class DirectionalLight;
}

namespace vfx {

class Weather;

class DayNightCycle : public ve::GameObject, public ve::Saveable {
public:
    DayNightCycle();
    void saveState(ve::Json& out) const override { out["timeOfDay"] = timeOfDay; }
    void loadState(const ve::Json& in) override { timeOfDay = in.value("timeOfDay", timeOfDay); }

    void start() override;
    void update(float dt) override;
    void onDestroy() override;
    void onGui() override; // "Day / night" window while the F1 overlay is open

    float timeOfDay = 17.3f;     // hours, 0..24
    bool advanceInEditor = false; // keep the clock running while the editor is paused
    bool timeKeys = false;        // T / G: fast-forward / rewind (debug)
    float dayLengthMinutes = 16.0f; // real minutes per in-game day
    float sunIntensity = 3.2f;
    float moonIntensity = 0.08f;
    // Night brightness. Exposure rises towards this at night (crude eye
    // adaptation): 1 = no adaptation (darkest), 3 = bright "game" nights.
    float nightExposure = 1.4f;
    // The scene's Weather (found automatically), or null. Valid for the current frame.
    Weather* weather() const { return m_weather; }

    ve::DirectionalLight* sun() const { return m_sun; }
    ve::DirectionalLight* moon() const { return m_moon; }
    glm::vec3 towardsSun() const { return m_towardsSun; }
    static glm::vec3 sunDirection(float hours);

private:
    Weather* m_weather = nullptr; // looked up every frame by id (can't dangle)
    uint64_t m_weatherId = 0;
    ve::DirectionalLight* m_sun = nullptr;
    ve::DirectionalLight* m_moon = nullptr;
    ve::GameObject* m_sunObject = nullptr;
    ve::GameObject* m_moonObject = nullptr;
    glm::vec3 m_towardsSun{0, 1, 0};
};

} // namespace vfx
