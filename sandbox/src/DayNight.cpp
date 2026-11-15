#include "DayNight.hpp"

#include <ve/ui/UiLayer.hpp>

#include <ve/core/Engine.hpp>

#include <imgui.h>

#include <ve/core/Input.hpp>
#include <ve/core/Time.hpp>
#include <ve/render/Renderer.hpp>
#include <ve/render/passes/BuiltinPasses.hpp>
#include <ve/scene/Components.hpp>
#include <ve/scene/Scene.hpp>
#include <vfx/Atmosphere.hpp>
#include <vfx/Weather.hpp>

#include <glm/gtc/constants.hpp>

#include <cmath>

namespace savanna {

DayNightCycle::DayNightCycle() : GameObject("DayNight")
{
    // Lights are components on child objects; we aim them each frame.
    m_sunObject = nullptr;
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
    m_sunObject = &scene().spawn("Sun");
    m_sun = &m_sunObject->addComponent<ve::DirectionalLight>();
    m_sun->castShadows = true;

    m_moonObject = &scene().spawn("Moon");
    m_moon = &m_moonObject->addComponent<ve::DirectionalLight>();
    m_moon->castShadows = true;
    m_moon->color = {0.55f, 0.65f, 1.0f};

    if (auto* atmo = renderer().graph().findPass<vfx::AtmospherePass>()) {
        atmo->sun = m_sun;
        atmo->moon = m_moon;
    }
}

void DayNightCycle::update(float dt)
{
    ve::Input& in = input();
    float speed = 24.0f / (dayLengthMinutes * 60.0f); // hours per second
    if (in.keyDown(ve::Key::T)) speed *= 40.0f;        // fast forward
    if (in.keyDown(ve::Key::G)) speed *= -40.0f;       // rewind
    timeOfDay = std::fmod(timeOfDay + speed * dt + 24.0f, 24.0f);

    m_towardsSun = sunDirection(timeOfDay);
    // The moon roughly opposite the sun, offset so it isn't exactly in line.
    const glm::vec3 towardsMoon = glm::normalize(-m_towardsSun + glm::vec3(0.25f, 0.35f, -0.2f));

    m_sunObject->transform().setRotation(glm::quatLookAt(-m_towardsSun, glm::vec3(0, 1, 0)));
    m_moonObject->transform().setRotation(glm::quatLookAt(-towardsMoon, glm::vec3(0, 1, 0)));

    // Sunlight colour = what survives the atmosphere: white at noon, deep orange at sunset.
    const float haze = weather ? weather->current().haze : 1.5f;
    const glm::vec3 T = vfx::AtmospherePass::sunTransmittance(m_towardsSun, haze);
    const float maxT = std::max({T.r, T.g, T.b, 1e-4f});
    const float weatherScale = weather ? weather->sunScale() : 1.0f;
    m_sun->color = T / maxT;
    m_sun->intensity = sunIntensity * maxT * weatherScale;
    m_sun->enabled = m_sun->intensity > 0.002f;

    const float moonUp = glm::smoothstep(-0.05f, 0.15f, towardsMoon.y);
    const float night = glm::smoothstep(0.08f, -0.12f, m_towardsSun.y);
    m_moon->intensity = moonIntensity * moonUp * night * (0.4f + 0.6f * weatherScale);
    m_moon->enabled = m_moon->intensity > 0.002f;

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

} // namespace savanna
