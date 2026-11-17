#include "CameraController.hpp"

#include <ve/core/Engine.hpp>
#include <ve/core/Input.hpp>
#include <ve/core/Time.hpp>
#include <ve/scene/Components.hpp>

#include <imgui.h>

#include <algorithm>

using ve::Key;

CameraController::CameraController() : GameObject("Camera")
{
    addComponent<ve::Camera>(); // the renderer draws from the active Camera

    // Named axes: keyboard and gamepad feed the same value (rebindable at runtime).
    ve::Input& in = input();
    in.bindAxis("MoveX", Key::A, Key::D);
    in.bindAxis("MoveX", ve::GamepadAxis::LeftX);
    in.bindAxis("MoveY", Key::S, Key::W);
    in.bindAxis("MoveY", ve::GamepadAxis::LeftY, -1.0f); // stick up = forward
    in.bindAxis("LookX", ve::GamepadAxis::RightX);
    in.bindAxis("LookY", ve::GamepadAxis::RightY);
    in.bindAction("Fast", Key::LShift);
    in.bindAction("Fast", ve::GamepadButton::LeftStick);
}

void CameraController::update(float)
{
    ve::Input& in = input();
    if (in.keyPressed(Key::Escape)) engine().quit();
    if (in.keyPressed(Key::Tab)) in.setMouseCaptured(!in.mouseCaptured());

    if (in.mouseCaptured() || in.mouseDown(ve::MouseButton::Right)) {
        const glm::vec2 d = in.mouseDelta() * sensitivity;
        m_yaw -= d.x;
        m_pitch = std::clamp(m_pitch - d.y, -1.5f, 1.5f);
    }
    const float stick = 2.5f * time().unscaledDeltaTime; // gamepad look, rad/s at full tilt
    m_yaw -= in.axisValue("LookX") * stick;
    m_pitch = std::clamp(m_pitch - in.axisValue("LookY") * stick, -1.5f, 1.5f);
    transform().setRotation(glm::quat(glm::vec3(m_pitch, m_yaw, 0.0f)));

    // Real (unscaled) time so the camera still moves when the game is paused.
    const float dt = time().unscaledDeltaTime;
    glm::vec3 move = transform().forward() * in.axisValue("MoveY") + transform().right() * in.axisValue("MoveX");
    move.y += in.axis(Key::LCtrl, Key::Space);
    transform().translate(move * speed * (in.actionDown("Fast") ? 4.0f : 1.0f) * dt);
}

void CameraController::onGui()
{
    // Tiny HUD. Any GameObject/Component can draw ImGui in onGui().
    ImGui::SetNextWindowPos({10, 10}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.4f);
    ImGui::Begin("##help", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs);
    ImGui::TextUnformatted("RMB/Tab look  WASD move  Space/Ctrl up/down  Arrows move sun  F1 engine overlay  Esc quit");
    ImGui::End();
}
