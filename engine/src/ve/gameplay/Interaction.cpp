#include "ve/gameplay/Interaction.hpp"

#include "ve/core/Engine.hpp"
#include "ve/core/Input.hpp"
#include "ve/physics/Physics.hpp"
#include "ve/scene/Scene.hpp"

#include <cmath>

namespace ve {

glm::vec3 Interactable::focusPoint() const
{
    return glm::vec3(transform().worldMatrix() * glm::vec4(focusOffset, 1.0f));
}

void Interactor::setView(const glm::vec3& eye, const glm::vec3& direction)
{
    m_eye = eye;
    m_dir = glm::length(direction) > 1e-6f ? glm::normalize(direction) : glm::vec3(0, 0, -1);
}

Interactable* Interactor::findFocus() const
{
    Scene& s = gameObject().scene();
    QueryFilter filter;
    filter.layers = layers;
    // 1. Whatever the ray hits: an Interactable on that object or a parent.
    if (auto hit = physics().raycast(m_eye, m_dir, maxReach, filter)) {
        for (GameObject* o = hit->gameObject; o; o = o->transform().parent() ? &o->transform().parent()->gameObject() : nullptr) {
            if (auto* i = o->getComponent<Interactable>(); i && i->isActive() && hit->distance <= i->reach) return i;
        }
    }
    // 2. Things without colliders: the best one in the view cone that nothing blocks.
    const float minCos = std::cos(glm::radians(coneDegrees));
    Interactable* best = nullptr;
    float bestCos = minCos;
    s.forEach<Interactable>([&](Interactable& i) {
        if (!i.isActive()) return;
        const glm::vec3 to = i.focusPoint() - m_eye;
        const float d = glm::length(to);
        if (d < 1e-4f || d > std::min(i.reach, maxReach)) return;
        const float c = glm::dot(to / d, m_dir);
        if (c <= bestCos) return;
        if (auto block = physics().raycast(m_eye, to / d, d - 0.05f, filter); block && block->gameObject != &i.gameObject()) return;
        best = &i;
        bestCos = c;
    });
    return best;
}

void Interactor::update(float)
{
    if (useMainCamera) {
        if (const Camera* cam = gameObject().scene().mainCamera()) setView(cam->transform().position(), cam->transform().forward());
    }
    Interactable* now = findFocus();
    Interactable* before = focus();
    if (now != before) {
        if (before) before->focusChanged(false);
        if (now) now->focusChanged(true);
    }
    m_focus = now;
    m_focusObject = now ? now->gameObject().id() : 0;
    if (!action.empty() && input().actionPressed(action)) interact();
}

Interactable* Interactor::focus() const
{
    // The focus may have been destroyed since update(), or just its component
    // removed: check it still exists without touching m_focus (it may dangle) -
    // the object by id, then the component by address among the object's.
    if (!m_focus || !m_focusObject) return nullptr;
    GameObject* o = gameObject().scene().findById(m_focusObject);
    if (!o || o->isDestroyed()) return nullptr;
    for (const auto& c : o->components())
        if (c.get() == m_focus) return m_focus;
    return nullptr;
}

std::string Interactor::prompt() const
{
    Interactable* i = focus();
    return i ? i->currentPrompt() : std::string();
}

bool Interactor::interact()
{
    Interactable* i = focus();
    if (!i || i->currentPrompt().empty()) return false;
    i->use(gameObject());
    return true;
}

} // namespace ve

// -----------------------------------------------------------------------------
// Reflection
// -----------------------------------------------------------------------------

#include "ve/scene/Reflection.hpp"

namespace ve {

void registerInteractionTypes(TypeRegistry& r)
{
    r.add<Interactable>("Interactable", [](TypeBuilder<Interactable>& t) {
        t.category("Gameplay");
        t.field("prompt", &Interactable::prompt).tooltip("shown while looked at (what using does is set from code: onUse)");
        t.field("reach", &Interactable::reach).range(0.0f, 50.0f).speed(0.05f);
        t.field("focusOffset", &Interactable::focusOffset).tooltip("local point aimed at when it has no collider");
    });
    r.add<Interactor>("Interactor", [](TypeBuilder<Interactor>& t) {
        t.category("Gameplay");
        t.field("action", &Interactor::action).tooltip("Input action that uses the focus");
        t.field("maxReach", &Interactor::maxReach).range(0.0f, 50.0f);
        t.field("coneDegrees", &Interactor::coneDegrees).range(0.0f, 90.0f);
        t.field("useMainCamera", &Interactor::useMainCamera);
    });
}

} // namespace ve
