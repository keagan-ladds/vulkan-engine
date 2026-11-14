#pragma once

// =============================================================================
// Input - keyboard & mouse state with per-frame edges and named actions.
//
//   if (input().keyDown(Key::W)) ...            held this frame
//   if (input().keyPressed(Key::Space)) ...     went down this frame
//   input().bindAction("Jump", Key::Space);
//   if (input().actionPressed("Jump")) ...
//   glm::vec2 look = input().mouseDelta();      pixels moved this frame
//
// "Pressed"/"Released" edges are per *rendered frame*, so read them in
// update(), not fixedUpdate() (which can run 0 or several times per frame).
// When the UI (ImGui) wants the keyboard/mouse, game queries return false.
// =============================================================================

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

union SDL_Event;

namespace ve {

// Values are USB-HID scancodes, which is exactly what SDL_Scancode uses, so
// conversion is a cast. Scancodes are layout independent: Key::W is "the key
// where W is on a QWERTY keyboard", which is what you want for WASD movement.
enum class Key : uint16_t {
    Unknown = 0,
    A = 4, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num1 = 30, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9, Num0,
    Enter = 40, Escape = 41, Backspace = 42, Tab = 43, Space = 44, Minus = 45, Equals = 46,
    F1 = 58, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Right = 79, Left = 80, Down = 81, Up = 82,
    LCtrl = 224, LShift = 225, LAlt = 226, RCtrl = 228, RShift = 229, RAlt = 230,
};

enum class MouseButton : uint8_t { Left = 1, Middle = 2, Right = 3, X1 = 4, X2 = 5 };

class Input {
public:
    static constexpr size_t kKeyCount = 512;

    // Engine-internal: called once per frame before events, then per event.
    void beginFrame();
    void processEvent(const SDL_Event& event);
    void setUiCapture(bool keyboard, bool mouse);

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

    // Captured = cursor hidden and locked, unlimited relative motion (FPS camera).
    void setMouseCaptured(bool captured);
    bool mouseCaptured() const { return m_mouseCaptured; }

    // --- Actions --------------------------------------------------------------
    // An action can be bound to several keys/buttons; it is "down" if any is.
    void bindAction(const std::string& action, Key key);
    void bindAction(const std::string& action, MouseButton button);
    void clearAction(const std::string& action);
    bool actionDown(const std::string& action) const;
    bool actionPressed(const std::string& action) const;
    bool actionReleased(const std::string& action) const;

private:
    struct Binding {
        std::vector<Key> keys;
        std::vector<MouseButton> buttons;
    };

    std::array<bool, kKeyCount> m_keys{};
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
};

} // namespace ve
