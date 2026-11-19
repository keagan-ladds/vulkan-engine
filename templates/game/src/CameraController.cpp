#include "CameraController.hpp"

#include <ve/core/Engine.hpp>
#include <ve/core/Input.hpp>
#include <ve/core/Time.hpp>
#include <ve/scene/Components.hpp>

#include <ve/ui/Gui.hpp>

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

void CameraController::start()
{
    // A tiny HUD on the engine's UI elements (ve/ui/Gui.hpp): a canvas laid
    // out on 1920x1080 reference units, anchored to the top-left corner.
    // Load your own fonts with ve::ui::loadFont; colours are sRGB hex.
    auto& canvas = addComponent<ve::ui::Canvas>();
    auto& strip = canvas.root().add<ve::ui::Panel>("help");
    strip.at({16.0f, 16.0f}).size({1010.0f, 40.0f});
    strip.color = ve::ui::rgba(0x00000066);
    strip.rounding = 6.0f;
    auto& text = strip.add<ve::ui::Label>("RMB/Tab look   WASD move   Space/Ctrl up/down   Arrows move sun   F1 engine overlay   Esc quit", "text");
    text.fill({14.0f, 0.0f, 14.0f, 0.0f});
    text.fontSize = 20.0f;
}
