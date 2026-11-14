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
    transform().setRotation(glm::quat(glm::vec3(m_pitch, m_yaw, 0.0f)));

    // Real (unscaled) time so the camera still moves when the game is paused.
    const float dt = time().unscaledDeltaTime;
    glm::vec3 move = transform().forward() * in.axis(Key::S, Key::W) + transform().right() * in.axis(Key::A, Key::D);
    move.y += in.axis(Key::LCtrl, Key::Space);
    transform().translate(move * speed * (in.keyDown(Key::LShift) ? 4.0f : 1.0f) * dt);
}

void CameraController::onGui()
{
    // Tiny HUD. Any GameObject/Component can draw ImGui in onGui().
    ImGui::SetNextWindowPos({10, 10}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.4f);
    ImGui::Begin("##help", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs);
    ImGui::TextUnformatted("RMB/Tab look  WASD move  Space/Ctrl up/down  F1 engine overlay  Esc quit");
    ImGui::End();
}
