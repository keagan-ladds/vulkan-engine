#include "ve/scene/MeshComponents.hpp"

#include "ve/anim/Animation.hpp"

#include "ve/assets/AssetManager.hpp"
#include "ve/assets/Model.hpp"
#include "ve/core/Log.hpp"
#include "ve/core/Strings.hpp"
#include "ve/gameplay/Placement.hpp"
#include "ve/physics/Physics.hpp"
#include "ve/render/Material.hpp"
#include "ve/render/Mesh.hpp"
#include "ve/render/Primitives.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/scene/Components.hpp"
#include "ve/scene/Scene.hpp"
#include "ve/world/Vegetation.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <tuple>

namespace ve {

// -----------------------------------------------------------------------------
// ModelInstance
// -----------------------------------------------------------------------------

namespace {

// Blender names duplicates "Name.001": markers ignore the suffix.
std::string withoutNumberSuffix(std::string name)
{
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos && dot + 1 < name.size() &&
        std::all_of(name.begin() + long(dot) + 1, name.end(), [](unsigned char c) { return std::isdigit(c); }))
        name.resize(dot);
    return name;
}

// "SNAP_door_door-opening" for kind "SNAP" -> {"door", "door-opening"}; nothing if it isn't that kind.
std::optional<std::vector<std::string>> markerArgs(const std::string& name, const char* kind)
{
    const size_t k = std::strlen(kind);
    if (name.compare(0, k, kind) != 0 || (name.size() > k && name[k] != '_')) return std::nullopt;
    return name.size() > k ? str::split(std::string_view(name).substr(k + 1), '_') : std::vector<std::string>{};
}

// "fence+grid" -> "fence, grid"; "any" -> "*"; "none" -> "".
std::string acceptsList(const std::string& arg)
{
    if (arg == "any") return "*";
    if (arg == "none") return "";
    std::string out;
    for (const std::string& t : str::split(arg, '+')) out += (out.empty() ? "" : ", ") + t;
    return out;
}

// A SNAP / BOX / EXCL node (see MeshComponents.hpp): add the component it stands
// for. Markers aren't drawn or solid; a mesh on one only gives its size.
void applyMarker(GameObject& go, const ModelNode& node, const Model& model)
{
    const std::string name = withoutNumberSuffix(node.name);
    auto snap = markerArgs(name, "SNAP");
    const auto box = markerArgs(name, "BOX");
    const auto excl = markerArgs(name, "EXCL");
    if (!snap && !box && !excl) return;

    nlohmann::json extras = node.extras.empty() ? nlohmann::json::object() : nlohmann::json::parse(node.extras, nullptr, false);
    if (!extras.is_object()) extras = nlohmann::json::object();
    auto extra = [&](const char* key, auto fallback) {
        const auto it = extras.find(key);
        if (it == extras.end()) return fallback;
        try {
            return it->template get<decltype(fallback)>();
        } catch (const nlohmann::json::exception&) {
            log::warn("Model '{}', node '{}': custom property '{}' has the wrong type", model.name, node.name, key);
            return fallback;
        }
    };

    AABB bounds{glm::vec3(-1.0f), glm::vec3(1.0f)}; // a cube empty (display size 1)
    if (node.mesh >= 0 && size_t(node.mesh) < model.meshes.size() && model.meshes[size_t(node.mesh)]) {
        bounds = model.meshes[size_t(node.mesh)]->bounds();
        std::vector<Component*> drawn;
        for (const auto& c : go.components())
            if (dynamic_cast<MeshRenderer*>(c.get()) || dynamic_cast<Rigidbody*>(c.get())) // skinned ones too
                drawn.push_back(c.get());
        for (Component* c : drawn) go.removeComponent(c);
    }
    const glm::vec3 scale = go.transform().localScale();

    if (snap) {
        auto& sp = go.addComponent<SnapPoint>();
        std::vector<std::string>& a = *snap;
        sp.alignRotation = !a.empty() && a.back() == "align";
        if (sp.alignRotation) a.pop_back();
        if (a.size() > 2) log::warn("Model '{}', node '{}': snap types can't contain '_' (use '-')", model.name, node.name);
        sp.type = a.empty() ? "grid" : a[0];
        sp.accepts = a.size() > 1 ? acceptsList(a[1]) : sp.type;
        sp.type = extra("type", sp.type);
        sp.accepts = extra("accepts", sp.accepts);
        sp.alignRotation = extra("align", sp.alignRotation);
    } else if (box) {
        // PlacementBox sizes aren't scaled by the object: move the scale into the box.
        auto& b = go.addComponent<PlacementBox>();
        b.center = bounds.center() * scale;
        b.halfExtents = bounds.extents() * glm::abs(scale);
        go.transform().setLocalScale(glm::vec3(1.0f));
    } else if (excl) {
        auto& e = go.addComponent<FoliageExclusion>(); // its box follows the object's scale itself
        e.box = std::find(excl->begin(), excl->end(), "circle") == excl->end();
        e.size = glm::vec2(bounds.extents().x, bounds.extents().z) * 2.0f;
        e.offset = glm::vec2(bounds.center().x, bounds.center().z);
        e.radius = std::max(bounds.extents().x * std::abs(scale.x), bounds.extents().z * std::abs(scale.z));
        e.radius = extra("radius", e.radius);
        e.falloff = extra("falloff", e.falloff);
        e.grass = extra("grass", e.grass);
        e.foliage = extra("foliage", e.foliage);
    }
}

} // namespace

