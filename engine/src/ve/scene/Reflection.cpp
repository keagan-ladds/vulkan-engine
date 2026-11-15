#include "ve/scene/Reflection.hpp"

#include "ve/assets/AssetManager.hpp"
#include "ve/audio/Audio.hpp"
#include "ve/core/Engine.hpp"
#include "ve/physics/Physics.hpp"
#include "ve/scene/Components.hpp"
#include "ve/scene/MeshComponents.hpp"

#include <algorithm>

namespace ve {

namespace detail {
std::shared_ptr<AudioClip> loadAudioClip(const std::string& path)
{
    try {
        return Engine::get().assets().loadAudio(path);
    } catch (const std::exception&) {
        return nullptr;
    }
}
std::string audioClipPath(const std::shared_ptr<AudioClip>& clip)
{
    return clip ? clip->path() : std::string();
}
} // namespace detail

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
        if (e->info.isComponent == components) out.push_back(resolve(*e));
    }
    std::sort(out.begin(), out.end(), [](const TypeInfo* a, const TypeInfo* b) {
        return a->category != b->category ? a->category < b->category : a->name < b->name;
    });
    return out;
}

// -----------------------------------------------------------------------------
// Engine types
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
    r.add<ModelInstance>("ModelInstance", [](TypeBuilder<ModelInstance>& t) {
        t.category("Rendering");
        t.field("path", &ModelInstance::path).assets(".gltf .glb");
        t.field("staticColliders", &ModelInstance::staticColliders).tooltip("Exact mesh colliders on every node");
        t.field("hiddenNodes", &ModelInstance::hiddenNodes).tooltip("Comma-separated node names to leave out, e.g. Door");
        t.onChanged([](ModelInstance& m) { m.rebuild(); });
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
        t.onChanged([](PrimitiveRenderer& p) { p.rebuild(); });
    });

    r.add<Rigidbody>("Rigidbody", [](TypeBuilder<Rigidbody>& t) {
        t.category("Physics");
        t.enumField("type", &Rigidbody::type, {"Static", "Kinematic", "Dynamic"});
        t.enumField("shape", &Rigidbody::shape, {"Custom (code)", "Box", "Sphere", "Capsule", "Cylinder"});
        t.field("shapeSize", &Rigidbody::shapeSize).range(0.01f, 1000.0f).tooltip("Box: full size. Sphere: x = diameter. Capsule/Cylinder: x = diameter, y = height");
        t.field("shapeOffset", &Rigidbody::shapeOffset);
        t.field("mass", &Rigidbody::mass).range(0.0f, 100000.0f).tooltip("kg; 0 = from volume");
        t.field("friction", &Rigidbody::friction).range(0.0f, 2.0f).speed(0.01f);
        t.field("restitution", &Rigidbody::restitution).range(0.0f, 1.0f).speed(0.01f);
        t.field("linearDamping", &Rigidbody::linearDamping).range(0.0f, 10.0f).speed(0.01f);
        t.field("angularDamping", &Rigidbody::angularDamping).range(0.0f, 10.0f).speed(0.01f);
        t.field("continuousCollision", &Rigidbody::continuousCollision);
        t.onChanged([](Rigidbody& rb) { rb.rebuild(); });
    });

    r.add<AudioSource>("AudioSource", [](TypeBuilder<AudioSource>& t) {
        t.category("Audio");
        t.field("clip", &AudioSource::clip);
        t.field("bus", &AudioSource::bus);
        t.field("volume", &AudioSource::volume).range(0.0f, 2.0f).speed(0.01f);
        t.field("pitch", &AudioSource::pitch).range(0.1f, 4.0f).speed(0.01f);
        t.field("loop", &AudioSource::loop);
        t.field("playOnStart", &AudioSource::playOnStart);
        t.field("spatial", &AudioSource::spatial);
        t.field("minDistance", &AudioSource::minDistance).range(0.0f, 1000.0f);
        t.field("maxDistance", &AudioSource::maxDistance).range(0.1f, 1000.0f);
        t.field("occlusion", &AudioSource::occlusion);
        t.field("occlusionVolume", &AudioSource::occlusionVolume).range(0.0f, 1.0f).speed(0.01f);
        t.field("occlusionLowpass", &AudioSource::occlusionLowpass).range(50.0f, 20000.0f).speed(10.0f);
        t.onChanged([](AudioSource& s) { s.applySettings(); });
    });
    r.add<AudioZone>("AudioZone", [](TypeBuilder<AudioZone>& t) {
        t.category("Audio");
        t.enumField("shape", &AudioZone::shape, {"Box", "Sphere", "Cylinder"});
        t.field("halfExtents", &AudioZone::halfExtents).range(0.0f, 1000.0f);
        t.field("radius", &AudioZone::radius).range(0.0f, 1000.0f);
        t.field("height", &AudioZone::height).range(0.0f, 1000.0f);
        t.field("fadeTime", &AudioZone::fadeTime).range(0.0f, 10.0f).speed(0.01f);
        t.property(
             "effects", FieldKind::Json,
             [](const AudioZone& z) {
                 Json a = Json::array();
                 for (const auto& e : z.effects) a.push_back({{"bus", e.bus}, {"volume", e.volume}, {"lowpass", e.lowpass}});
                 return a;
             },
             [](AudioZone& z, const Json& j) {
                 if (!j.is_array()) return;
                 z.effects.clear();
                 for (const auto& e : j) {
                     AudioZone::Effect fx;
                     fx.bus = e.value("bus", std::string("Weather"));
                     fx.volume = e.value("volume", 1.0f);
                     fx.lowpass = e.value("lowpass", AudioSystem::kNoFilter);
                     z.effects.push_back(fx);
                 }
             })
            .tooltip("[{\"bus\": \"Weather\", \"volume\": 0.3, \"lowpass\": 450}]");
    });
}

} // namespace ve
