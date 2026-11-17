#include "Building.hpp"

#include "Props.hpp"

#include <ve/assets/AssetManager.hpp>
#include <ve/core/Engine.hpp>
#include <ve/core/Input.hpp>
#include <ve/core/Log.hpp>
#include <ve/gameplay/Interaction.hpp>
#include <ve/nav/Navigation.hpp>
#include <ve/physics/Physics.hpp>
#include <ve/render/Mesh.hpp>
#include <ve/render/Primitives.hpp>
#include <ve/render/Renderer.hpp>
#include <ve/scene/Components.hpp>
#include <ve/scene/Prefab.hpp>
#include <ve/scene/Scene.hpp>
#include <ve/world/Terrain.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <cmath>
#include <fstream>
#include <map>

namespace savanna {

VE_REGISTER_TYPE(BuildPiece, "BuildPiece")
{
    t.category("Savanna");
    t.enumField("kind", &BuildPiece::kind,
                {"Foundation", "Floor", "Wall", "WallDoor", "WallWindow", "Railing", "Post", "Beam", "RoofMetal", "RoofThatch", "Stairs", "Fence"});
    t.enumField("material", &BuildPiece::material, {"Mud", "Plaster", "Timber", "Stone"});
}

namespace {

using Kind = BuildPiece::Kind;
using Mat = BuildPiece::Material;

constexpr float kHalf = BuildPiece::kTile * 0.5f;
constexpr float kThick = 0.2f;                         // walls
const float kRoofAngle = glm::radians(20.0f);
const float kRise = BuildPiece::kTile * std::tan(kRoofAngle); // over one tile of run

// --- Mesh building --------------------------------------------------------------

struct Builder3 {
    ve::MeshData md;
    struct Box {
        glm::vec3 center, half;
        glm::quat rot;
    };
    std::vector<Box> colliders;

