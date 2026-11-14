#pragma once

// A free-flying camera: an example GameObject with behaviour.
//   Right mouse (hold) or Tab (toggle) = look, WASD = move, Space/Ctrl = up/down,
//   Shift = fast, Esc = quit.

#include <ve/scene/GameObject.hpp>

class CameraController : public ve::GameObject {
public:
    CameraController();
    void update(float dt) override;
    void onGui() override;

    float speed = 6.0f;
    float sensitivity = 0.0025f;

private:
    float m_yaw = 0.0f;
    float m_pitch = -0.15f;
};
