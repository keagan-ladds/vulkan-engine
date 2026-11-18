#include "Building.hpp"

#include <ve/net/Network.hpp>

#include "GameUi.hpp"
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
#include <ve/ui/IconBaker.hpp>
#include <ve/world/Terrain.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>

namespace savanna {

VE_REGISTER_TYPE(DoorHinge, "DoorHinge")
{
    t.category("Savanna");
    t.field("openAngle", &DoorHinge::openAngle).range(0.0f, 180.0f);
    t.field("speed", &DoorHinge::speed).range(0.1f, 20.0f);
}

VE_REGISTER_TYPE(BuildPiece, "BuildPiece")
{
    t.category("Savanna");
    t.enumField("kind", &BuildPiece::kind,
                {"Foundation", "Floor", "Wall", "WallDoor", "WallWindow", "Railing", "Post", "Beam", "RoofMetal", "RoofThatch", "Stairs", "Fence",
                 "Door", "Window"});
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
    // (The frame comes with the door / window that's fitted into the opening.)
}

// Door / window openings in the walls (local, from the wall's origin).
constexpr float kDoorX = 0.5f, kDoorTop = 2.1f;
constexpr float kWinX = 0.45f, kWinBottom = 1.0f, kWinTop = 1.9f;

// A fitting's frame inside an opening, proud of both wall faces.
void frame(Builder3& b, float x0, float x1, float y0, float y1, bool sill)
{
    const float f = 0.07f, d = kThick * 0.5f + 0.025f;
    b.box({x0, y0, -d}, {x0 + f, y1, d}, kTimber, glm::mat4(1.0f), true);
    b.box({x1 - f, y0, -d}, {x1, y1, d}, kTimber, glm::mat4(1.0f), true);
    b.box({x0, y1 - f, -d}, {x1, y1, d}, kTimber, glm::mat4(1.0f), true);
    if (sill) b.box({x0 - 0.05f, y0, -d - 0.05f}, {x1 + 0.05f, y0 + f, d + 0.05f}, kTimber, glm::mat4(1.0f), true);
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
    case Kind::WallDoor: wallBody(b, mat, -kDoorX, kDoorX, 0.0f, kDoorTop); break;
    case Kind::WallWindow: wallBody(b, mat, -kWinX, kWinX, kWinBottom, kWinTop); break;
    case Kind::Door:
        // The frame; the leaf is its own object (it swings), see BuildPiece::start.
        frame(b, -kDoorX, kDoorX, 0.0f, kDoorTop, false);
        break;
    case Kind::Window: {
        frame(b, -kWinX, kWinX, kWinBottom, kWinTop, true);
        for (int i = 1; i <= 3; ++i) { // bars
            const float x = -kWinX + 2.0f * kWinX * 0.25f * float(i);
            b.box({x - 0.015f, kWinBottom, -0.015f}, {x + 0.015f, kWinTop, 0.015f}, glm::vec3(0.15f), glm::mat4(1.0f), true);
        }
        for (int s = -1; s <= 1; s += 2) { // shutters, folded back against the outside
            const float x0 = s < 0 ? -kWinX - 0.47f : kWinX + 0.02f;
            b.box({x0, kWinBottom, kThick * 0.5f + 0.03f}, {x0 + 0.45f, kWinTop, kThick * 0.5f + 0.07f}, glm::vec3(0.25f, 0.38f, 0.42f));
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
struct PieceSnap {
    glm::vec3 offset;
    const char* type = "grid";
    const char* accepts = "grid";
    bool align = false;
};

std::vector<PieceSnap> snapPoints(Kind kind)
{
    std::vector<PieceSnap> p;
    auto edges = [&](float y) { // a tile's corners and edge midpoints
        for (int z = -1; z <= 1; ++z)
            for (int x = -1; x <= 1; ++x)
                if (x || z) p.push_back({{float(x) * kHalf, y, float(z) * kHalf}});
    };
    auto line = [&](float y) { // a wall-like piece along x: its ends and middle
        for (int x = -1; x <= 1; ++x) p.push_back({{float(x) * kHalf, y, 0.0f}});
    };
    switch (kind) {
    case Kind::Foundation: edges(BuildPiece::kFoundation); break;
    case Kind::Floor: edges(0.0f); break;
    case Kind::Wall: line(0.0f); line(BuildPiece::kWall); break;
    case Kind::WallDoor: // + the opening a door fits into
        line(0.0f);
        line(BuildPiece::kWall);
        p.push_back({{0.0f, kDoorTop * 0.5f, 0.0f}, "door-opening", ""});
        break;
    case Kind::WallWindow:
        line(0.0f);
        line(BuildPiece::kWall);
        p.push_back({{0.0f, (kWinBottom + kWinTop) * 0.5f, 0.0f}, "window-opening", ""});
        break;
    case Kind::Door: p.push_back({{0.0f, kDoorTop * 0.5f, 0.0f}, "door", "door-opening", true}); break;
    case Kind::Window: p.push_back({{0.0f, (kWinBottom + kWinTop) * 0.5f, 0.0f}, "window", "window-opening", true}); break;
    case Kind::Railing:
    case Kind::Beam: line(0.0f); break;
    case Kind::Fence: // fences join fences, and stand on foundations / floors
        line(0.0f);
        for (PieceSnap& s : p) {
            s.type = "fence";
            s.accepts = "fence, grid";
        }
        break;
    case Kind::Post: p = {{{0, 0, 0}}, {{0, BuildPiece::kWall, 0}}}; break;
    case Kind::RoofMetal:
    case Kind::RoofThatch:
        for (int x = -1; x <= 1; ++x) {
            p.push_back({{float(x) * kHalf, 0.0f, kHalf}});
            p.push_back({{float(x) * kHalf, kRise, -kHalf}});
        }
        break;
    case Kind::Stairs:
        for (int x = -1; x <= 1; ++x) p.push_back({{float(x) * kHalf, BuildPiece::kFoundation, -0.5f}});
        break;
    }
    return p;
}

// The door leaf: planks on a hinge at its left edge (local x = 0 is the hinge).
std::shared_ptr<ve::Mesh> doorLeafMesh(Builder3& b)
{
    static std::weak_ptr<ve::Mesh> cache;
    const float w = 2.0f * kDoorX - 0.14f;
    for (int i = 0; i < 4; ++i)
        b.box({0.005f + float(i) * w * 0.25f, 0.02f, -0.03f}, {float(i + 1) * w * 0.25f - 0.005f, kDoorTop - 0.08f, 0.03f},
              kTimber * (0.85f + 0.08f * float(i % 2)));
    b.box({0.0f, 0.4f, -0.045f}, {w, 0.5f, -0.03f}, kTimberDark); // ledges
    b.box({0.0f, 1.5f, -0.045f}, {w, 1.6f, -0.03f}, kTimberDark);
    b.collider({0.0f, 0.02f, -0.03f}, {w, kDoorTop - 0.08f, 0.03f});
    if (auto m = cache.lock()) return m;
    ve::MeshData md = b.md;
    md.makeFlatShaded();
    auto mesh = ve::Mesh::create(ve::Engine::get().renderer(), md, {}, "door leaf");
    cache = mesh;
    return mesh;
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

// What a piece looks like as one mesh (icons): its body, plus a door's leaf (closed).
ve::MeshData pieceMeshData(Kind kind, Mat mat)
{
    Builder3 b;
    build(b, kind, mat);
    ve::MeshData md = b.md;
    if (kind == Kind::Door) {
        Builder3 leaf;
        doorLeafMesh(leaf);
        md.append(leaf.md, glm::translate(glm::mat4(1.0f), {-kDoorX + 0.07f, 0.0f, 0.0f}));
    }
    md.makeFlatShaded();
    return md;
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
    for (const PieceSnap& p : snapPoints(kind)) {
        auto& sp = addComponent<ve::SnapPoint>();
        sp.offset = p.offset;
        sp.type = p.type;
        sp.accepts = p.accepts;
        sp.alignRotation = p.align; // fittings turn to match their opening
    }
    const bool ghost = ve::PlacementGhost::is(*this);
    // A door's leaf swings on a hinge: its own object, its own (moving) collider.
    Builder3 leaf;
    ve::GameObject* leafObject = nullptr;
    if (kind == Kind::Door) {
        auto& l = scene().spawn("Door leaf");
        l.setParent(this, false);
        l.transform().setLocalPosition({-kDoorX + 0.07f, 0.0f, 0.0f});
        l.addComponent<ve::MeshRenderer>(doorLeafMesh(leaf), propMaterial());
        leafObject = &l;
        // The hinge comes from the prefab (it's Saveable: it must exist as soon as the piece
        // is created, so a loaded game can restore it); add one if a prefab forgot it.
        DoorHinge* h = getComponent<DoorHinge>();
        auto& hinge = h ? *h : addComponent<DoorHinge>();
        hinge.setLeaf(&l);
        auto& use = addComponent<ve::Interactable>();
        use.reach = 3.0f;
        use.focusOffset = {0.0f, 1.2f, 0.0f};
        use.promptFn = [&hinge] { return hinge.open() ? std::string("Close door") : std::string("Open door"); };
        use.onUse = [&hinge](ve::GameObject&) { hinge.setOpen(!hinge.open()); };
    }
    // The solid volume, for the placement fit test (the leaf closed).
    auto addBoxes = [&](const Builder3& from, const glm::vec3& at) {
        for (const auto& c : from.colliders) {
            auto& box = addComponent<ve::PlacementBox>();
            box.center = c.center + at;
            box.halfExtents = c.half;
            box.rotation = c.rot;
        }
    };
    addBoxes(b, glm::vec3(0.0f));
    if (leafObject) addBoxes(leaf, leafObject->transform().localPosition());
    if (ghost) return; // a preview: no physics
    // Physics surface by material (footsteps, impacts). Doors / windows are timber.
    const char* surface = isFitting() || material == Material::Timber ? "Wood" : material == Material::Stone ? "Stone" : "Mud";
    if (kind == Kind::RoofMetal) surface = "Metal";
    if (kind == Kind::RoofThatch) surface = "Thatch";
    for (const auto& c : b.colliders) {
        auto& col = scene().spawn(name()); // (shown when aimed at)
        col.setParent(this, false);
        col.addComponent<ve::Rigidbody>(ve::Collider::box(c.half).offset(c.center, c.rot), ve::BodyType::Static).surface = surface;
    }
    if (leafObject)
        for (const auto& c : leaf.colliders) // kinematic: it follows the swinging leaf
            leafObject->addComponent<ve::Rigidbody>(ve::Collider::box(c.half).offset(c.center, c.rot), ve::BodyType::Kinematic).surface = "Wood";
}

// -----------------------------------------------------------------------------
// DoorHinge
// -----------------------------------------------------------------------------

void DoorHinge::update(float dt)
{
    const float target = m_open ? 1.0f : 0.0f;
    if (m_t == target) return;
    m_t = m_t < target ? std::min(target, m_t + dt * speed) : std::max(target, m_t - dt * speed);
    const float e = m_t * m_t * (3.0f - 2.0f * m_t);
    if (ve::GameObject* leaf = m_leaf ? gameObject().scene().findById(m_leaf) : nullptr)
        leaf->transform().setLocalRotation(glm::angleAxis(glm::radians(-openAngle * e), glm::vec3(0, 1, 0)));
}

void DoorHinge::loadState(const ve::Json& in)
{
    m_open = in.value("open", m_open);
    m_t = m_open ? 0.999f : 0.001f; // jump there (the next update finishes it)
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
    registerRequests();
    buildStatus();
    // The cards' pictures: during the loading screen if there is one.
    if (engine().loading()) {
        ve::Scene* sc = &scene();
        const uint64_t id = this->id();
        engine().addLoadStep("Preparing the build kit", [sc, id] {
            if (auto* b = dynamic_cast<Builder*>(sc->findById(id))) b->bakeIcons();
        });
    } else {
        bakeIcons();
    }
}

void Builder::bakeIcons()
{
    for (Entry& e : m_catalogue) {
        if (e.icon) continue;
        const ve::Json* pj = ve::prefab::load(e.prefab);
        if (!pj) continue;
        const ve::Json f = pj->value("object", ve::Json::object()).value("fields", ve::Json::object());
        const auto kind = Kind(f.value("kind", 0));
        const auto mat = Mat(f.value("material", 0));
        ve::ui::IconView view;
        view.yawDegrees = kind == Kind::Stairs ? 215.0f : 35.0f; // stairs: from the side you climb
        view.pitchDegrees = (kind == Kind::Foundation || kind == Kind::Floor) ? 38.0f : 22.0f;
        e.icon = ve::ui::bakeIcon(renderer(), pieceMeshData(kind, mat), 192, view);
    }
    buildMenu();
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
            m_catalogue.push_back({e.value("name", std::string("?")), e.value("prefab", std::string()), e.value("category", std::string("Pieces")),
                                   e.value("description", std::string()), nullptr});
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

bool Builder::justStopped() const
{
    return m_stoppedFrame == renderer().frameNumber();
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
    if (in.actionPressed("BuildMode")) openMenu(); // (while it's open, B closes it: a menu shortcut)
    if (m_status) m_status->visible = m_active;
    if (!m_active || m_catalogue.empty()) return;
    if (in.keyPressed(ve::Key::Escape)) {
        setActive(false);
        m_stoppedFrame = renderer().frameNumber();
        return;
    }
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
    const BuildPiece* host = nullptr; // what a fitting snapped into
    if (auto s = ve::placement::findSnap(scene(), m_preview.snapPoints(), m_rot, aim, 0.9f, m_preview.object())) {
        m_pos = s->position;
        m_rot = s->rotation; // fittings turn to match their opening
        m_snapped = true;
        host = BuildPiece::of(const_cast<ve::GameObject*>(&s->target->gameObject()));
    }
    const BuildPiece* self = BuildPiece::of(m_preview.object());
    const bool fitting = self && self->isFitting();
    // Fits: nothing in the way (the ground may cut into it: foundations sit on
    // slopes; a fitting's frame may touch the wall it's fitted into), and
    // resting on something: the ground or another piece. Fittings only go into openings.
    const bool fits = ve::placement::fits(physics(), m_preview.volume(), m_pos, m_rot, 0.1f, [&](const ve::RaycastHit& h) {
        if (h.gameObject && dynamic_cast<ve::Terrain*>(h.gameObject)) return true;
        return fitting && host && h.gameObject && BuildPiece::of(h.gameObject) == host;
    });
    m_preview.setPose(m_pos, m_rot);
    m_preview.setValid(fits && (fitting ? m_snapped : (m_snapped || m_aimHit)) && m_preview.ready());
}

ve::GameObject* Builder::place()
{
    if (!m_active || !m_preview.visible() || !m_preview.valid()) return nullptr;
    // Building changes the shared world: the host does it (offline that's us,
    // right now). A client asks and the piece arrives like any other spawn.
    ve::net::Writer w;
    w.writeString(m_catalogue[size_t(m_index)].prefab);
    w.write(m_pos);
    w.writeRotation(m_rot);
    m_placed = nullptr;
    net().toServer("build.place", w);
    return m_placed;
}

void Builder::registerRequests()
{
    ve::net::Network& n = net();
    // Host: a player (or we) placed a piece.
    n.on("build.place", [this](ve::net::PeerId from, ve::net::Reader& r) {
        const std::string prefab = r.readString();
        const auto pos = r.read<glm::vec3>();
        const glm::quat rot = r.readRotation();
        if (r.failed()) return;
        const bool known = std::any_of(m_catalogue.begin(), m_catalogue.end(), [&](const Entry& e) { return e.prefab == prefab; });
        if (!known) {
            ve::log::warn("Build: player {} asked for an unknown piece '{}'", from, prefab);
            return;
        }
        // (A real game re-checks the fit and the player's materials / reach here.)
        m_placed = net().spawn(prefab, pos, rot);
        if (m_placed) {
            rebuildNavigation(pos);
            ve::net::Writer at;
            at.write(pos);
            net().toClients("build.changed", at);
        }
    });
    // Host: remove a piece (and its fittings).
    n.on("build.remove", [this](ve::net::PeerId, ve::net::Reader& r) {
        const auto id = r.read<uint64_t>();
        if (r.failed()) return;
        m_removed = removeNow(scene().findById(id));
    });
    // Clients: pieces came or went near here (the navmesh follows).
    n.on("build.changed", [this](ve::net::PeerId, ve::net::Reader& r) {
        const auto at = r.read<glm::vec3>();
        if (!r.failed()) rebuildNavigation(at);
    });
}

int Builder::remove(ve::GameObject* object)
{
    BuildPiece* piece = BuildPiece::of(object);
    if (!piece || !piece->getComponent<ve::PrefabInstance>()) return 0; // only built pieces
    ve::net::Writer w;
    w.write(piece->id());
    m_removed = 0;
    net().toServer("build.remove", w);
    return net().isClient() ? 1 : m_removed; // (a client: asked; it goes when the host says)
}

int Builder::removeNow(ve::GameObject* object)
{
    BuildPiece* piece = BuildPiece::of(object);
    if (!piece || !piece->getComponent<ve::PrefabInstance>()) return 0; // only built pieces
    // Fittings in its openings go with it (found by their snap points sitting
    // on the openings', so it works for pieces loaded from a save too).
    std::vector<glm::vec3> openings;
    for (const auto& c : piece->components())
        if (auto* sp = dynamic_cast<ve::SnapPoint*>(c.get()); sp && sp->type.find("-opening") != std::string::npos)
            openings.push_back(sp->worldPosition());
    std::vector<BuildPiece*> gone{piece};
    if (!openings.empty()) {
        scene().forEach<ve::SnapPoint>([&](ve::SnapPoint& sp) {
            auto* other = BuildPiece::of(&sp.gameObject());
            if (!other || other == piece || !other->isFitting() || ve::PlacementGhost::is(*other)) return;
            for (const glm::vec3& o : openings)
                if (glm::distance(sp.worldPosition(), o) < 0.05f && std::find(gone.begin(), gone.end(), other) == gone.end())
                    gone.push_back(other);
        });
    }
    const glm::vec3 at = piece->transform().position();
    for (BuildPiece* p : gone) net().despawn(*p); // here and for everyone
    rebuildNavigation(at);
    ve::net::Writer w;
    w.write(at);
    net().toClients("build.changed", w);
    return int(gone.size());
}

void Builder::rebuildNavigation(const glm::vec3& at)
{
    // Villagers walk around (and through the doors of) what you build.
    if (auto* nav = ve::NavMeshSurface::find(scene())) nav->rebuild({at.x - 4.0f, at.z - 4.0f, at.x + 4.0f, at.z + 4.0f});
}

void Builder::onDestroy()
{
    m_preview.hide();
    for (const char* name : {"build.place", "build.remove", "build.changed"}) net().off(name);
}

// -----------------------------------------------------------------------------
// Build menu + status panel (engine UI elements, the game's style)
// -----------------------------------------------------------------------------

bool Builder::menuOpen() const
{
    return m_menu && m_menu->visible;
}

void Builder::openMenu()
{
    if (!m_menu || m_catalogue.empty()) return;
    if (m_category.empty()) showCategory(m_catalogue.front().category);
    m_menu->visible = true;
    m_menu->focus(nullptr);
    input().setMouseCaptured(false);
}

void Builder::closeMenu()
{
    if (!m_menu || !m_menu->visible) return;
    m_menu->visible = false;
    input().setMouseCaptured(true);
}

void Builder::buildMenu()
{
    namespace ui = ve::ui;
    if (!m_menu) {
        m_menu = &addComponent<ui::Canvas>();
        m_menu->modal = true;
        m_menu->order = 20;
        m_menu->tooltipFont = gui::font::body;
        m_menu->onBack = [this] { closeMenu(); };
        m_menu->shortcuts = {{ve::Key::B, ve::GamepadButton::DPadUp, [this] { closeMenu(); }}};
    }
    m_menu->visible = false;
    ui::Element& root = m_menu->root();
    root.clear();
    auto& dim = root.add<ui::Panel>("dim");
    dim.fill();
    dim.color = ui::rgba(0x00000060);
    dim.blocksPointer = true;
    ui::Element& content = gui::window(root, "Build", {1180.0f, 640.0f});

    // Category tabs, in the catalogue's order.
    auto& tabs = content.add<ui::Stack>("tabs");
    tabs.vertical = false;
    tabs.spacing = 8.0f;
    tabs.anchors({0.0f, 0.0f}, {1.0f, 0.0f}).size({0.0f, 46.0f});
    std::vector<std::string> categories;
    for (const Entry& e : m_catalogue)
        if (std::find(categories.begin(), categories.end(), e.category) == categories.end()) categories.push_back(e.category);
    for (const std::string& c : categories) {
        auto& tab = gui::boxButton(tabs, c, [this, c] { showCategory(c); }, 20.0f);
        tab.name = "tab:" + c;
        tab.size({float(c.size()) * 11.0f + 44.0f, 46.0f});
    }

    auto& cards = content.add<ui::Grid>("cards");
    cards.columns = 6;
    cards.cell = {172.0f, 206.0f};
    cards.spacing = {12.0f, 12.0f};
    cards.at({0.0f, 66.0f}).size(cards.contentSize(12));
    m_cards = &cards;

    m_cardInfo = &gui::label(content, "", gui::font::body, 20.0f, gui::color::dim);
    m_cardInfo->anchors({0.0f, 1.0f}, {1.0f, 1.0f}).pivot({0.0f, 1.0f}).at({0.0f, 0.0f}).size({0.0f, 30.0f});
    m_cardInfo->text = "Pick a piece   ·   B / Esc close";
    if (!m_category.empty()) showCategory(m_category);
}

void Builder::showCategory(const std::string& category)
{
    namespace ui = ve::ui;
    m_category = category;
    if (!m_cards) return;
    m_cards->clear();
    for (size_t i = 0; i < m_catalogue.size(); ++i) {
        const Entry& e = m_catalogue[i];
        if (e.category != category) continue;
        auto& card = m_cards->add<ui::Button>("", "piece:" + e.name);
        card.normal = gui::color::slot;
        card.hover = ui::rgba(0x3a2a18f0);
        card.down = ui::rgba(0x5a3e20ff);
        card.borderColor = ui::withAlpha(gui::color::goldDark, 0.6f);
        card.borderWidth = 1.0f;
        card.rounding = 6.0f;
        card.focusBorder = gui::color::gold;
        card.selected = m_active && int(i) == m_index;
        card.tooltip = e.description;
        auto& icon = card.add<ui::Image>("icon");
        icon.anchors({0.0f, 0.0f}, {1.0f, 0.0f}).at({12.0f, 8.0f}).size({12.0f, 148.0f});
        icon.texture = e.icon;
        auto& name = gui::label(card, e.name, gui::font::body, 18.0f, gui::color::text);
        name.anchors({0.0f, 1.0f}, {1.0f, 1.0f}).pivot({0.0f, 1.0f}).at({6.0f, -6.0f}).size({6.0f, 44.0f});
        name.alignX = ui::Align::Center;
        name.wrap = true;
        card.onClick = [this, i] {
            select(int(i));
            setActive(true);
            closeMenu();
        };
        card.onHover = [this, i](bool on) {
            if (on && m_cardInfo) m_cardInfo->text = m_catalogue[i].name + "  —  " + m_catalogue[i].description;
        };
    }
    for (const auto& c : m_menu->root().find("tabs")->children())
        if (auto* tab = dynamic_cast<ui::Button*>(c.get())) tab->selected = tab->name == "tab:" + category;
}

void Builder::buildStatus()
{
    namespace ui = ve::ui;
    m_status = &addComponent<ui::Canvas>();
    m_status->visible = false;
    auto& panel = m_status->root().add<ui::Panel>("building");
    panel.anchor({0.5f, 1.0f}).pivot({0.5f, 1.0f}).at({0.0f, -50.0f}).size({760.0f, 78.0f});
    panel.color = ui::rgba(0x120c08b8);
    panel.borderColor = ui::withAlpha(gui::color::goldDark, 0.7f);
    panel.borderWidth = 1.0f;
    panel.rounding = 6.0f;
    auto& icon = panel.add<ui::Image>("icon");
    icon.anchor({0.0f, 0.5f}).pivot({0.0f, 0.5f}).at({10.0f, 0.0f}).size({64.0f, 64.0f});
    icon.custom = [this](ui::Painter& p, const ui::Rect& r) {
        if (!m_catalogue.empty() && m_catalogue[size_t(m_index)].icon) p.image(m_catalogue[size_t(m_index)].icon, r);
    };
    auto& title = gui::label(panel, "", gui::font::heading, 26.0f, gui::color::gold);
    title.anchors({0.0f, 0.0f}, {1.0f, 0.0f}).at({86.0f, 6.0f}).size({16.0f, 36.0f});
    title.bind = [this]() -> std::string {
        if (m_catalogue.empty()) return {};
        return m_catalogue[size_t(m_index)].name + (m_preview.valid() ? (m_snapped ? "   ·   snapped" : "") : "   ·   blocked");
    };
    auto& keys = gui::label(panel, "LMB place    RMB remove    R rotate    Wheel next    B build menu    Esc stop building", gui::font::body, 18.0f,
                            gui::color::dim);
    keys.anchors({0.0f, 0.0f}, {1.0f, 0.0f}).at({86.0f, 42.0f}).size({16.0f, 28.0f});
}

} // namespace savanna
