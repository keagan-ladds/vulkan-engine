#pragma once

// An example reusable Component: spins and bobs whatever it's attached to.
//   go.addComponent<Spinner>().speed = 2.0f;

#include <ve/scene/GameObject.hpp>

#include <cmath>

class Spinner : public ve::Component {
public:
    float speed = 1.0f;       // radians per second
    float bobHeight = 0.25f;  // meters

    void start() override { m_base = transform().position(); }

    void update(float dt) override
    {
        m_t += dt;
        transform().rotate({0.0f, 1.0f, 0.0f}, speed * dt);
        transform().setPosition(m_base + glm::vec3(0.0f, std::sin(m_t * 2.0f) * bobHeight, 0.0f));
    }

private:
    glm::vec3 m_base{0.0f};
    float m_t = 0.0f;
};
