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
#include "ve/scene/ObjectRef.hpp"

#include <glm/glm.hpp>

#include <functional>
#include <string>

namespace ve {

/// Makes its object usable ("press E to ..."): an Interactor focuses it when
/// looked at within reach, shows its prompt, and calls use() on the action.
/// Override use() / currentPrompt() in a subclass, or set the callbacks.
class Interactable : public Component {
public:
    std::string prompt = "Use";        ///< shown while in focus ("" = nothing to do right now)
    float reach = 3.0f;                ///< metres from the eye
    glm::vec3 focusOffset{0.0f};       ///< local point aimed at for the cone test (the object's origin + this)
    std::function<std::string()> promptFn;           ///< overrides `prompt` when set
    std::function<void(GameObject& user)> onUse;  ///< Called when used (`user` = the object that has the Interactor, e.g. the player).
    std::function<void(bool focused)> onFocus;  ///< Called when it gains (true) or loses (false) focus: highlight it.

    /// The prompt to show now (promptFn if set, else `prompt`).
    virtual std::string currentPrompt() const { return promptFn ? promptFn() : prompt; }
    /// Uses it: calls onUse. Override for behaviour in a subclass.
    virtual void use(GameObject& user)
    {
        if (onUse) onUse(user);
    }
    /// Focus changed: calls onFocus. Override in a subclass.
    virtual void focusChanged(bool focused)
    {
        if (onFocus) onFocus(focused);
    }
    /// World point the focus cone aims at: the object's position + focusOffset.
    glm::vec3 focusPoint() const;
};

/// Finds what the player looks at (eye ray, then a cone for objects without
/// colliders), keeps it as the focus and uses it on `action`. Put it on the player.
class Interactor : public Component {
public:
    std::string action = "Use";   ///< Input action that uses the focus ("" = call interact() yourself)
    float maxReach = 3.0f;        ///< never further than this (each Interactable has its own reach too)
    float coneDegrees = 25.0f;    ///< fallback focus for objects without colliders
    uint32_t layers = 0xFFFFFFFFu; ///< collision layers the eye ray sees (PhysicsWorld::layerMask): e.g. not loose debris
    bool useMainCamera = true;    ///< the eye is the main camera; otherwise call setView() every frame

    void update(float dt) override;
    /// Sets the eye ray for this frame (when useMainCamera is false).
    void setView(const glm::vec3& eye, const glm::vec3& direction);

    Interactable* focus() const;  ///< null if nothing (or the focus was destroyed)
    std::string prompt() const;   ///< the focus' prompt, "" if none
    bool interact();              ///< uses the focus; false if nothing was in focus

private:
    Interactable* findFocus() const;
    glm::vec3 m_eye{0.0f}, m_dir{0.0f, 0.0f, -1.0f};
    ComponentRef<Interactable> m_focus; ///< it (or its object) may be destroyed
};

class TypeRegistry;
/// Reflection (scene files, editor): this module's components. The Engine calls it at startup.
void registerInteractionTypes(TypeRegistry& r);

} // namespace ve
