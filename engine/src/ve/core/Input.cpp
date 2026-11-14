#include "ve/core/Input.hpp"

#include <SDL.h>

namespace ve {

void Input::beginFrame()
{
    // Remember last frame's state so we can detect "pressed this frame" edges.
    m_prevKeys = m_keys;
    m_prevButtons = m_buttons;
    m_mouseDelta = glm::vec2(0.0f);
    m_wheel = 0.0f;
}

void Input::processEvent(const SDL_Event& e)
{
    switch (e.type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        const auto sc = static_cast<size_t>(e.key.keysym.scancode);
        if (sc < kKeyCount) {
            m_keys[sc] = (e.type == SDL_KEYDOWN);
        }
        break;
    }
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (e.button.button < m_buttons.size()) {
            m_buttons[e.button.button] = (e.type == SDL_MOUSEBUTTONDOWN);
        }
        break;
    case SDL_MOUSEMOTION:
        // Accumulate: several motion events can arrive in one frame.
        m_mouseDelta += glm::vec2(static_cast<float>(e.motion.xrel), static_cast<float>(e.motion.yrel));
        m_mousePos = glm::vec2(static_cast<float>(e.motion.x), static_cast<float>(e.motion.y));
        break;
    case SDL_MOUSEWHEEL:
        m_wheel += e.wheel.preciseY;
        break;
    case SDL_WINDOWEVENT:
        // Release everything when focus is lost so keys don't get "stuck".
        if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            m_keys.fill(false);
            m_buttons.fill(false);
        }
        break;
    default:
        break;
    }
}

void Input::setUiCapture(bool keyboard, bool mouse)
{
    // While the mouse is captured for gameplay the UI never gets it.
    m_uiKeyboard = keyboard && !m_mouseCaptured;
    m_uiMouse = mouse && !m_mouseCaptured;
}

bool Input::keyDown(Key key) const
{
    const auto k = static_cast<size_t>(key);
    return !m_uiKeyboard && k < kKeyCount && m_keys[k];
}

bool Input::keyPressed(Key key) const
{
    const auto k = static_cast<size_t>(key);
    return !m_uiKeyboard && k < kKeyCount && m_keys[k] && !m_prevKeys[k];
}

bool Input::keyReleased(Key key) const
{
    const auto k = static_cast<size_t>(key);
    return !m_uiKeyboard && k < kKeyCount && !m_keys[k] && m_prevKeys[k];
}

float Input::axis(Key negative, Key positive) const
{
    return (keyDown(positive) ? 1.0f : 0.0f) - (keyDown(negative) ? 1.0f : 0.0f);
}

bool Input::mouseDown(MouseButton b) const
{
    return !m_uiMouse && m_buttons[static_cast<size_t>(b)];
}

bool Input::mousePressed(MouseButton b) const
{
    const auto i = static_cast<size_t>(b);
    return !m_uiMouse && m_buttons[i] && !m_prevButtons[i];
}

bool Input::mouseReleased(MouseButton b) const
{
    const auto i = static_cast<size_t>(b);
    return !m_uiMouse && !m_buttons[i] && m_prevButtons[i];
}

glm::vec2 Input::mouseDelta() const
{
    return m_uiMouse ? glm::vec2(0.0f) : m_mouseDelta;
}

glm::vec2 Input::mousePosition() const
{
    return m_mousePos;
}

float Input::wheel() const
{
    return m_uiMouse ? 0.0f : m_wheel;
}

void Input::setMouseCaptured(bool captured)
{
    m_mouseCaptured = captured;
    SDL_SetRelativeMouseMode(captured ? SDL_TRUE : SDL_FALSE);
}

void Input::bindAction(const std::string& action, Key key)
{
    m_actions[action].keys.push_back(key);
}

void Input::bindAction(const std::string& action, MouseButton button)
{
    m_actions[action].buttons.push_back(button);
}

void Input::clearAction(const std::string& action)
{
    m_actions.erase(action);
}

bool Input::actionDown(const std::string& action) const
{
    const auto it = m_actions.find(action);
    if (it == m_actions.end()) return false;
    for (Key k : it->second.keys) if (keyDown(k)) return true;
    for (MouseButton b : it->second.buttons) if (mouseDown(b)) return true;
    return false;
}

bool Input::actionPressed(const std::string& action) const
{
    const auto it = m_actions.find(action);
    if (it == m_actions.end()) return false;
    for (Key k : it->second.keys) if (keyPressed(k)) return true;
    for (MouseButton b : it->second.buttons) if (mousePressed(b)) return true;
    return false;
}

bool Input::actionReleased(const std::string& action) const
{
    const auto it = m_actions.find(action);
    if (it == m_actions.end()) return false;
    for (Key k : it->second.keys) if (keyReleased(k)) return true;
    for (MouseButton b : it->second.buttons) if (mouseReleased(b)) return true;
    return false;
}

} // namespace ve
