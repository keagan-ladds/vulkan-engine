#include "ve/gameplay/Placement.hpp"

#include "ve/core/Engine.hpp"
#include "ve/gameplay/Interaction.hpp"
#include "ve/physics/Physics.hpp"
#include "ve/render/Material.hpp"
#include "ve/render/Mesh.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/scene/Prefab.hpp"
#include "ve/scene/Scene.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string_view>

namespace ve {

namespace {

// "wall, floor" contains "floor" (or "*": anything). Views into the list, no
// copies: findSnap runs it for every point pair, every frame.
bool listHas(std::string_view list, std::string_view value)
{
    while (!list.empty()) {
        const size_t comma = list.find(',');
        std::string_view item = list.substr(0, comma);
        list = comma == std::string_view::npos ? std::string_view() : list.substr(comma + 1);
        const size_t b = item.find_first_not_of(" \t");
        if (b == std::string_view::npos) continue;
        item = item.substr(b, item.find_last_not_of(" \t") - b + 1);
        if (item == "*" || item == value) return true;
    }
    return false;
}

void forSubtree(GameObject& o, const std::function<void(GameObject&)>& fn)
{
    fn(o);
    for (Transform* c : o.transform().children()) forSubtree(c->gameObject(), fn);
}

// From world space to the root's frame: its position and rotation, but not its
// scale (a piece scaled 2x measures twice as big, like snapPointsOf's points).
glm::mat4 rootFrameInverse(const GameObject& root)
{
    const glm::mat4& w = root.transform().worldMatrix();
    glm::mat3 r(w);
    for (int a = 0; a < 3; ++a) {
        const float len = glm::length(r[a]);
        if (len > 1e-8f) r[a] /= len;
    }
    glm::mat4 frame(r);
    frame[3] = w[3];
    return glm::inverse(frame);
}

bool inSubtree(const GameObject& o, const GameObject* root)
{
    for (const Transform* t = &o.transform(); t; t = t->parent())
        if (&t->gameObject() == root) return true;
    return false;
}

} // namespace

// -----------------------------------------------------------------------------
// SnapPoint / PlacementGhost
// -----------------------------------------------------------------------------

glm::vec3 SnapPoint::worldPosition() const
{
    return transform().transformPoint(offset);
}

glm::vec3 SnapPoint::worldFacing() const
{
    return transform().rotation() * facing;
}

bool SnapPoint::connectsTo(const std::string& otherType, const std::string& otherAccepts) const
{
    return listHas(accepts, otherType) || listHas(otherAccepts, type);
}

bool PlacementGhost::is(const GameObject& object)
{
    for (const Transform* t = &object.transform(); t; t = t->parent())
        if (t->gameObject().getComponent<PlacementGhost>()) return true;
    return false;
}

// -----------------------------------------------------------------------------
// placement::
// -----------------------------------------------------------------------------

namespace placement {

std::vector<LocalSnap> snapPointsOf(const GameObject& root)
{
    std::vector<LocalSnap> out;
    const glm::mat4 inv = glm::inverse(root.transform().worldMatrix());
    const glm::vec3 scale(glm::length(glm::vec3(root.transform().worldMatrix()[0])), glm::length(glm::vec3(root.transform().worldMatrix()[1])),
                          glm::length(glm::vec3(root.transform().worldMatrix()[2])));
    forSubtree(const_cast<GameObject&>(root), [&](GameObject& o) {
        for (const auto& c : o.components()) {
            const auto* sp = dynamic_cast<const SnapPoint*>(c.get());
            if (!sp) continue;
            // Relative to the root, in its rotated frame without its scale: rotate by the
            // piece's rotation later. (Snap points are geometry: they scale with the piece.)
            const glm::vec3 local = glm::vec3(inv * glm::vec4(sp->worldPosition(), 1.0f)) * scale;
            const glm::vec3 facing = glm::mat3(inv) * sp->worldFacing();
            out.push_back({local, sp->type, sp->accepts, sp->alignRotation, facing});
        }
    });
    return out;
}

std::optional<SnapResult> findSnap(const Scene& scene, const std::vector<LocalSnap>& points, const glm::quat& rotation,
                                   const glm::vec3& aim, float radius, const GameObject* ignore)
{
    std::optional<SnapResult> best;
    if (points.empty()) return best;
    // The rotation a pair asks for: the player's, or turned so the facings line up.
    auto yawOf = [](const glm::vec3& v) { return std::atan2(-v.z, v.x); };
    const float chosen = yawOf(rotation * glm::vec3(1, 0, 0));
    auto rotationFor = [&](const LocalSnap& p, const SnapPoint& t) {
        if (!p.alignRotation) return rotation;
        // Yaw that turns the piece's facing onto the target's, or onto its opposite.
        const float y = yawOf(glm::vec3(t.worldFacing().x, 0.0f, t.worldFacing().z)) - yawOf(glm::vec3(p.facing.x, 0.0f, p.facing.z));
        auto diff = [&](float a) { return std::abs(std::remainder(a - chosen, 6.2831853f)); };
        const float pick = diff(y) <= diff(y + 3.14159265f) ? y : y + 3.14159265f;
        return glm::angleAxis(pick, glm::vec3(0, 1, 0));
    };
    // A cheap bound: only targets within reach of any of the piece's points.
    float reach = 0.0f;
    for (const LocalSnap& p : points) reach = std::max(reach, glm::length(p.offset));
    reach += radius;
    scene.forEach<SnapPoint>([&](const SnapPoint& t) {
        if (!t.isActive()) return;
        const GameObject& owner = t.gameObject();
        if (ignore && inSubtree(owner, ignore)) return;
        if (PlacementGhost::is(owner)) return;
        const glm::vec3 tw = t.worldPosition();
        if (glm::distance(tw, aim) > reach) return;
        for (size_t i = 0; i < points.size(); ++i) {
            if (!t.connectsTo(points[i].type, points[i].accepts)) continue;
            const glm::quat r = rotationFor(points[i], t);
            // Close if the piece's point would land near the target, or if the
            // player aims right at the target (a door at the middle of an
            // opening, while the door's origin is at its bottom).
            const float moved = glm::distance(aim + r * points[i].offset, tw);
            const float d = std::min(moved, glm::distance(aim, tw));
            if (d > radius) continue;
            if (best && (d > best->score || (d == best->score && moved >= best->distance))) continue;
            best = SnapResult{tw - r * points[i].offset, r, tw, &t, moved, d};
        }
    });
    return best;
}

glm::vec3 snapToGrid(const glm::vec3& p, const glm::vec3& cell, const glm::vec3& origin)
{
    glm::vec3 out = p;
    for (int a = 0; a < 3; ++a)
        if (cell[a] > 0.0f) out[a] = origin[a] + std::round((p[a] - origin[a]) / cell[a]) * cell[a];
    return out;
}

float snapAngle(float radians, float step)
{
    return step > 0.0f ? std::round(radians / step) * step : radians;
}

AABB localBounds(const GameObject& root)
{
    AABB out;
    const glm::mat4 inv = rootFrameInverse(root);
    forSubtree(const_cast<GameObject&>(root), [&](GameObject& o) {
        const auto* mr = o.getComponent<MeshRenderer>();
        if (!mr || !mr->mesh) return;
        const AABB b = mr->mesh->bounds();
        if (!b.valid()) return;
        const glm::mat4 m = inv * o.transform().worldMatrix();
        for (int c = 0; c < 8; ++c) {
            const glm::vec3 p((c & 1) ? b.max.x : b.min.x, (c & 2) ? b.max.y : b.min.y, (c & 4) ? b.max.z : b.min.z);
            out.expand(glm::vec3(m * glm::vec4(p, 1.0f)));
        }
    });
    return out;
}

std::vector<OrientedBox> volumeOf(const GameObject& root)
{
    std::vector<OrientedBox> out;
    const glm::mat4 inv = rootFrameInverse(root);
    forSubtree(const_cast<GameObject&>(root), [&](GameObject& o) {
        for (const auto& c : o.components()) {
            const auto* b = dynamic_cast<const PlacementBox*>(c.get());
            if (!b) continue;
            // m carries the scale of the root and the children: split it into a
            // rotation (quat_cast wants a pure one) and the box's scaled half sizes.
            const glm::mat4 m = inv * o.transform().worldMatrix();
            const glm::mat3 linear(m);
            glm::mat3 rot = linear;
            for (int a = 0; a < 3; ++a) {
                const float len = glm::length(rot[a]);
                if (len > 1e-8f) rot[a] /= len;
            }
            const glm::mat3 boxAxes = glm::mat3_cast(b->rotation);
            glm::vec3 half;
            for (int a = 0; a < 3; ++a) half[a] = b->halfExtents[a] * glm::length(linear * boxAxes[a]);
            out.push_back({glm::vec3(m * glm::vec4(b->center, 1.0f)), half, glm::quat_cast(rot) * b->rotation});
        }
    });
    if (out.empty()) {
        const AABB a = localBounds(root);
        if (a.valid()) out.push_back({a.center(), a.extents(), glm::quat(1, 0, 0, 0)});
    }
    return out;
}

bool fits(const PhysicsWorld& physics, const std::vector<OrientedBox>& volume, const glm::vec3& position, const glm::quat& rotation,
          float margin, const std::function<bool(const RaycastHit&)>& ignoreHit)
{
    QueryFilter f;
    f.statics = true;
    f.dynamics = true;
    f.characters = true; // don't build into the player
    for (const OrientedBox& b : volume) {
        const glm::vec3 half = glm::max(b.half - glm::vec3(margin), glm::vec3(0.01f));
        for (const RaycastHit& h : physics.overlapBox(position + rotation * b.center, half, rotation * b.rotation, f))
            if (!ignoreHit || !ignoreHit(h)) return false;
    }
    return true;
}

} // namespace placement

// -----------------------------------------------------------------------------
// PlacementPreview
// -----------------------------------------------------------------------------

PlacementPreview::PlacementPreview() = default;

PlacementPreview::~PlacementPreview()
{
    hide();
}

GameObject* PlacementPreview::object() const
{
    return m_scene && m_id ? m_scene->findById(m_id) : nullptr;
}

bool PlacementPreview::visible() const
{
    return object() != nullptr;
}

bool PlacementPreview::show(Scene& scene, const std::string& prefabPath)
{
    if (visible() && m_prefab == prefabPath && m_scene == &scene) return true;
    hide();
    m_scene = &scene;
    GameObject* o = prefab::instantiate(scene, prefabPath);
    if (!o) return false;
    // The ghost follows the cursor and swaps materials every frame: never batch
    // it as static, even if the prefab says its pieces are.
    o->setStatic(false);
    m_id = o->id();
    m_prefab = prefabPath;
    m_measured = false;
    o->addComponent<PlacementGhost>();
    forSubtree(*o, [](GameObject& g) {
        g.persistent = false; // not saved anywhere
        for (const auto& c : g.components()) {
            Component* comp = c.get();
            if (auto* pi = dynamic_cast<PrefabInstance*>(comp)) pi->saveInGame = false;
            // Whatever the prefab declares that would act in the world: off.
            if (dynamic_cast<Rigidbody*>(comp) || dynamic_cast<SnapPoint*>(comp) || dynamic_cast<Interactable*>(comp)) comp->enabled = false;
        }
    });
    if (!m_material) {
        m_material = Engine::get().renderer().createStandardMaterial(StandardMaterial::Mode::Transparent);
        m_material->setRoughness(0.6f);
        m_material->setVertexColorWeight(0.0f);
    }
    m_valid = !m_valid; // force the colour update
    setValid(!m_valid);
    return true;
}

void PlacementPreview::hide()
{
    if (GameObject* o = object()) o->destroy();
    m_id = 0;
    m_prefab.clear();
    m_snaps.clear();
    m_volume.clear();
    m_bounds = AABB{};
    m_measured = false;
}

void PlacementPreview::setPose(const glm::vec3& position, const glm::quat& rotation)
{
    if (GameObject* o = object()) {
        o->transform().setPosition(position);
        o->transform().setRotation(rotation);
    }
}

void PlacementPreview::setValid(bool valid)
{
    if (valid == m_valid) return;
    m_valid = valid;
    if (m_material) m_material->setBaseColor(valid ? validColor : invalidColor);
}

void PlacementPreview::update()
{
    GameObject* o = object();
    if (!o) return;
    // Pieces may build their meshes and snap points in start(): read them once
    // it ran (as if unrotated at the origin), and keep everything ghosted.
    if (!m_measured) {
        const glm::vec3 p = o->transform().position();
        const glm::quat q = o->transform().rotation();
        o->transform().setPosition(glm::vec3(0.0f));
        o->transform().setRotation(glm::quat(1, 0, 0, 0));
        m_bounds = placement::localBounds(*o);
        // (Snap points are disabled on the ghost; snapPointsOf reads them anyway.)
        m_snaps = placement::snapPointsOf(*o);
        m_volume = placement::volumeOf(*o);
        o->transform().setPosition(p);
        o->transform().setRotation(q);
        m_measured = m_bounds.valid() || !m_snaps.empty();
    }
    forSubtree(*o, [&](GameObject& g) {
        for (const auto& c : g.components()) {
            if (auto* mr = dynamic_cast<MeshRenderer*>(c.get())) {
                for (auto& m : mr->materials) m = m_material;
                if (mr->materials.empty()) mr->materials.push_back(m_material);
                mr->castShadows = false;
            } else if (dynamic_cast<SnapPoint*>(c.get()) || dynamic_cast<Rigidbody*>(c.get()) || dynamic_cast<Interactable*>(c.get())) {
                c->enabled = false;
            }
        }
    });
}

} // namespace ve

// -----------------------------------------------------------------------------
// Reflection
// -----------------------------------------------------------------------------

#include "ve/scene/Reflection.hpp"

namespace ve {

void registerPlacementTypes(TypeRegistry& r)
{
    r.add<SnapPoint>("SnapPoint", [](TypeBuilder<SnapPoint>& t) {
        t.category("Gameplay");
        t.field("offset", &SnapPoint::offset).tooltip("local position of the connection point");
        t.field("type", &SnapPoint::type);
        t.field("accepts", &SnapPoint::accepts).tooltip("types it connects to, comma-separated (* = any)");
        t.field("alignRotation", &SnapPoint::alignRotation).tooltip("also turn the piece so its facing lines up with the target's (doors, windows)");
        t.field("facing", &SnapPoint::facing).tooltip("local direction lined up with the target point's facing");
    });
    r.add<PlacementBox>("PlacementBox", [](TypeBuilder<PlacementBox>& t) {
        t.category("Gameplay");
        t.field("center", &PlacementBox::center);
        t.field("halfExtents", &PlacementBox::halfExtents).tooltip("part of the solid volume checked when placing the piece");
    });
}

} // namespace ve
