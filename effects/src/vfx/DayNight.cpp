#include "vfx/DayNight.hpp"

#include <ve/ui/UiLayer.hpp>

#include <ve/audio/Audio.hpp>
#include <ve/core/Engine.hpp>

#include <imgui.h>

#include <ve/core/Input.hpp>
#include <ve/core/Time.hpp>
#include <ve/render/Renderer.hpp>
#include <ve/render/passes/BuiltinPasses.hpp>
#include <ve/scene/Components.hpp>
#include <ve/scene/Scene.hpp>
#include "vfx/Atmosphere.hpp"
#include "vfx/Weather.hpp"


#include <glm/gtc/constants.hpp>

#include <cmath>

namespace vfx {

DayNightCycle::DayNightCycle() : GameObject("DayNight")
{
    runInEditor = true; // the sky and lighting follow edits while the game is paused
}

void DayNightCycle::onDestroy()
{
    if (auto* atmo = renderer().graph().findPass<vfx::AtmospherePass>()) {
        if (atmo->sunObject == m_sun) atmo->sunObject.reset();
        if (atmo->moonObject == m_moon) atmo->moonObject.reset();
    }
    // The lights are separate objects we created (unless the editor already deleted them).
    if (ve::GameObject* o = m_sun.get(scene())) o->destroy();
    if (ve::GameObject* o = m_moon.get(scene())) o->destroy();
    m_sun.reset();
    m_moon.reset();
}

static ve::DirectionalLight* lightOn(const ve::GameObject* o)
{
    return o ? o->getComponent<ve::DirectionalLight>() : nullptr;
}

Weather* DayNightCycle::weather() const
{
    return m_weather.get(scene());
}

ve::DirectionalLight* DayNightCycle::sun() const
{
    return lightOn(m_sun.get(scene()));
}

ve::DirectionalLight* DayNightCycle::moon() const
{
    return lightOn(m_moon.get(scene()));
}

glm::vec3 DayNightCycle::sunDirection(float hours)
{
    // 6:00 = east horizon (+X), 12:00 = high (tilted south, +Z), 18:00 = west (-X).
    const float theta = (hours - 6.0f) / 24.0f * glm::two_pi<float>();
    const float tilt = glm::radians(25.0f);
    return glm::normalize(glm::vec3(std::cos(theta), std::sin(theta) * std::cos(tilt), std::sin(theta) * std::sin(tilt)));
}

void DayNightCycle::start()
{
    ve::GameObject& sunObject = scene().spawn("Sun");
    sunObject.runInEditor = true;
    sunObject.addComponent<ve::DirectionalLight>().castShadows = true;
    m_sun = &sunObject;

    ve::GameObject& moonObject = scene().spawn("Moon");
    moonObject.runInEditor = true;
    auto& moon = moonObject.addComponent<ve::DirectionalLight>();
    moon.castShadows = true;
    moon.color = {0.55f, 0.65f, 1.0f};
    m_moon = &moonObject;

    if (auto* atmo = renderer().graph().findPass<vfx::AtmospherePass>()) {
        atmo->sunObject = m_sun;
        atmo->moonObject = m_moon;
    }
}

void DayNightCycle::update(float dt)
{
    ve::Input& in = input();
    ve::DirectionalLight* sunLight = sun();
    ve::DirectionalLight* moonLight = moon();
    Weather* weatherNow = m_weather.get(scene());
    if (!weatherNow) m_weather = weatherNow = scene().findFirst<Weather>(); // (none yet, or replaced)
    float speed = 24.0f / (std::max(dayLengthMinutes, 0.01f) * 60.0f); // hours per second
    if (engine().editorPaused() && !advanceInEditor) speed = 0.0f;
    if (timeKeys && in.keyDown(ve::Key::T)) speed = 24.0f / 60.0f; // fast forward: 1 h per 2.5 s
    if (timeKeys && in.keyDown(ve::Key::G)) speed = -24.0f / 60.0f; // rewind
    timeOfDay = std::fmod(timeOfDay + speed * dt + 24.0f, 24.0f);

    m_towardsSun = sunDirection(timeOfDay);
    // The moon roughly opposite the sun, offset so it isn't exactly in line.
    const glm::vec3 towardsMoon = glm::normalize(-m_towardsSun + glm::vec3(0.25f, 0.35f, -0.2f));

    if (sunLight) sunLight->transform().setRotation(glm::quatLookAt(-m_towardsSun, glm::vec3(0, 1, 0)));
    if (moonLight) moonLight->transform().setRotation(glm::quatLookAt(-towardsMoon, glm::vec3(0, 1, 0)));

    // Sunlight colour = what survives the atmosphere: white at noon, deep orange at sunset.
    const float haze = weatherNow ? weatherNow->current().haze : 1.5f;
    const glm::vec3 T = vfx::AtmospherePass::sunTransmittance(m_towardsSun, haze);
    const float maxT = std::max({T.r, T.g, T.b, 1e-4f});
    const float weatherScale = weatherNow ? weatherNow->sunScale() : 1.0f;
    if (sunLight) {
        sunLight->color = T / maxT;
        sunLight->intensity = sunIntensity * maxT * weatherScale;
        sunLight->enabled = sunLight->intensity > 0.002f;
    }

    const float moonUp = glm::smoothstep(-0.05f, 0.15f, towardsMoon.y);
    const float night = glm::smoothstep(0.08f, -0.12f, m_towardsSun.y);
    if (moonLight) {
        moonLight->intensity = moonIntensity * moonUp * night * (0.4f + 0.6f * weatherScale);
        moonLight->enabled = moonLight->intensity > 0.002f;
    }

    // For ambience (AmbientLayer hours) and game code.
    audio().setParameter("timeOfDay", timeOfDay);
    audio().setParameter("night", night);

    // Simple "eye adaptation": brighter exposure at night.
    if (auto* composite = renderer().graph().findPass<ve::CompositePass>()) {
        composite->exposure = glm::mix(1.0f, nightExposure, night);
    }
}

void DayNightCycle::onGui()
{
    if (!engine().ui().debugOverlay) return;
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 430.0f, 90.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Day / night")) {
        ImGui::SliderFloat("Time of day", &timeOfDay, 0.0f, 24.0f, "%.2f h");
        ImGui::SliderFloat("Day length (min)", &dayLengthMinutes, 1.0f, 120.0f);
        ImGui::SliderFloat("Sun intensity", &sunIntensity, 0.0f, 8.0f);
        ImGui::SliderFloat("Moon intensity", &moonIntensity, 0.0f, 0.5f);
        ImGui::SliderFloat("Night exposure", &nightExposure, 0.5f, 4.0f);
        if (auto* atmo = renderer().graph().findPass<vfx::AtmospherePass>())
            ImGui::ColorEdit3("Night ambient", &atmo->nightAmbient.x, ImGuiColorEditFlags_Float);
    }
    ImGui::End();
}

} // namespace vfx
