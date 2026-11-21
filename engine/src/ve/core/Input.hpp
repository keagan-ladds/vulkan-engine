#pragma once

// =============================================================================
// Input - keyboard, mouse and gamepad state with per-frame edges, named
// actions (buttons) and named axes (analog), rebindable at runtime.
//
//   if (input().keyDown(Key::W)) ...            held this frame
//   if (input().keyPressed(Key::Space)) ...     went down this frame
//   input().bindAction("Jump", Key::Space);
//   input().bindAction("Jump", GamepadButton::A);
//   if (input().actionPressed("Jump")) ...
//   input().bindAxis("MoveY", Key::S, Key::W);          // keys: -1 / 0 / +1
//   input().bindAxis("MoveY", GamepadAxis::LeftY, -1);  // stick (up = forward)
//   float forward = input().axisValue("MoveY");          // the strongest source
//   glm::vec2 look = input().mouseDelta();      pixels moved this frame
//
// Gamepad: the first connected controller (hot-plugged), SDL's standard
// layout (Xbox names). Sticks have a radial dead zone; triggers can drive
// actions (bindAction(action, GamepadAxis::RightTrigger)).
// Rebinding: startRebind("Jump") -> the next key / mouse button / gamepad
// button pressed becomes the binding for that kind of device; bindings
// round-trip through JSON (saveBindings / loadBindings) for a settings file.
//
// "Pressed"/"Released" edges are per *rendered frame*, so read them in
// update(), not fixedUpdate() (which can run 0 or several times per frame).
// When the UI (ImGui) wants the keyboard/mouse, game queries return false.
// =============================================================================

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

union SDL_Event;
struct _SDL_GameController;
struct _SDL_Joystick;

namespace ve {

using Json = nlohmann::json;

// Values are USB-HID scancodes, which is exactly what SDL_Scancode uses, so
// conversion is a cast. Scancodes are layout independent: Key::W is "the key
// where W is on a QWERTY keyboard", which is what you want for WASD movement.
enum class Key : uint16_t {
    Unknown = 0,
    A = 4, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num1 = 30, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9, Num0,
    Enter = 40, Escape = 41, Backspace = 42, Tab = 43, Space = 44, Minus = 45, Equals = 46,
    F1 = 58, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Home = 74, PageUp = 75, Delete = 76, End = 77, PageDown = 78,
    Right = 79, Left = 80, Down = 81, Up = 82,
    LCtrl = 224, LShift = 225, LAlt = 226, RCtrl = 228, RShift = 229, RAlt = 230,
};

enum class MouseButton : uint8_t { Left = 1, Middle = 2, Right = 3, X1 = 4, X2 = 5 };

// SDL_GameControllerButton / Axis order.
enum class GamepadButton : uint8_t {
    A, B, X, Y, Back, Guide, Start, LeftStick, RightStick, LeftShoulder, RightShoulder,
    DPadUp, DPadDown, DPadLeft, DPadRight, Count
};
enum class GamepadAxis : uint8_t { LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger, Count };

class Input {
public:
    static constexpr size_t kKeyCount = 512;

    // Engine-internal: called once per frame before events, then per event.
    void beginFrame();
    void processEvent(const SDL_Event& event);
    // UI capture: keys / pad and / or the mouse go to the UI, not gameplay.
    // force: even while the mouse is captured for gameplay (a modal game menu).
    void setUiCapture(bool keyboard, bool mouse, bool force = false);

    // --- Keyboard -------------------------------------------------------------
    bool keyDown(Key key) const;
    bool keyPressed(Key key) const;
    bool keyReleased(Key key) const;
    // -1, 0 or +1 from two keys, e.g. axis(Key::S, Key::W) for forward/back.
    float axis(Key negative, Key positive) const;

    // --- Mouse ----------------------------------------------------------------
    bool mouseDown(MouseButton button) const;
    bool mousePressed(MouseButton button) const;
    bool mouseReleased(MouseButton button) const;
    glm::vec2 mouseDelta() const;    // pixels moved this frame
    glm::vec2 mousePosition() const; // window pixels
    float wheel() const;             // scroll this frame (+ = away from user)

    // --- Text (UI text fields; not affected by UI capture) ---------------------------------
    const std::string& textTyped() const { return m_text; } // UTF-8 typed this frame (layout + IME aware)
    bool keyTyped(Key key) const;    // pressed or auto-repeated this frame (editing keys: Backspace, arrows...)
    void setTextInput(bool on);      // the OS text input (IME, on-screen keyboards) while a text field is edited
    bool textInputActive() const { return m_textInput; }
    // Tests: typed next frame (as if from the keyboard).
    void injectText(const std::string& utf8) { m_injectedText += utf8; }
    void injectKeyTyped(Key key) { m_injectedKeys.push_back(uint16_t(key)); }

