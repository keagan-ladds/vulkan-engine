#include "ve/core/Input.hpp"

#include <SDL.h>

#include <algorithm>
#include <cmath>

namespace ve {

namespace {
const char* kPadNames[] = {"A", "B", "X", "Y", "Back", "Guide", "Start", "Left Stick", "Right Stick", "LB", "RB",
                           "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right"};
const char* kAxisNames[] = {"Left Stick X", "Left Stick Y", "Right Stick X", "Right Stick Y", "LT", "RT"};
const char* kMouseNames[] = {"", "Mouse Left", "Mouse Middle", "Mouse Right", "Mouse X1", "Mouse X2"};
} // namespace

void Input::beginFrame()
{
    // Remember last frame's state so we can detect "pressed this frame" edges.
    m_prevKeys = m_keys;
    m_prevButtons = m_buttons;
    m_prevPadButtons = m_padButtons;
    m_prevPadAxes = m_padAxes;
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
            if (e.type == SDL_KEYDOWN && !e.key.repeat && !m_rebind.empty()) finishRebind(Key(sc), MouseButton(0), GamepadButton::Count);
        }
        break;
    }
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (e.button.button < m_buttons.size()) {
            m_buttons[e.button.button] = (e.type == SDL_MOUSEBUTTONDOWN);
            if (e.type == SDL_MOUSEBUTTONDOWN && !m_rebind.empty()) finishRebind(Key::Unknown, MouseButton(e.button.button), GamepadButton::Count);
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
    case SDL_CONTROLLERDEVICEADDED:
        if (!m_pad) openGamepad(e.cdevice.which);
        break;
    case SDL_CONTROLLERDEVICEREMOVED:
        if (m_pad && e.cdevice.which == m_padInstance) {
            SDL_GameControllerClose(m_pad);
            m_pad = nullptr;
            m_padInstance = -1;
            m_padButtons.fill(false);
            m_padAxes.fill(0.0f);
            // Another one plugged in? Take it.
            for (int i = 0; i < SDL_NumJoysticks() && !m_pad; ++i)
                if (SDL_IsGameController(i)) openGamepad(i);
        }
        break;
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
        if (e.cbutton.which == m_padInstance && e.cbutton.button < m_padButtons.size()) {
            const bool down = e.type == SDL_CONTROLLERBUTTONDOWN;
            m_padButtons[e.cbutton.button] = down;
            if (down && !m_rebind.empty()) finishRebind(Key::Unknown, MouseButton(0), GamepadButton(e.cbutton.button));
        }
        break;
    case SDL_CONTROLLERAXISMOTION:
        if (e.caxis.which == m_padInstance && e.caxis.axis < m_padAxes.size())
            m_padAxes[e.caxis.axis] = std::max(-1.0f, float(e.caxis.value) / 32767.0f);
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

void Input::setUiCapture(bool keyboard, bool mouse, bool force)
{
    // While the mouse is captured for gameplay the UI never gets it (unless forced: a modal menu).
    m_uiKeyboard = keyboard && (!m_mouseCaptured || force);
    m_uiMouse = mouse && (!m_mouseCaptured || force);
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

namespace {
template <class V, class T>
void addOnce(V& v, const T& x)
{
    if (std::find(v.begin(), v.end(), x) == v.end()) v.push_back(x);
}
} // namespace

void Input::bindAction(const std::string& action, Key key)
{
    addOnce(m_actions[action].keys, key);
}

void Input::bindAction(const std::string& action, MouseButton button)
{
    addOnce(m_actions[action].buttons, button);
}

void Input::clearAction(const std::string& action)
{
    m_actions.erase(action);
}

bool Input::actionDown(const std::string& action) const
{
    const auto it = m_actions.find(action);
    if (it == m_actions.end() || !m_rebind.empty()) return false;
    for (Key k : it->second.keys) if (keyDown(k)) return true;
    for (MouseButton b : it->second.buttons) if (mouseDown(b)) return true;
    for (GamepadButton b : it->second.pad) if (gamepadDown(b)) return true;
    for (GamepadAxis a : it->second.triggers) if (triggerDown(a, false)) return true;
    return false;
}

bool Input::actionPressed(const std::string& action) const
{
    const auto it = m_actions.find(action);
    if (it == m_actions.end() || !m_rebind.empty()) return false;
    for (Key k : it->second.keys) if (keyPressed(k)) return true;
    for (MouseButton b : it->second.buttons) if (mousePressed(b)) return true;
    for (GamepadButton b : it->second.pad) if (gamepadPressed(b)) return true;
    for (GamepadAxis a : it->second.triggers) if (triggerDown(a, false) && !triggerDown(a, true)) return true;
    return false;
}

bool Input::actionReleased(const std::string& action) const
{
    const auto it = m_actions.find(action);
    if (it == m_actions.end()) return false;
    for (Key k : it->second.keys) if (keyReleased(k)) return true;
    for (MouseButton b : it->second.buttons) if (mouseReleased(b)) return true;
    for (GamepadButton b : it->second.pad) if (!gamepadDown(b) && m_prevPadButtons[size_t(b)]) return true;
    for (GamepadAxis a : it->second.triggers) if (!triggerDown(a, false) && triggerDown(a, true)) return true;
    return false;
}

void Input::bindAction(const std::string& action, GamepadButton button)
{
    addOnce(m_actions[action].pad, button);
}

void Input::bindAction(const std::string& action, GamepadAxis trigger)
{
    addOnce(m_actions[action].triggers, trigger);
}

bool Input::triggerDown(GamepadAxis a, bool previous) const
{
    const float v = (previous ? m_prevPadAxes : m_padAxes)[size_t(a)];
    return !m_uiKeyboard && m_pad && std::abs(v) > triggerThreshold;
}

// --- Gamepad --------------------------------------------------------------------

void Input::openGamepad(int deviceIndex)
{
    m_pad = SDL_GameControllerOpen(deviceIndex);
    m_padInstance = m_pad ? SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(m_pad)) : -1;
    m_padButtons.fill(false);
    m_padAxes.fill(0.0f);
}

std::string Input::gamepadName() const
{
    const char* n = m_pad ? SDL_GameControllerName(m_pad) : nullptr;
    return n ? n : "";
}

bool Input::gamepadDown(GamepadButton b) const
{
    return !m_uiKeyboard && m_pad && b < GamepadButton::Count && m_padButtons[size_t(b)];
}

bool Input::gamepadPressed(GamepadButton b) const
{
    return !m_uiKeyboard && m_pad && b < GamepadButton::Count && m_padButtons[size_t(b)] && !m_prevPadButtons[size_t(b)];
}

float Input::gamepadAxis(GamepadAxis a) const
{
    if (m_uiKeyboard || !m_pad || a >= GamepadAxis::Count) return 0.0f;
    if (a == GamepadAxis::LeftTrigger || a == GamepadAxis::RightTrigger) return std::clamp(m_padAxes[size_t(a)], 0.0f, 1.0f);
    // Radial dead zone over the stick's two axes, rescaled to start at 0.
    const size_t x = (a == GamepadAxis::LeftX || a == GamepadAxis::LeftY) ? 0 : 2;
    const glm::vec2 v(m_padAxes[x], m_padAxes[x + 1]);
    const float len = glm::length(v);
    if (len <= stickDeadZone) return 0.0f;
    const float k = std::min(1.0f, (len - stickDeadZone) / (1.0f - stickDeadZone)) / len;
    return m_padAxes[size_t(a)] * k;
}

bool Input::attachVirtualGamepad()
{
    if (m_virtual) return true;
    // A scripted pad has no window to focus (headless tests): SDL would drop its input otherwise.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_VirtualJoystickDesc desc{};
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    desc.name = "Virtual gamepad";
    m_virtualIndex = SDL_JoystickAttachVirtualEx(&desc);
    if (m_virtualIndex < 0) return false;
    m_virtual = SDL_JoystickOpen(m_virtualIndex);
    if (!m_pad && SDL_IsGameController(m_virtualIndex)) openGamepad(m_virtualIndex);
    return m_virtual != nullptr;
}

void Input::setVirtualGamepadButton(GamepadButton button, bool down)
{
    if (m_virtual) SDL_JoystickSetVirtualButton(m_virtual, int(button), down ? 1 : 0);
}

void Input::setVirtualGamepadAxis(GamepadAxis axis, float value)
{
    if (m_virtual) SDL_JoystickSetVirtualAxis(m_virtual, int(axis), Sint16(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
}

// --- Axes -------------------------------------------------------------------------

void Input::bindAxis(const std::string& axis, Key negative, Key positive)
{
    addOnce(m_axes[axis].keys, std::pair{negative, positive});
}

void Input::bindAxis(const std::string& axis, GamepadAxis padAxis, float scale)
{
    addOnce(m_axes[axis].pad, std::pair{padAxis, scale});
}

void Input::clearAxis(const std::string& axis)
{
    m_axes.erase(axis);
}

float Input::axisValue(const std::string& name) const
{
    const auto it = m_axes.find(name);
    if (it == m_axes.end()) return 0.0f;
    float best = 0.0f;
    auto take = [&](float v) {
        if (std::abs(v) > std::abs(best)) best = v;
    };
    for (const auto& [neg, pos] : it->second.keys) take(axis(neg, pos));
    for (const auto& [a, scale] : it->second.pad) take(gamepadAxis(a) * scale);
    return std::clamp(best, -1.0f, 1.0f);
}

// --- Rebinding / settings ------------------------------------------------------------

std::vector<std::string> Input::actions() const
{
    std::vector<std::string> names;
    for (const auto& [n, b] : m_actions) names.push_back(n);
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> Input::bindingNames(const std::string& action) const
{
    std::vector<std::string> out;
    const auto it = m_actions.find(action);
    if (it == m_actions.end()) return out;
    for (Key k : it->second.keys) out.push_back(SDL_GetScancodeName(SDL_Scancode(k)));
    for (MouseButton b : it->second.buttons) out.push_back(size_t(b) < std::size(kMouseNames) ? kMouseNames[size_t(b)] : "Mouse ?");
    for (GamepadButton b : it->second.pad) out.push_back(std::string("Pad ") + (b < GamepadButton::Count ? kPadNames[size_t(b)] : "?"));
    for (GamepadAxis a : it->second.triggers) out.push_back(std::string("Pad ") + (a < GamepadAxis::Count ? kAxisNames[size_t(a)] : "?"));
    return out;
}

void Input::startRebind(const std::string& action)
{
    m_rebind = action;
}

void Input::finishRebind(Key key, MouseButton button, GamepadButton pad)
{
    if (key == Key::Escape) { // cancel
        m_rebind.clear();
        return;
    }
    Binding& b = m_actions[m_rebind];
    if (pad != GamepadButton::Count) {
        b.pad = {pad}; // gamepad bindings replace gamepad bindings
        b.triggers.clear();
    } else {
        b.keys.clear(); // keyboard / mouse bindings replace keyboard / mouse bindings
        b.buttons.clear();
        if (key != Key::Unknown) b.keys.push_back(key);
        if (button != MouseButton(0)) b.buttons.push_back(button);
    }
    m_rebind.clear();
}

Json Input::saveBindings() const
{
    Json actions = Json::object(), axes = Json::object();
    for (const auto& [name, b] : m_actions) {
        Json j;
        for (Key k : b.keys) j["keys"].push_back(int(k));
        for (MouseButton m : b.buttons) j["mouse"].push_back(int(m));
        for (GamepadButton p : b.pad) j["pad"].push_back(int(p));
        for (GamepadAxis t : b.triggers) j["triggers"].push_back(int(t));
        actions[name] = j.is_null() ? Json::object() : j;
    }
    for (const auto& [name, a] : m_axes) {
        Json j = Json::object();
        for (const auto& [n, p] : a.keys) j["keys"].push_back({int(n), int(p)});
        for (const auto& [ax, s] : a.pad) j["pad"].push_back({int(ax), s});
        axes[name] = j;
    }
    return Json{{"actions", actions}, {"axes", axes}};
}

void Input::loadBindings(const Json& json)
{
    if (!json.is_object()) return;
    const Json actions = json.value("actions", Json::object());
    for (auto it = actions.begin(); it != actions.end(); ++it) {
        Binding b;
        const Json& j = it.value();
        for (const Json& v : j.value("keys", Json::array())) b.keys.push_back(Key(v.get<int>()));
        for (const Json& v : j.value("mouse", Json::array())) b.buttons.push_back(MouseButton(v.get<int>()));
        for (const Json& v : j.value("pad", Json::array())) b.pad.push_back(GamepadButton(v.get<int>()));
        for (const Json& v : j.value("triggers", Json::array())) b.triggers.push_back(GamepadAxis(v.get<int>()));
        m_actions[it.key()] = std::move(b);
    }
    const Json axes = json.value("axes", Json::object());
    for (auto it = axes.begin(); it != axes.end(); ++it) {
        AxisBinding a;
        for (const Json& v : it.value().value("keys", Json::array())) a.keys.push_back({Key(v[0].get<int>()), Key(v[1].get<int>())});
        for (const Json& v : it.value().value("pad", Json::array())) a.pad.push_back({GamepadAxis(v[0].get<int>()), v[1].get<float>()});
        m_axes[it.key()] = std::move(a);
    }
}

} // namespace ve
