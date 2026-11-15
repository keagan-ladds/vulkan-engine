#pragma once

// The sun as a GameObject: a DirectionalLight you can move with the keyboard.
//   Left/Right arrows = direction (azimuth), Up/Down arrows = height (elevation).
// Its colour comes from the atmosphere (warm and dim near the horizon) and it
// fades out below the horizon. Header-only for brevity.

#include <ve/core/Input.hpp>
#include <ve/core/Time.hpp>
#include <ve/scene/Components.hpp>
#include <ve/scene/GameObject.hpp>
#include <vfx/Atmosphere.hpp>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

class SunController : public ve::GameObject {
public:
    SunController() : GameObject("Sun") { m_light = &addComponent<ve::DirectionalLight>(); }

    float azimuthDegrees = 220.0f;  // compass direction the sunlight comes from (0 = +X, 90 = +Z)
    float elevationDegrees = 20.0f; // height above the horizon
    float intensity = 3.0f;
    float turnSpeed = 60.0f;        // degrees per second while a key is held

    ve::DirectionalLight& light() { return *m_light; }

    void start() override { apply(); }

    void update(float) override
    {
        ve::Input& in = input();
        const float dt = time().unscaledDeltaTime; // works while paused too
        azimuthDegrees += in.axis(ve::Key::Left, ve::Key::Right) * turnSpeed * dt;
        elevationDegrees = std::clamp(elevationDegrees + in.axis(ve::Key::Down, ve::Key::Up) * turnSpeed * 0.5f * dt,
                                      -15.0f, 89.0f);
        apply();
    }

private:
    void apply()
    {
        const float az = glm::radians(azimuthDegrees), el = glm::radians(elevationDegrees);
        const glm::vec3 towardsSun(std::cos(el) * std::cos(az), std::sin(el), std::cos(el) * std::sin(az));
        transform().setRotation(glm::quatLookAt(-towardsSun, glm::abs(towardsSun.y) > 0.99f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0)));
        // Sunlight after the atmosphere: white at noon, orange at sunset (normalised; brightness via intensity).
        const glm::vec3 t = vfx::AtmospherePass::sunTransmittance(glm::normalize(glm::vec3(towardsSun.x, std::max(towardsSun.y, 0.01f), towardsSun.z)));
        m_light->color = t / std::max({t.r, t.g, t.b, 1e-4f});
        // Fade out as it sets (the sky goes dark by itself).
        const float fade = std::clamp((elevationDegrees + 2.0f) / 6.0f, 0.0f, 1.0f);
        m_light->intensity = intensity * fade * fade;
    }

    ve::DirectionalLight* m_light = nullptr;
};
