#pragma once

// =============================================================================
// Interaction - "look at something up close and press Use" (F4).
//
// The engine finds what the player is looking at and fires the events; the
// game decides what using means and how prompts are shown.
//
//   // The thing: a component with a prompt and what happens on use.
//   auto& i = door.addComponent<ve::Interactable>();
//   i.prompt = "Open door";
//   i.onUse = [&](ve::GameObject& user) { door.open(); };
//
//   // The player: an Interactor (uses the main camera's view by default).
//   auto& who = player.addComponent<ve::Interactor>();
//   who.action = "Use";                         // an Input action name (bindAction)
//   ...
//   hud.prompt = who.prompt();                  // "" when nothing is in focus
//
// Focus: the object hit by a ray from the eye (an Interactable on it or on a
// parent of the collider's object), else the best Interactable inside a view
// cone within reach that nothing blocks (things without colliders, e.g. a
// floating pickup). For dynamic prompts override prompt / use in a subclass
// or set promptFn.
// =============================================================================

#include "ve/scene/Components.hpp"

#include <glm/glm.hpp>

#include <functional>
#include <string>

namespace ve {

class Interactable : public Component {
public:
    std::string prompt = "Use";        // shown while in focus ("" = nothing to do right now)
    float reach = 3.0f;                // metres from the eye
    glm::vec3 focusOffset{0.0f};       // local point aimed at for the cone test (the object's origin + this)
    std::function<std::string()> promptFn;           // overrides `prompt` when set
    std::function<void(GameObject& user)> onUse;
    std::function<void(bool focused)> onFocus;

    virtual std::string currentPrompt() const { return promptFn ? promptFn() : prompt; }
    virtual void use(GameObject& user)
    {
        if (onUse) onUse(user);
    }
    virtual void focusChanged(bool focused)
    {
        if (onFocus) onFocus(focused);
    }
    glm::vec3 focusPoint() const;
};

class Interactor : public Component {
public:
    std::string action = "Use";   // Input action that uses the focus ("" = call interact() yourself)
    float maxReach = 3.0f;        // never further than this (each Interactable has its own reach too)
    float coneDegrees = 25.0f;    // fallback focus for objects without colliders
    bool useMainCamera = true;    // the eye is the main camera; otherwise call setView() every frame

    void update(float dt) override;
    void setView(const glm::vec3& eye, const glm::vec3& direction);

    Interactable* focus() const;  // null if nothing (or the focus was destroyed)
    std::string prompt() const;   // the focus' prompt, "" if none
    bool interact();              // uses the focus; false if nothing was in focus

private:
    Interactable* findFocus() const;
    glm::vec3 m_eye{0.0f}, m_dir{0.0f, 0.0f, -1.0f};
    uint64_t m_focusObject = 0;   // the focused Interactable's object (it may be destroyed)
    Interactable* m_focus = nullptr;
};

} // namespace ve