    // Captured = cursor hidden and locked, unlimited relative motion (FPS camera).
    void setMouseCaptured(bool captured);
    bool mouseCaptured() const { return m_mouseCaptured; }

    // --- Gamepad --------------------------------------------------------------
    bool gamepadConnected() const { return m_pad != nullptr; }
    std::string gamepadName() const;
    bool gamepadDown(GamepadButton button) const;
    bool gamepadPressed(GamepadButton button) const;
    float gamepadAxis(GamepadAxis axis) const; // sticks -1..1 (dead zone applied, +y = down), triggers 0..1
    float stickDeadZone = 0.18f;
    float triggerThreshold = 0.5f;             // a trigger bound to an action is "down" past this

    // Tests / tools: a virtual gamepad (SDL), driven from code.
    bool attachVirtualGamepad();
    void setVirtualGamepadButton(GamepadButton button, bool down);
    void setVirtualGamepadAxis(GamepadAxis axis, float value); // -1..1 (triggers 0..1)

    // --- Actions --------------------------------------------------------------
    // An action can be bound to several keys/buttons; it is "down" if any is.
    void bindAction(const std::string& action, Key key);
    void bindAction(const std::string& action, MouseButton button);
    void bindAction(const std::string& action, GamepadButton button);
    void bindAction(const std::string& action, GamepadAxis trigger); // past triggerThreshold
    void clearAction(const std::string& action);
    bool actionDown(const std::string& action) const;
    bool actionPressed(const std::string& action) const;
    bool actionReleased(const std::string& action) const;

    // --- Axes (analog) ----------------------------------------------------------
    void bindAxis(const std::string& axis, Key negative, Key positive);
    void bindAxis(const std::string& axis, GamepadAxis padAxis, float scale = 1.0f);
    void clearAxis(const std::string& axis);
    float axisValue(const std::string& axis) const; // -1..1: the strongest of its sources

    // --- Rebinding / settings ----------------------------------------------------
    std::vector<std::string> actions() const;             // sorted
    std::vector<std::string> bindingNames(const std::string& action) const; // "E", "Mouse Left", "Pad A"...
    std::string bindingText(const std::string& action, std::string_view sep = ", ") const; // bindingNames joined
    // The next key / mouse button (or gamepad button) pressed replaces the
    // action's keyboard+mouse (or gamepad) bindings. Escape cancels.
    void startRebind(const std::string& action);
    void cancelRebind() { m_rebind.clear(); }
    const std::string& rebinding() const { return m_rebind; }
    Json saveBindings() const;
    void loadBindings(const Json& json); // replaces the actions / axes it lists

private:
    struct Binding {
        std::vector<Key> keys;
        std::vector<MouseButton> buttons;
        std::vector<GamepadButton> pad;
        std::vector<GamepadAxis> triggers;
    };
    struct AxisBinding {
        std::vector<std::pair<Key, Key>> keys;
        std::vector<std::pair<GamepadAxis, float>> pad;
    };
    bool triggerDown(GamepadAxis a, bool previous) const;
    void openGamepad(int deviceIndex);
    void finishRebind(Key key, MouseButton button, GamepadButton pad);

    std::array<bool, kKeyCount> m_keys{};
    std::string m_text;               // typed this frame
    std::vector<uint16_t> m_typed;    // key-down events this frame, repeats included
    std::string m_injectedText;
    std::vector<uint16_t> m_injectedKeys;
    bool m_textInput = false;
    std::array<bool, kKeyCount> m_prevKeys{};
    std::array<bool, 8> m_buttons{};
    std::array<bool, 8> m_prevButtons{};
    glm::vec2 m_mouseDelta{0.0f};
    glm::vec2 m_mousePos{0.0f};
    float m_wheel = 0.0f;
    bool m_mouseCaptured = false;
    bool m_uiKeyboard = false;
    bool m_uiMouse = false;
    std::unordered_map<std::string, Binding> m_actions;
    std::unordered_map<std::string, AxisBinding> m_axes;
    std::string m_rebind;

    ::_SDL_GameController* m_pad = nullptr;
    int m_padInstance = -1;
    std::array<bool, size_t(GamepadButton::Count)> m_padButtons{}, m_prevPadButtons{};
    std::array<float, size_t(GamepadAxis::Count)> m_padAxes{}, m_prevPadAxes{};
    int m_virtualIndex = -1;
    ::_SDL_Joystick* m_virtual = nullptr;
};

} // namespace ve