    void box(const glm::vec3& lo, const glm::vec3& hi, const glm::vec3& color, const glm::mat4& xf = glm::mat4(1.0f), bool collide = false)
    {
        const glm::vec3 c = (lo + hi) * 0.5f;
        md.append(ve::primitives::cube(hi - lo), xf * glm::translate(glm::mat4(1.0f), c), glm::vec4(color, 1.0f));
        if (collide) colliders.push_back({glm::vec3(xf * glm::vec4(c, 1.0f)), (hi - lo) * 0.5f, glm::quat_cast(glm::mat3(xf))});
    }
    void collider(const glm::vec3& lo, const glm::vec3& hi, const glm::mat4& xf = glm::mat4(1.0f))
    {
        colliders.push_back({glm::vec3(xf * glm::vec4((lo + hi) * 0.5f, 1.0f)), (hi - lo) * 0.5f, glm::quat_cast(glm::mat3(xf))});
    }
};

glm::vec3 wallColor(Mat m)
{
    switch (m) {
    case Mat::Plaster: return {0.86f, 0.80f, 0.69f};
    case Mat::Timber: return {0.45f, 0.30f, 0.18f};
    case Mat::Stone: return {0.50f, 0.41f, 0.32f}; // warm laterite stone
    default: return {0.58f, 0.39f, 0.25f}; // mud brick
    }
}
const glm::vec3 kTimber{0.42f, 0.28f, 0.16f};
const glm::vec3 kTimberDark{0.30f, 0.20f, 0.12f};
const glm::vec3 kMudDark{0.42f, 0.28f, 0.19f};

// A wall's body with an opening (x0..x1, y0..y1 local; none if x0 >= x1).
void wallBody(Builder3& b, Mat m, float ox0, float ox1, float oy0, float oy1)
{
    const glm::vec3 c = wallColor(m);
    const float h = BuildPiece::kWall, t = kThick * 0.5f;
    // Plinth (darker, a little proud) and a top course: reads as a built wall, not a slab.
    const glm::vec3 plinth = m == Mat::Plaster ? glm::vec3(0.55f, 0.38f, 0.26f) : kMudDark;
    auto part = [&](float x0, float x1, float y0, float y1) {
        if (x1 - x0 < 0.01f || y1 - y0 < 0.01f) return;
        b.collider({x0, y0, -t}, {x1, y1, t});
        const float py = std::min(y1, 0.35f);
        if (y0 < py) b.box({x0, y0, -t - 0.015f}, {x1, py, t + 0.015f}, plinth);
        if (y1 > std::max(y0, py)) b.box({x0, std::max(y0, py), -t}, {x1, std::min(y1, h - 0.12f), t}, c);
        if (y1 > h - 0.12f) b.box({x0, h - 0.12f, -t - 0.01f}, {x1, y1, t + 0.01f}, c * 0.85f);
    };
    if (ox0 >= ox1) {
        part(-kHalf, kHalf, 0.0f, h);
        return;
    }
    part(-kHalf, ox0, 0.0f, h);
    part(ox1, kHalf, 0.0f, h);
    part(ox0, ox1, 0.0f, oy0);
    part(ox0, ox1, oy1, h);
    // Timber frame around the opening.
    const float f = 0.07f, d = t + 0.02f;
    b.box({ox0 - f, oy0, -d}, {ox0, oy1, d}, kTimber);
    b.box({ox1, oy0, -d}, {ox1 + f, oy1, d}, kTimber);
    b.box({ox0 - f, oy1, -d}, {ox1 + f, oy1 + f, d}, kTimber);
    if (oy0 > 0.01f) b.box({ox0 - f, oy0 - f, -d - 0.04f}, {ox1 + f, oy0, d + 0.04f}, kTimber); // sill
}

void build(Builder3& b, Kind kind, Mat mat)
{
    const float h = BuildPiece::kWall;
    switch (kind) {
    case Kind::Foundation: {
        const glm::vec3 c = wallColor(mat == Mat::Mud ? Mat::Mud : Mat::Stone);
        b.box({-kHalf, -0.4f, -kHalf}, {kHalf, BuildPiece::kFoundation - 0.06f, kHalf}, c, glm::mat4(1.0f), true);
        b.box({-kHalf - 0.02f, BuildPiece::kFoundation - 0.06f, -kHalf - 0.02f}, {kHalf + 0.02f, BuildPiece::kFoundation, kHalf + 0.02f}, c * 1.12f);
        // Stone courses on the sides.
        for (int i = 0; i < 2; ++i) {
            const float y = -0.1f + 0.25f * float(i);
            b.box({-kHalf - 0.01f, y, -kHalf - 0.01f}, {kHalf + 0.01f, y + 0.03f, kHalf + 0.01f}, c * 0.8f);
        }
        break;
    }
    case Kind::Floor: {
        // Planks across, joists under.
        for (int i = 0; i < 5; ++i) {
            const float z0 = -kHalf + float(i) * 0.4f;
            const glm::vec3 c = kTimber * (0.9f + 0.1f * float(i % 3));
            b.box({-kHalf, -0.05f, z0 + 0.005f}, {kHalf, 0.0f, z0 + 0.395f}, c);
        }
        for (int i = 0; i < 3; ++i) {
            const float x = -kHalf + 0.1f + float(i) * 0.9f;
            b.box({x - 0.06f, -0.2f, -kHalf}, {x + 0.06f, -0.05f, kHalf}, kTimberDark);
        }
        b.collider({-kHalf, -0.12f, -kHalf}, {kHalf, 0.0f, kHalf});
        break;
    }
    case Kind::Wall: wallBody(b, mat, 0.0f, 0.0f, 0.0f, 0.0f); break;
    case Kind::WallDoor: {
        wallBody(b, mat, -0.5f, 0.5f, 0.0f, 2.1f);
        // A plank door, open inwards on its hinge.
        const glm::mat4 hinge = glm::rotate(glm::translate(glm::mat4(1.0f), {-0.5f, 0.0f, 0.0f}), glm::radians(-75.0f), {0, 1, 0});
        for (int i = 0; i < 4; ++i)
            b.box({0.02f + float(i) * 0.24f, 0.02f, -0.03f}, {0.25f + float(i) * 0.24f, 2.05f, 0.03f}, kTimber * (0.85f + 0.08f * float(i % 2)), hinge);
        break;
    }
    case Kind::WallWindow: {
        wallBody(b, mat, -0.45f, 0.45f, 1.0f, 1.9f);
        for (int i = 1; i <= 3; ++i) { // bars
            const float x = -0.45f + 0.225f * float(i);
            b.box({x - 0.015f, 1.0f, -0.015f}, {x + 0.015f, 1.9f, 0.015f}, glm::vec3(0.15f));
        }
        for (int s = -1; s <= 1; s += 2) { // shutters, opened back against the wall
            const float x = s < 0 ? -0.95f : 0.5f;
            b.box({x, 1.0f, kThick * 0.5f + 0.02f}, {x + 0.45f, 1.9f, kThick * 0.5f + 0.06f}, glm::vec3(0.25f, 0.38f, 0.42f));
        }
        break;
    }
    case Kind::Railing: {
        for (int s = -1; s <= 1; s += 2) b.box({float(s) * kHalf - 0.06f, 0.0f, -0.06f}, {float(s) * kHalf + 0.06f, 1.05f, 0.06f}, kTimberDark);
        b.box({-kHalf, 0.95f, -0.05f}, {kHalf, 1.02f, 0.05f}, kTimber);
        b.box({-kHalf, 0.12f, -0.04f}, {kHalf, 0.18f, 0.04f}, kTimber);
        for (int i = 1; i < 8; ++i) {
            const float x = -kHalf + 0.25f * float(i);
            b.box({x - 0.02f, 0.18f, -0.02f}, {x + 0.02f, 0.95f, 0.02f}, kTimber * 0.95f);
        }
        b.collider({-kHalf, 0.0f, -0.06f}, {kHalf, 1.05f, 0.06f});
        break;
    }
    case Kind::Post:
        b.box({-0.1f, 0.0f, -0.1f}, {0.1f, h, 0.1f}, kTimber, glm::mat4(1.0f), true);
        b.box({-0.12f, 0.0f, -0.12f}, {0.12f, 0.15f, 0.12f}, kMudDark);
        break;
    case Kind::Beam:
        b.box({-kHalf, -0.22f, -0.09f}, {kHalf, 0.0f, 0.09f}, kTimber, glm::mat4(1.0f), true);
        break;
    case Kind::RoofMetal:
    case Kind::RoofThatch: {
        // A sloped panel: low edge at z = +1 (y = 0), high edge at z = -1 (y = rise),
        // built flat in panel space and tilted.
        const float slant = BuildPiece::kTile / std::cos(kRoofAngle);
        const glm::mat4 xf = glm::rotate(glm::translate(glm::mat4(1.0f), {0.0f, kRise * 0.5f, 0.0f}), kRoofAngle, {1, 0, 0});
        const float over = kind == Kind::RoofThatch ? 0.45f : 0.3f; // eaves
        if (kind == Kind::RoofMetal) {
            b.box({-kHalf, 0.0f, -slant * 0.5f}, {kHalf, 0.02f, slant * 0.5f + over}, glm::vec3(0.50f, 0.32f, 0.21f), xf);
            for (int i = 0; i <= 12; ++i) { // corrugation, unevenly rusted
                const float x = -kHalf + float(i) * (2.0f * kHalf / 12.0f);
                const float rust = 0.75f + 0.35f * std::fmod(float(i) * 0.618f, 1.0f);
                b.box({x - 0.025f, 0.02f, -slant * 0.5f}, {x + 0.025f, 0.05f, slant * 0.5f + over},
                      glm::vec3(0.58f, 0.36f, 0.22f) * rust, xf);
            }
        } else {
            b.box({-kHalf - 0.05f, 0.0f, -slant * 0.5f}, {kHalf + 0.05f, 0.24f, slant * 0.5f + over}, glm::vec3(0.66f, 0.53f, 0.30f), xf);
            b.box({-kHalf - 0.05f, -0.04f, slant * 0.5f}, {kHalf + 0.05f, 0.18f, slant * 0.5f + over + 0.08f}, glm::vec3(0.55f, 0.43f, 0.24f), xf);
        }
        b.box({-kHalf, -0.12f, -slant * 0.5f}, {-kHalf + 0.1f, 0.0f, slant * 0.5f}, kTimberDark, xf); // rafters
        b.box({kHalf - 0.1f, -0.12f, -slant * 0.5f}, {kHalf, 0.0f, slant * 0.5f}, kTimberDark, xf);
        b.collider({-kHalf, 0.0f, -slant * 0.5f}, {kHalf, 0.1f, slant * 0.5f + over}, xf);
        break;
    }
    case Kind::Stairs: {
        // Up towards -z, from the ground to a foundation's top over 1 m.
        constexpr int kSteps = 3;
        const float rise = BuildPiece::kFoundation / float(kSteps), run = 1.0f / float(kSteps);
        for (int i = 0; i < kSteps; ++i) {
            const float z1 = 0.5f - run * float(i), z0 = z1 - run;
            b.box({-kHalf + 0.1f, rise * float(i + 1) - 0.05f, z0}, {kHalf - 0.1f, rise * float(i + 1), z1 + 0.03f}, kTimber * (0.9f + 0.1f * float(i % 2)));
            b.collider({-kHalf, 0.0f, z0}, {kHalf, rise * float(i + 1), z1});
        }
        for (int s = -1; s <= 1; s += 2) // stringers
            b.box({float(s) * kHalf - 0.1f * float(s > 0), 0.0f, -0.5f}, {float(s) * kHalf + 0.1f * float(s < 0), BuildPiece::kFoundation, 0.5f}, kTimberDark);
        break;
    }
    case Kind::Fence: {
        for (int s = -1; s <= 1; s += 2) b.box({float(s) * kHalf - 0.07f, 0.0f, -0.07f}, {float(s) * kHalf + 0.07f, 1.35f, 0.07f}, kTimberDark);
        for (int r = 0; r < 3; ++r) {
            const float y = 0.3f + 0.4f * float(r);
            b.box({-kHalf, y, -0.03f}, {kHalf, y + 0.1f, 0.03f}, kTimber * (0.9f + 0.05f * float(r)));
        }
        b.collider({-kHalf, 0.0f, -0.07f}, {kHalf, 1.35f, 0.07f});
        break;
    }
    }
}

// Where each kind connects (local, before the piece's rotation).
std::vector<glm::vec3> snapPoints(Kind kind)
{
    std::vector<glm::vec3> p;
    auto edges = [&](float y) { // a tile's corners and edge midpoints
        for (int z = -1; z <= 1; ++z)
            for (int x = -1; x <= 1; ++x)
                if (x || z) p.push_back({float(x) * kHalf, y, float(z) * kHalf});
    };
    auto line = [&](float y) { // a wall-like piece along x: its ends and middle
        for (int x = -1; x <= 1; ++x) p.push_back({float(x) * kHalf, y, 0.0f});
    };
    switch (kind) {
    case Kind::Foundation: edges(BuildPiece::kFoundation); break;
    case Kind::Floor: edges(0.0f); break;
    case Kind::Wall:
    case Kind::WallDoor:
    case Kind::WallWindow: line(0.0f); line(BuildPiece::kWall); break;
    case Kind::Railing:
    case Kind::Beam:
    case Kind::Fence: line(0.0f); break;
    case Kind::Post: p = {{0, 0, 0}, {0, BuildPiece::kWall, 0}}; break;
    case Kind::RoofMetal:
    case Kind::RoofThatch:
        for (int x = -1; x <= 1; ++x) {
            p.push_back({float(x) * kHalf, 0.0f, kHalf});
            p.push_back({float(x) * kHalf, kRise, -kHalf});
        }
        break;
    case Kind::Stairs:
        for (int x = -1; x <= 1; ++x) p.push_back({float(x) * kHalf, BuildPiece::kFoundation, -0.5f});
        break;
    }
    return p;
}

// Meshes are shared by every piece of the same kind and material.
std::shared_ptr<ve::Mesh> pieceMesh(Kind kind, Mat mat, Builder3& b)
{
    static std::map<int, std::weak_ptr<ve::Mesh>> cache;
    const int key = int(kind) * 16 + int(mat);
    build(b, kind, mat);
    if (auto m = cache[key].lock()) return m;
    ve::MeshData md = b.md;
    md.makeFlatShaded();
    auto mesh = ve::Mesh::create(ve::Engine::get().renderer(), md, {}, "build piece");
    cache[key] = mesh;
    return mesh;
}

} // namespace

// -----------------------------------------------------------------------------
// BuildPiece
// -----------------------------------------------------------------------------

BuildPiece* BuildPiece::of(ve::GameObject* o)
{
    for (; o; o = o->transform().parent() ? &o->transform().parent()->gameObject() : nullptr)
        if (auto* p = dynamic_cast<BuildPiece*>(o)) return p;
    return nullptr;
}

void BuildPiece::start()
{
    Builder3 b;
    addComponent<ve::MeshRenderer>(pieceMesh(kind, material, b), propMaterial());
    for (const glm::vec3& p : snapPoints(kind)) {
        auto& sp = addComponent<ve::SnapPoint>();
        sp.offset = p;
        if (kind == Kind::Fence) { // fences join fences, and stand on foundations / floors
            sp.type = "fence";
            sp.accepts = "fence, grid";
        }
    }
    for (const auto& c : b.colliders) { // the solid volume, for the placement fit test
        auto& box = addComponent<ve::PlacementBox>();
        box.center = c.center;
        box.halfExtents = c.half;
        box.rotation = c.rot;
    }
    if (ve::PlacementGhost::is(*this)) return; // a preview: no physics
    for (const auto& c : b.colliders) {
        auto& col = scene().spawn(name()); // (shown when aimed at)
        col.setParent(this, false);
        col.addComponent<ve::Rigidbody>(ve::Collider::box(c.half).offset(c.center, c.rot), ve::BodyType::Static);
    }
}

// -----------------------------------------------------------------------------
// Builder
// -----------------------------------------------------------------------------

Builder::Builder() : GameObject("Builder")
{
    ve::Input& in = input();
    in.bindAction("BuildMode", ve::Key::B);
    in.bindAction("BuildMode", ve::GamepadButton::DPadUp);
    in.bindAction("BuildPlace", ve::MouseButton::Left);
    in.bindAction("BuildPlace", ve::GamepadAxis::RightTrigger);
    in.bindAction("BuildRemove", ve::MouseButton::Right);
    in.bindAction("BuildRemove", ve::GamepadAxis::LeftTrigger);
    in.bindAction("BuildRotate", ve::Key::R);
    in.bindAction("BuildRotate", ve::GamepadButton::RightShoulder);
    in.bindAction("BuildNext", ve::Key::X);
    in.bindAction("BuildNext", ve::GamepadButton::DPadRight);
    in.bindAction("BuildPrev", ve::Key::Z);
    in.bindAction("BuildPrev", ve::GamepadButton::DPadLeft);
}

void Builder::start()
{
    loadCatalogue();
}

void Builder::loadCatalogue()
{
    // The kit is data: building/catalogue.json lists the pieces in menu order.
    m_catalogue.clear();
    try {
        std::ifstream in(assets().resolve("building/catalogue.json"));
        ve::Json j;
        in >> j;
        for (const ve::Json& e : j.value("pieces", ve::Json::array()))
            m_catalogue.push_back({e.value("name", std::string("?")), e.value("prefab", std::string())});
    } catch (const std::exception& e) {
        ve::log::warn("Building: {}", e.what());
    }
}

bool Builder::building(const ve::Scene& scene)
{
    const Builder* b = scene.findFirst<Builder>();
    return b && b->active();
}

void Builder::setActive(bool on)
{
    if (m_active == on) return;
    m_active = on;
    // The player's own Use / Throw step aside while building.
    if (ve::GameObject* player = scene().find("Player"))
        if (auto* who = player->getComponent<ve::Interactor>()) who->enabled = !on;
    if (!on) m_preview.hide();
}

void Builder::select(int index)
{
    if (m_catalogue.empty()) return;
    const int n = int(m_catalogue.size());
    m_index = ((index % n) + n) % n;
}

ve::GameObject* Builder::aimedPiece() const
{
    const ve::Camera* cam = scene().mainCamera();
    if (!cam) return nullptr;
    const glm::vec3 eye = cam->transform().position(), dir = cam->transform().forward();
    auto hit = physics().raycast(eye, dir, 8.0f);
    return hit && hit->gameObject ? BuildPiece::of(hit->gameObject) : nullptr;
}

void Builder::update(float)
{
    ve::Input& in = input();
    if (in.actionPressed("BuildMode")) setActive(!m_active);
    if (!m_active || m_catalogue.empty()) return;
    if (in.actionPressed("BuildNext") || in.wheel() < 0.0f) select(m_index + 1);
    if (in.actionPressed("BuildPrev") || in.wheel() > 0.0f) select(m_index - 1);
    if (in.actionPressed("BuildRotate")) m_yaw += glm::radians(45.0f);
    if (in.actionPressed("BuildRemove")) remove(aimedPiece());
    if (in.actionPressed("BuildPlace")) place();
}

void Builder::lateUpdate(float)
{
    if (m_active && !m_catalogue.empty()) updatePreview();
}

void Builder::updatePreview()
{
    if (!m_preview.show(scene(), m_catalogue[size_t(m_index)].prefab)) return;
    m_preview.update();

    // Aim: where the camera looks, on the ground or another piece, within reach.
    glm::vec3 aim;
    if (m_aimOverride) {
        aim = *m_aimOverride;
        m_aimHit = true;
    } else {
        const ve::Camera* cam = scene().mainCamera();
        if (!cam) return;
        const glm::vec3 eye = cam->transform().position(), dir = cam->transform().forward();
        auto hit = physics().raycast(eye, dir, 9.0f);
        m_aimHit = hit.has_value();
        aim = hit ? hit->point : eye + dir * 6.0f;
    }
    m_rot = glm::angleAxis(ve::placement::snapAngle(m_yaw, glm::radians(45.0f)), glm::vec3(0, 1, 0));
    m_pos = aim;
    m_snapped = false;
    if (auto s = ve::placement::findSnap(scene(), m_preview.snapPoints(), m_rot, aim, 0.9f, m_preview.object())) {
        m_pos = s->position;
        m_snapped = true;
    }
    // Fits: nothing in the way (the ground may cut into it: foundations sit on
    // slopes), and resting on something: the ground or another piece.
    const bool fits = ve::placement::fits(physics(), m_preview.volume(), m_pos, m_rot, 0.1f,
                                          [](const ve::RaycastHit& h) { return h.gameObject && dynamic_cast<ve::Terrain*>(h.gameObject); });
    m_preview.setPose(m_pos, m_rot);
    m_preview.setValid(fits && (m_snapped || m_aimHit) && m_preview.ready());
}

ve::GameObject* Builder::place()
{
    if (!m_active || !m_preview.visible() || !m_preview.valid()) return nullptr;
    ve::GameObject* piece = ve::prefab::instantiate(scene(), m_catalogue[size_t(m_index)].prefab, m_pos, m_rot);
    if (piece) rebuildNavigation(m_pos);
    return piece;
}

bool Builder::remove(ve::GameObject* object)
{
    BuildPiece* piece = BuildPiece::of(object);
    if (!piece || !piece->getComponent<ve::PrefabInstance>()) return false; // only built pieces
    const glm::vec3 at = piece->transform().position();
    piece->destroy();
    rebuildNavigation(at);
    return true;
}

void Builder::rebuildNavigation(const glm::vec3& at)
{
    // Villagers walk around (and through the doors of) what you build.
    if (auto* nav = ve::NavMeshSurface::find(scene())) nav->rebuild({at.x - 4.0f, at.z - 4.0f, at.x + 4.0f, at.z + 4.0f});
}

void Builder::onDestroy()
{
    m_preview.hide();
}

void Builder::onGui()
{
    if (!m_active || m_catalogue.empty()) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y - 70.0f}, ImGuiCond_Always, {0.5f, 1.0f});
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##build", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs);
    ImGui::Text("BUILDING   %s  (%d/%d)", m_catalogue[size_t(m_index)].name.c_str(), m_index + 1, int(m_catalogue.size()));
    ImGui::SameLine();
    if (m_preview.valid()) ImGui::TextColored({0.5f, 1.0f, 0.6f, 1.0f}, m_snapped ? "  snapped" : "  ok");
    else ImGui::TextColored({1.0f, 0.45f, 0.4f, 1.0f}, "  blocked");
    ImGui::TextDisabled("Wheel / Z X select   R rotate   LMB place   RMB remove   B exit");
    ImGui::End();
}

} // namespace savanna
