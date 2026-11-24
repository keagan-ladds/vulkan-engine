#pragma once

// =============================================================================
// DayNightCycle - moves the sun and moon (two shadow-casting directional
// lights with cascades), colours the sunlight with the atmosphere's
// transmittance (orange sunsets!), dims for weather, and adjusts exposure.
// A scene object (registered): put one in a scene to get a sky that moves.
// Uses the scene's vfx::Weather (if any) for haze and dimming.
// =============================================================================

#include <ve/scene/GameObject.hpp>
#include <ve/scene/ObjectRef.hpp>
#include <ve/scene/SaveGame.hpp>

namespace ve {
class DirectionalLight;
}

namespace vfx {

class Weather;

/// Moves the sun and moon over a 24-hour clock, colours them through the
/// atmosphere, and adapts exposure at night. Publishes the time of day to audio ("timeOfDay").
class DayNightCycle : public ve::GameObject, public ve::Saveable {
public:
    DayNightCycle();
    void saveState(ve::Json& out) const override { out["timeOfDay"] = timeOfDay; }
    void loadState(const ve::Json& in) override { timeOfDay = in.value("timeOfDay", timeOfDay); }

    void start() override;
    void update(float dt) override;
    void onDestroy() override;
    void onGui() override; ///< "Day / night" window while the F1 overlay is open

    float timeOfDay = 17.3f;     ///< hours, 0..24
    bool advanceInEditor = false; ///< keep the clock running while the editor is paused
    bool timeKeys = false;        ///< T / G: fast-forward / rewind (debug)
    float dayLengthMinutes = 16.0f; ///< real minutes per in-game day
    float sunIntensity = 3.2f;  ///< Sun light intensity at noon (dims through the atmosphere).
    float moonIntensity = 0.08f;  ///< Moon light intensity.
    /// Night brightness. Exposure rises towards this at night (crude eye
    /// adaptation): 1 = no adaptation (darkest), 3 = bright "game" nights.
    float nightExposure = 1.4f;
    /// The scene's Weather (found automatically), or null. Don't keep the pointer.
    Weather* weather() const;

    /// The lights (separate objects; the editor can delete them). May be null; don't keep the pointers.
    ve::DirectionalLight* sun() const;
    /// The moon light (may be null; don't keep the pointer).
    ve::DirectionalLight* moon() const;
    /// Unit direction towards the sun now.
    glm::vec3 towardsSun() const { return m_towardsSun; }
    /// Unit direction towards the sun at `hours` (0..24).
    static glm::vec3 sunDirection(float hours);

private:
    /// By id: the editor can delete or replace the Weather and the Sun / Moon
    /// objects at any time (and undo-restore them with the same ids, so the
    /// lights are found through their objects, not remembered as components).
    ve::ObjectRef<Weather> m_weather;
    ve::ObjectRef<> m_sun, m_moon;
    glm::vec3 m_towardsSun{0, 1, 0};
};

} // namespace vfx