void ModelInstance::rebuild()
{
    if (m_root) {
        m_root->destroy();
        m_root = nullptr;
    }
    if (path.empty()) return;
    try {
        auto model = assets().loadModel(path);
        m_root = &gameObject().scene().instantiate(*model, &gameObject(), staticColliders);
        if (markers) {
            // instantiate() makes one child per node, in the model's order.
            std::function<void(int, GameObject&)> walk = [&](int index, GameObject& go) {
                const ModelNode& node = model->nodes[size_t(index)];
                applyMarker(go, node, *model);
                const auto& kids = go.transform().children();
                for (size_t i = 0; i < node.children.size() && i < kids.size(); ++i) walk(node.children[i], kids[i]->gameObject());
            };
            const auto& roots = m_root->transform().children();
            for (size_t i = 0; i < model->roots.size() && i < roots.size(); ++i) walk(model->roots[i], roots[i]->gameObject());
        }
        if (!hiddenNodes.empty()) {
            const std::vector<std::string> names = str::split(hiddenNodes, ',');
            // Destroyed before they start: no colliders or draws for hidden nodes.
            std::function<void(Transform&)> hide = [&](Transform& t) {
                for (Transform* c : t.children()) {
                    if (std::find(names.begin(), names.end(), c->gameObject().name()) != names.end()) c->gameObject().destroy();
                    else hide(*c);
                }
            };
            hide(m_root->transform());
        }
        if (auto* animator = m_root->getComponent<Animator>(); animator && !animation.empty()) animator->clip = animation;
    } catch (const std::exception& e) {
        log::warn("ModelInstance on '{}': {}", gameObject().name(), e.what());
    }
}

void ModelInstance::onDestroy()
{
    // The instantiated nodes are children: they go with the owner. When only
    // this component is removed, remove them too.
    if (m_root && !gameObject().isDestroyed()) m_root->destroy();
    m_root = nullptr;
}

// -----------------------------------------------------------------------------
// PrimitiveRenderer
// -----------------------------------------------------------------------------

void PrimitiveRenderer::rebuild()
{
    Renderer& r = renderer();
    const glm::vec3 s = glm::max(size, glm::vec3(0.001f));
    // Primitives of the same shape and size share one mesh (a crate pyramid is
    // one cube), kept alive by the renderers using it.
    static std::map<std::tuple<Shape, float, float, float>, std::weak_ptr<Mesh>> shared;
    std::weak_ptr<Mesh>& slot = shared[{shape, s.x, s.y, s.z}];
    std::shared_ptr<Mesh> mesh = slot.lock();
    if (!mesh) {
        MeshData md;
        switch (shape) {
        case Shape::Cube: md = primitives::cube(s); break;
        case Shape::Sphere: md = primitives::sphere(s.x * 0.5f); break;
        case Shape::Cylinder: md = primitives::cylinder(s.x * 0.5f, s.x * 0.5f, s.y, 24); break;
        case Shape::Plane: md = primitives::plane(s.x, s.z); break;
        }
        mesh = Mesh::create(r, md, {}, "primitive");
        slot = mesh;
    }
    if (!m_material) m_material = r.createStandardMaterial();
    m_material->setBaseColor(color).setRoughness(roughness).setMetallic(metallic).setUvScale(uvScale);
    std::shared_ptr<Texture> tex;
    if (!texture.empty()) {
        try {
            tex = assets().loadTexture(texture);
        } catch (const std::exception& e) {
            log::warn("PrimitiveRenderer on '{}': {}", gameObject().name(), e.what());
        }
    }
    m_material->setAlbedoTexture(tex);
    if (!m_renderer) m_renderer = &gameObject().addComponent<MeshRenderer>();
    m_renderer->mesh = mesh;
    m_renderer->materials = {m_material};
    m_renderer->castShadows = castShadows;
}

void PrimitiveRenderer::onDestroy()
{
    if (m_renderer && !gameObject().isDestroyed()) gameObject().removeComponent(m_renderer);
    m_renderer = nullptr;
}

} // namespace ve
