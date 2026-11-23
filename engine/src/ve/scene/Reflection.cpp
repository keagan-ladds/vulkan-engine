#include "ve/scene/Reflection.hpp"

#include "ve/scene/Components.hpp"
#include "ve/scene/Decal.hpp"
#include "ve/scene/MeshComponents.hpp"
#include "ve/scene/Prefab.hpp"

#include <algorithm>

namespace ve {

TypeRegistry& TypeRegistry::get()
{
    static TypeRegistry registry;
    return registry;
}

TypeInfo* TypeRegistry::resolve(Entry& e)
{
    if (!e.built) {
        e.built = true;
        e.build(e.info);
    }
    return &e.info;
}

const TypeInfo* TypeRegistry::find(const std::string& name)
{
    auto it = m_byName.find(name);
    return it == m_byName.end() ? nullptr : resolve(*it->second);
}

const TypeInfo* TypeRegistry::find(std::type_index type)
{
    auto it = m_byType.find(type);
    return it == m_byType.end() ? nullptr : resolve(*it->second);
}

const TypeInfo* TypeRegistry::of(const Component& c) { return find(std::type_index(typeid(c))); }
const TypeInfo* TypeRegistry::of(const GameObject& o) { return find(std::type_index(typeid(o))); }

std::vector<const TypeInfo*> TypeRegistry::all(bool components)
{
    std::vector<const TypeInfo*> out;
    for (auto& e : m_entries) {
        if (e->info.isComponent == components && !e->info.isData) out.push_back(resolve(*e));
    }
    std::sort(out.begin(), out.end(), [](const TypeInfo* a, const TypeInfo* b) {
        return a->category != b->category ? a->category < b->category : a->name < b->name;
    });
    return out;
}

// -----------------------------------------------------------------------------
// Scene types. Every other module registers its own (registerPhysicsTypes(),
// registerAudioTypes(), ...: declared in the module's header, defined at the end
// of its .cpp); the Engine calls them all, so scene doesn't include the modules
// above it.
// -----------------------------------------------------------------------------

void registerBuiltinTypes()
{
    static bool done = false;
    if (done) return;
    done = true;
    TypeRegistry& r = TypeRegistry::get();
    r.add<GameObject>("GameObject", [](TypeBuilder<GameObject>& t) { t.category("General"); });
    r.add<Camera>("Camera", [](TypeBuilder<Camera>& t) {
        t.category("Rendering");
        t.field("fovDegrees", &Camera::fovDegrees).range(10.0f, 140.0f);
        t.field("nearPlane", &Camera::nearPlane).range(0.01f, 10.0f).speed(0.01f);
        t.field("farPlane", &Camera::farPlane).range(10.0f, 20000.0f).speed(5.0f);
        t.field("priority", &Camera::priority);
    });
    r.add<DirectionalLight>("DirectionalLight", [](TypeBuilder<DirectionalLight>& t) {
        t.category("Rendering");
        t.field("color", &DirectionalLight::color).color();
        t.field("intensity", &DirectionalLight::intensity).range(0.0f, 20.0f);
        t.field("castShadows", &DirectionalLight::castShadows);
    });
    r.add<PointLight>("PointLight", [](TypeBuilder<PointLight>& t) {
        t.category("Rendering");
        t.field("color", &PointLight::color).color();
        t.field("intensity", &PointLight::intensity).range(0.0f, 200.0f).speed(0.2f);
        t.field("range", &PointLight::range).range(0.1f, 100.0f);
        t.field("castShadows", &PointLight::castShadows);
        t.field("shadowBias", &PointLight::shadowBias).range(0.0f, 0.2f).speed(0.001f);
    });
    r.add<MeshRenderer>("MeshRenderer", [](TypeBuilder<MeshRenderer>& t) {
        t.category("Rendering");
        // (mesh and materials are set by code: ModelInstance, PrimitiveRenderer, game objects)
        t.field("static", &MeshRenderer::isStatic).tooltip("Never moves: batched on the GPU (static batching)");
        t.field("castShadows", &MeshRenderer::castShadows);
        t.field("visible", &MeshRenderer::visible);
        t.onChanged([](MeshRenderer& m) { m.refresh(); });
    });
    r.add<DecalProjector>("DecalProjector", [](TypeBuilder<DecalProjector>& t) {
        t.category("Rendering");
        // (the box is the transform: projects along -Z, scale = width, height, depth)
        t.field("albedo", &DecalProjector::albedo).assets(".png .jpg .jpeg .tga").tooltip("rgb colour, a = opacity (sRGB)");
        t.field("normal", &DecalProjector::normal).assets(".png .jpg .jpeg .tga").tooltip("tangent-space normal map (linear)");
        t.field("mask", &DecalProjector::mask).assets(".png .jpg .jpeg .tga").tooltip("G = roughness, B = metallic, A = opacity (linear)");
        t.field("color", &DecalProjector::color).color().tooltip("tint; alpha = overall opacity");
        t.enumField("blend", &DecalProjector::blend, {"Normal", "Multiply"});
        t.field("albedoOpacity", &DecalProjector::albedoOpacity).range(0.0f, 1.0f).speed(0.01f);
        t.field("normalOpacity", &DecalProjector::normalOpacity).range(0.0f, 1.0f).speed(0.01f);
        t.field("normalStrength", &DecalProjector::normalStrength).range(0.0f, 4.0f).speed(0.01f);
        t.field("flatten", &DecalProjector::flatten).range(0.0f, 1.0f).speed(0.01f).tooltip("smooths out the surface's own bumps (water)");
        t.field("roughness", &DecalProjector::roughness).range(0.0f, 1.0f).speed(0.01f);
        t.field("roughnessOpacity", &DecalProjector::roughnessOpacity).range(0.0f, 1.0f).speed(0.01f);
        t.field("metallic", &DecalProjector::metallic).range(0.0f, 1.0f).speed(0.01f);
        t.field("metallicOpacity", &DecalProjector::metallicOpacity).range(0.0f, 1.0f).speed(0.01f);
        t.field("angleFadeStart", &DecalProjector::angleFadeStart).range(0.0f, 180.0f).tooltip("degrees from the box's +Z");
        t.field("angleFadeEnd", &DecalProjector::angleFadeEnd).range(0.0f, 180.0f);
        t.field("depthFade", &DecalProjector::depthFade).range(0.0f, 1.0f).speed(0.01f);
        t.field("drawDistance", &DecalProjector::drawDistance).range(1.0f, 2000.0f).speed(1.0f);
        t.field("sortOrder", &DecalProjector::sortOrder).tooltip("higher draws on top");
        t.field("layers", &DecalProjector::layers).tooltip("bit mask: 1 = world, 2 = characters");
        t.onChanged([](DecalProjector& d) { d.reload(); });
    });
    r.add<ModelInstance>("ModelInstance", [](TypeBuilder<ModelInstance>& t) {
        t.category("Rendering");
        t.field("path", &ModelInstance::path).assets(".gltf .glb");
        t.field("staticColliders", &ModelInstance::staticColliders).tooltip("Exact mesh colliders on every node");
        t.field("hiddenNodes", &ModelInstance::hiddenNodes).tooltip("Comma-separated node names to leave out, e.g. Door");
        t.field("animation", &ModelInstance::animation).tooltip("Animated models: the clip to loop (empty = the first)");
        t.field("markers", &ModelInstance::markers).tooltip("Nodes named SNAP_*, BOX_*, EXCL_* become snap points, placement boxes, foliage exclusions");
        t.onChanged([](ModelInstance& m) { m.rebuild(); });
    });
    r.add<PrefabInstance>("PrefabInstance", [](TypeBuilder<PrefabInstance>& t) {
        t.category("Scene");
        t.field("prefab", &PrefabInstance::prefab).assets(".prefab").tooltip("the prefab this object was made from");
        t.field("saveInGame", &PrefabInstance::saveInGame).tooltip("spawned while playing: save games recreate it");
    });
    r.add<PrimitiveRenderer>("PrimitiveRenderer", [](TypeBuilder<PrimitiveRenderer>& t) {
        t.category("Rendering");
        t.enumField("shape", &PrimitiveRenderer::shape, {"Cube", "Sphere", "Cylinder", "Plane"});
        t.field("size", &PrimitiveRenderer::size).range(0.01f, 1000.0f);
        t.field("color", &PrimitiveRenderer::color).color();
        t.field("roughness", &PrimitiveRenderer::roughness).range(0.0f, 1.0f).speed(0.01f);
        t.field("metallic", &PrimitiveRenderer::metallic).range(0.0f, 1.0f).speed(0.01f);
        t.field("texture", &PrimitiveRenderer::texture).assets(".png .jpg .jpeg .tga");
        t.field("uvScale", &PrimitiveRenderer::uvScale);
        t.field("castShadows", &PrimitiveRenderer::castShadows);
        t.field("static", &PrimitiveRenderer::isStatic).tooltip("Never moves: batched on the GPU");
        t.onChanged([](PrimitiveRenderer& p) { p.rebuild(); });
    });
}

} // namespace ve
