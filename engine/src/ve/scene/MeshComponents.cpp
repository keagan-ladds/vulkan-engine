#include "ve/scene/MeshComponents.hpp"

#include "ve/anim/Animation.hpp"

#include "ve/assets/AssetManager.hpp"
#include "ve/assets/Model.hpp"
#include "ve/core/Log.hpp"
#include "ve/core/Strings.hpp"
#include "ve/render/Material.hpp"
#include "ve/render/Mesh.hpp"
#include "ve/render/Primitives.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/render/Texture.hpp"
#include "ve/scene/Components.hpp"
#include "ve/scene/Scene.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <functional>
#include <map>
#include <tuple>

namespace ve {

// -----------------------------------------------------------------------------
// MeshRenderer (Components.hpp): static renderers
// -----------------------------------------------------------------------------

void MeshRenderer::refresh()
{
    // Not spawned yet: start() queues it. Otherwise the renderer re-checks it
    // (batch it, move it to another batch, or draw it the normal way again).
    Scene::refreshStatic(*this);
}

// -----------------------------------------------------------------------------
// SpotLight (Components.hpp): the cookie mask
// -----------------------------------------------------------------------------

void SpotLight::reload()
{
    if (cookie == m_loadedCookie) return;
    m_loadedCookie = cookie;
    cookieTexture.reset(); // (a path that was cleared drops its texture)
    if (cookie.empty()) return;
    try {
        cookieTexture = assets().loadTexture(cookie);
    } catch (const std::exception& e) {
        log::warn("SpotLight on '{}': {}", gameObject().name(), e.what());
    }
}

// -----------------------------------------------------------------------------
// ModelInstance
// -----------------------------------------------------------------------------

namespace {

std::unordered_map<std::string, ModelMarkerHandler>& markerHandlers()
{
    static std::unordered_map<std::string, ModelMarkerHandler> handlers;
    return handlers;
}

// Blender names duplicates "Name.001": markers ignore the suffix.
std::string withoutNumberSuffix(std::string name)
{
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos && dot + 1 < name.size() &&
        std::all_of(name.begin() + long(dot) + 1, name.end(), [](unsigned char c) { return std::isdigit(c); }))
        name.resize(dot);
    return name;
}

// A node that is a registered marker kind: hand it to that kind's handler.
void applyMarker(GameObject& go, const ModelNode& node, const Model& model)
{
    const std::string name = withoutNumberSuffix(node.name);
    const size_t sep = name.find('_');
    const auto& handlers = markerHandlers();
    const auto it = handlers.find(name.substr(0, sep)); // "SNAP_x" and "SNAP" ("SNAPSHOT" is no marker)
    if (it == handlers.end()) return;

    ModelMarker marker{go, node, model, {}, AABB{glm::vec3(-1.0f), glm::vec3(1.0f)}, nlohmann::json::object()};
    if (sep != std::string::npos) marker.args = str::split(std::string_view(name).substr(sep + 1), '_');
    if (!node.extras.empty()) {
        nlohmann::json j = nlohmann::json::parse(node.extras, nullptr, false);
        if (j.is_object()) marker.extras = std::move(j);
    }
    if (node.mesh >= 0 && size_t(node.mesh) < model.meshes.size() && model.meshes[size_t(node.mesh)])
        marker.bounds = model.meshes[size_t(node.mesh)]->bounds();
    // Not drawn or solid: whatever instantiate() put on the node (renderer,
    // static collider) goes; the marker's own components come next.
    std::vector<Component*> made;
    for (const auto& c : go.components()) made.push_back(c.get());
    for (Component* c : made) go.removeComponent(c);
    it->second(marker);
}

} // namespace

void registerModelMarker(const std::string& kind, ModelMarkerHandler handler)
{
    markerHandlers()[kind] = std::move(handler);
}

GameObject* ModelInstance::instanceRoot() const
{
    return m_root.get(gameObject().scene());
}

void ModelInstance::rebuild()
{
    if (GameObject* old = instanceRoot()) old->destroy();
    m_root.reset();
    if (path.empty()) return;
    try {
        auto model = assets().loadModel(path);
        GameObject* root = &gameObject().scene().instantiate(*model, &gameObject(), staticColliders);
        m_root = root;
        if (markers && !markerHandlers().empty()) {
            // instantiate() makes one child per node, in the model's order.
            std::function<void(int, GameObject&)> walk = [&](int index, GameObject& go) {
                const ModelNode& node = model->nodes[size_t(index)];
                applyMarker(go, node, *model);
                const auto& kids = go.transform().children();
                for (size_t i = 0; i < node.children.size() && i < kids.size(); ++i) walk(node.children[i], kids[i]->gameObject());
            };
            const auto& roots = root->transform().children();
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
            hide(root->transform());
        }
        auto* animator = root->getComponent<Animator>();
        if (animator && !animation.empty()) animator->clip = animation;
        // Buildings (exact static colliders) never move: their nodes' renderers
        // are batched on the GPU (MeshRenderer::isStatic). Animated models move.
        if (staticColliders && !animator) root->setStatic(true);
    } catch (const std::exception& e) {
        log::warn("ModelInstance on '{}': {}", gameObject().name(), e.what());
    }
}

void ModelInstance::onDestroy()
{
    // The instantiated nodes are children: they go with the owner. When only
    // this component is removed, remove them too.
    if (GameObject* root = instanceRoot(); root && !gameObject().isDestroyed()) root->destroy();
    m_root.reset();
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
    const std::tuple<Shape, float, float, float> key{shape, s.x, s.y, s.z};
    auto found = shared.find(key);
    std::shared_ptr<Mesh> mesh = found != shared.end() ? found->second.lock() : nullptr;
    if (!mesh) {
        // About to add one: drop entries whose meshes are gone (sizes edited
        // in the editor would otherwise pile up).
        std::erase_if(shared, [](const auto& entry) { return entry.second.expired(); });
        MeshData md;
        switch (shape) {
        case Shape::Cube: md = primitives::cube(s); break;
        case Shape::Sphere: md = primitives::sphere(s.x * 0.5f); break;
        case Shape::Cylinder: md = primitives::cylinder(s.x * 0.5f, s.x * 0.5f, s.y, 24); break;
        case Shape::Plane: md = primitives::plane(s.x, s.z); break;
        }
        mesh = Mesh::create(r, md, {}, "primitive");
        shared[key] = mesh;
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
    MeshRenderer* mr = m_renderer.get(gameObject().scene());
    if (!mr) m_renderer = mr = &gameObject().addComponent<MeshRenderer>();
    mr->mesh = mesh;
    mr->materials = {m_material};
    mr->castShadows = castShadows;
    mr->isStatic = isStatic;
    mr->refresh(); // (a static one's mesh or material may have changed)
}

void PrimitiveRenderer::onDestroy()
{
    if (MeshRenderer* mr = m_renderer.get(gameObject().scene()); mr && !gameObject().isDestroyed()) gameObject().removeComponent(mr);
    m_renderer.reset();
}

} // namespace ve
