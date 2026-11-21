#include "ve/world/Vegetation.hpp"

#include "ve/gameplay/Placement.hpp"

#include "ve/core/Engine.hpp"
#include "ve/core/Profiler.hpp"

#include "ve/assets/AssetManager.hpp"
#include "ve/core/Log.hpp"
#include "ve/core/Strings.hpp"
#include "ve/render/Impostor.hpp"
#include "ve/render/Material.hpp"
#include "ve/render/Mesh.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/render/Texture.hpp"
#include "ve/scene/Components.hpp"
#include "ve/scene/MeshComponents.hpp"
#include "ve/scene/Reflection.hpp"
#include "ve/scene/Scene.hpp"
#include "ve/scene/SceneSerializer.hpp"
#include "ve/world/Grass.hpp"
#include "ve/world/LodModel.hpp"
#include "ve/world/Noise.hpp"
#include "ve/world/Terrain.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

namespace ve {

// -----------------------------------------------------------------------------
// MeshLibrary
// -----------------------------------------------------------------------------

namespace {
struct LibraryEntry {
    MeshLibrary::Builder builder;
    std::shared_ptr<const FoliageMeshes> built;
};
std::map<std::string, LibraryEntry>& library()
{
    static std::map<std::string, LibraryEntry> entries;
    return entries;
}

// Every Component of type T on `root` and its descendants, in hierarchy order.
template <class T>
void collect(GameObject& root, std::vector<T*>& out)
{
    if (!root.activeInHierarchy()) return;
    for (const auto& c : root.components())
        if (auto* t = dynamic_cast<T*>(c.get()); t && t->enabled) out.push_back(t);
    for (Transform* child : root.transform().children()) collect(child->gameObject(), out);
}

std::string fieldsOf(const char* type, const void* object)
{
    const TypeInfo* t = TypeRegistry::get().find(type);
    return t ? SceneSerializer::serializeFields(*t, object).dump() : std::string();
}

// Default layer seed from its name: FNV-1a (32 bit), the same on every
// platform and standard library (std::hash isn't). Never 0 (= "unset").
uint32_t nameSeed(const std::string& name)
{
    uint32_t h = 2166136261u;
    for (const char c : name) {
        h ^= static_cast<uint8_t>(c);
        h *= 16777619u;
    }
    return h != 0 ? h : 1u;
}

// Active foliage exclusions (ghost previews of building pieces don't count).
// GrassLayer::exclusions() is the grass version.
std::vector<FoliageExclusionShape> exclusionShapes(const Scene& scene)
{
    std::vector<FoliageExclusionShape> out;
    scene.forEach<FoliageExclusion>([&](FoliageExclusion& e) {
        if (!e.isActive() || !e.foliage || PlacementGhost::is(e.gameObject())) return;
        out.push_back(e.shape());
    });
    return out;
}

// The area where two exclusion lists differ (x0, z0, x1, z1), padded; empty (x0 > x1) if they're the same.
glm::vec4 changedArea(const std::vector<FoliageExclusionShape>& a, const std::vector<FoliageExclusionShape>& b, float pad)
{
    glm::vec4 area(1e30f, 1e30f, -1e30f, -1e30f);
    auto add = [&](const std::vector<FoliageExclusionShape>& from, const std::vector<FoliageExclusionShape>& other) {
        for (const auto& e : from) {
            if (std::find(other.begin(), other.end(), e) != other.end()) continue;
            const glm::vec4 bb = e.bounds();
            area = {std::min(area.x, bb.x - pad), std::min(area.y, bb.y - pad), std::max(area.z, bb.z + pad), std::max(area.w, bb.w + pad)};
        }
    };
    add(a, b);
    add(b, a);
    return area;
}
} // namespace

FoliageExclusionShape FoliageExclusion::shape() const
{
    FoliageExclusionShape e;
    const Transform& t = transform();
    const glm::vec3 c = t.transformPoint(glm::vec3(offset.x, 0.0f, offset.y));
    e.center = {c.x, c.z};
    e.radius = radius;
    e.box = box;
    const glm::vec3 right = t.rotation() * glm::vec3(1, 0, 0);
    e.yaw = std::atan2(-right.z, right.x);
    const glm::vec3 sc = t.localScale();
    e.halfExtents = size * 0.5f * glm::vec2(std::abs(sc.x), std::abs(sc.z));
    return e;
}

// "a:1, b:2" -> {{"a", 1}, {"b", 2}}; a bare name means 1.
std::vector<std::pair<std::string, float>> parseNameValues(const std::string& spec)
{
    std::vector<std::pair<std::string, float>> out;
    for (const std::string& item : str::split(spec, ',')) {
        const size_t colon = item.rfind(':');
        float value = 1.0f;
        std::string name = item;
        if (colon != std::string::npos) {
            name = str::trim(std::string_view(item).substr(0, colon));
            try {
                value = std::stof(item.substr(colon + 1));
            } catch (const std::exception&) {
                value = 1.0f;
            }
        }
        out.push_back({name, value});
    }
    return out;
}

void MeshLibrary::add(const std::string& name, Builder builder)
{
    library()[name] = {std::move(builder), {}};
}

std::shared_ptr<const FoliageMeshes> MeshLibrary::get(Renderer& renderer, const std::string& name)
{
    auto it = library().find(name);
    if (it == library().end()) return nullptr;
    if (it->second.built) return it->second.built;
    // The built meshes live in a static table: drop them at Engine shutdown,
    // before the renderer goes (hooked once per Engine, when the first is built).
    static bool releaseHooked = false;
    if (!releaseHooked && Engine::exists()) {
        releaseHooked = true;
        Engine::get().atShutdown([] {
            releaseMeshes();
            releaseHooked = false;
        });
    }
    auto meshes = std::make_shared<const FoliageMeshes>(it->second.builder(renderer));
    it->second.built = meshes;
    return meshes;
}

void MeshLibrary::releaseMeshes()
{
    for (auto& [name, e] : library()) e.built.reset();
}

std::vector<std::string> MeshLibrary::names()
{
    std::vector<std::string> out;
    for (const auto& [name, e] : library()) out.push_back(name);
    return out;
}

std::vector<std::pair<int, float>> parseBiomeWeights(const std::string& spec, const TerrainData& terrain,
                                                     const std::string& owner)
{
    std::vector<std::pair<int, float>> out;
    for (const auto& [name, value] : parseNameValues(spec)) {
        const int index = name == "*" ? -1 : terrain.biomeIndex(name);
        if (index < 0 && name != "*") {
            log::warnOnce(owner + ": the terrain has no biome '" + name + "'");
            continue;
        }
        out.push_back({index, value});
    }
    return out;
}

float biomeDensity(const std::vector<std::pair<int, float>>& weights, const TerrainData& terrain, glm::vec2 p)
{
    if (weights.empty()) return 1.0f;
    float d = 0.0f;
    for (const auto& [biome, k] : weights) d += biome < 0 ? k : k * terrain.biomeWeight(biome, p.x, p.y);
    return std::clamp(d, 0.0f, 1.0f);
}

// -----------------------------------------------------------------------------
// FoliageType
// -----------------------------------------------------------------------------

FoliageType::FoliageType()
{
    // Scene-file defaults (they differ from FoliageLayer's code defaults:
    // finite bounds keep the JSON tidy, and props are usually sparser).
    spacing = 6.0f;
    minHeight = -1e6f;
    maxHeight = 1e6f;
    shadowDistance = 1e6f;
}

std::shared_ptr<const FoliageMeshes> FoliageType::meshes(Renderer& r)
{
    std::ostringstream key;
    key << mesh << '|' << library << '|' << int(lodMode) << '|' << lodThresholds.x << ',' << lodThresholds.y << ','
        << lodThresholds.z << '|' << autoLods << '|' << impostor << '|' << int(collision) << '|' << colliderSize.x << ','
        << colliderSize.y << ',' << colliderSize.z << '|' << colliderUserValue << '|' << states;
    if (key.str() == m_meshKey) return m_meshes;
    m_meshKey = key.str();
    const std::string owner = "FoliageType '" + gameObject().name() + "'";
    m_meshes = buildMeshes(r, mesh, library, owner);
    // Extra states: "Name = library name or glTF path(s)"; a spec with a '.' is a file.
    m_stateMeshes.clear();
    for (const std::string& entry : str::split(states, ';')) {
        const size_t eq = entry.find('=');
        const std::string name = str::trim(std::string_view(entry).substr(0, eq));
        const std::string spec = eq == std::string::npos ? std::string() : str::trim(std::string_view(entry).substr(eq + 1));
        std::shared_ptr<const FoliageMeshes> sm;
        if (!spec.empty()) {
            const bool file = spec.find('.') != std::string::npos;
            sm = buildMeshes(r, file ? spec : std::string(), file ? std::string() : spec, owner + " state '" + name + "'");
            if (!sm) log::warnOnce(owner + ": state '" + name + "' has no meshes (" + spec + ")");
        }
        m_stateMeshes.push_back({name, sm ? sm : std::make_shared<FoliageMeshes>()});
    }
    return m_meshes;
}

std::shared_ptr<const FoliageMeshes> FoliageType::buildMeshes(Renderer& r, const std::string& mesh, const std::string& library,
                                                              const std::string& owner)
{
    auto out = std::make_shared<FoliageMeshes>();
    std::vector<MeshData> hulls; // per variant, for ConvexHull
    if (!library.empty() && mesh.empty()) {
        auto lib = MeshLibrary::get(r, library);
        if (!lib) {
            log::warnOnce(owner + ": no MeshLibrary entry '" + library + "'");
            return nullptr;
        }
        *out = *lib;
    } else if (!mesh.empty()) {
        LodModel model;
        try {
            model = loadLodModel(assets(), mesh);
        } catch (const std::exception& e) {
            log::warnOnce(owner + ": " + e.what());
            return nullptr;
        }
        auto threshold = [&](size_t level) {
            InstanceLod l;
            const float v = level < 3 ? lodThresholds[int(level)] : 0.0f;
            if (lodMode == LodMode::ScreenSize) l.screenSize = std::max(v, 1e-4f);
            else l.maxDistance = v > 0.0f ? v : FLT_MAX;
            return l;
        };
        std::vector<MeshData> authoredColliders;
        size_t levelCount = 0; // of the first variant (log)
        for (LodVariant& v : model.variants) {
            // Missing materials (untextured exports): plain white.
            for (LodGeometry& g : v.levels)
                for (auto& m : g.materials)
                    if (!m) m = r.createStandardMaterial();
            hulls.push_back(v.levels.front().data);
            LodChainDesc c;
            c.name = v.name;
            if (v.levels.size() == 1 && autoLods > 0 && v.levels[0].submeshes.size() == 1) {
                for (int k = 1; k <= std::min(autoLods, 2); ++k)
                    c.simplify.push_back({.ratio = std::pow(0.35f, float(k)), .maxError = 0.08f * float(k)});
            }
            if (levelCount == 0) levelCount = v.levels.size() + c.simplify.size();
            c.levels = std::move(v.levels);
            c.maxLevels = impostor ? 3 : 4;
            c.impostor = impostor;
            for (size_t i = 0; i < 4; ++i) c.switches.push_back(threshold(i));
            std::vector<InstanceLod> chain = buildLodChain(r, std::move(c));
            out->variants.push_back(std::move(chain));
            if (!v.collider.vertices.empty()) authoredColliders.push_back(v.collider);
        }
        // Authored *_COL shapes: one per variant, or one shared.
        if (collision == Collision::Auto) {
            if (authoredColliders.size() == model.variants.size() || authoredColliders.size() == 1)
                for (const MeshData& c : authoredColliders) out->colliders.push_back(Collider::convexHull(c));
            else if (!authoredColliders.empty())
                log::warnOnce(owner + ": some variants have *_COL shapes and some don't: colliders ignored");
        }
        log::info("{}: {} variant(s) from {}, {} level(s){}, {} authored collider(s)", owner, model.variants.size(), mesh,
                  levelCount, impostor ? " + impostor" : "", authoredColliders.size());
    } else {
        return nullptr;
    }
    if (out->variants.empty()) return nullptr;

    const float radius = std::max(colliderSize.x, 0.01f), height = std::max(colliderSize.y, 0.01f);
    switch (collision) {
    case Collision::Auto:
        break;
    case Collision::None:
        out->colliders.clear();
        break;
    case Collision::Capsule:
        out->colliders = {Collider::capsule(std::max(height * 0.5f - radius, 0.01f), radius).offset({0.0f, height * 0.5f, 0.0f})};
        break;
    case Collision::Cylinder:
        out->colliders = {Collider::cylinder(height * 0.5f, radius).offset({0.0f, height * 0.5f, 0.0f})};
        break;
    case Collision::Sphere:
        out->colliders = {Collider::sphere(radius).offset({0.0f, radius, 0.0f})};
        break;
    case Collision::Box:
        out->colliders = {Collider::box(glm::max(colliderSize, glm::vec3(0.01f)) * 0.5f).offset({0.0f, height * 0.5f, 0.0f})};
        break;
    case Collision::ConvexHull:
        if (hulls.empty()) {
            log::warnOnce(owner + ": ConvexHull needs a glTF mesh (library meshes bring their own colliders)");
        } else {
            out->colliders.clear();
            for (const MeshData& h : hulls) out->colliders.push_back(Collider::convexHull(h));
        }
        break;
    }
    if (colliderUserValue != 0) out->colliderUserValue = uint32_t(colliderUserValue);
    return out;
}

// -----------------------------------------------------------------------------
// Foliage
// -----------------------------------------------------------------------------

Foliage::Foliage() : GameObject("Foliage")
{
    runInEditor = true;
}

void Foliage::start()
{
    rebuild();
}

int Foliage::layerIndex(const std::string& name) const
{
    for (size_t i = 0; i < m_layerNames.size(); ++i)
        if (m_layerNames[i] == name) return int(i);
    return -1;
}

std::string Foliage::state() const
{
    std::string s = fieldsOf("Foliage", this);
    std::vector<FoliageType*> types;
    collect(const_cast<Foliage&>(*this), types);
    for (FoliageType* t : types) s += t->gameObject().name() + fieldsOf("FoliageType", t);
    return s;
}

void Foliage::update(float dt)
{
    VE_PROFILE("Foliage rules");
    m_sinceBuild += dt;
    m_sinceCheck += dt;
    if (m_sinceBuild < 0.25f) return;
    const Terrain* terrain = Terrain::find(scene());
    const Terrain* builtOn = m_terrain.get(scene());
    FoliageSystem* system = this->system();
    if (terrain != builtOn || (terrain && terrain->version() != m_terrainVersion)) {
        // Brush edits only: re-place around them instead of everything.
        glm::vec4 area;
        if (!m_dirty && system && terrain == builtOn && terrain->changedOnlyIn(m_terrainVersion, area) && state() == m_state)
            refresh(area);
        else
            m_dirty = true;
    }
    // Serializing every field and scanning the scene is too costly for every
    // frame: edits are picked up within a quarter second (code can markDirty()).
    const bool check = m_sinceCheck >= 0.25f;
    if (check) m_sinceCheck = 0.0f;
    if (check && !m_dirty && state() != m_state) m_dirty = true;
    if (m_dirty) {
        rebuild();
        system = this->system();
    } else if (system && check) {
        // An exclusion appeared, moved or went (a building): re-place only around it.
        auto shapes = exclusionShapes(scene());
        if (shapes != m_exclusions) {
            const glm::vec4 area = changedArea(shapes, m_exclusions, 1.0f);
            m_exclusions = shapes;
            if (area.x <= area.z) system->setExclusions(std::move(shapes), area);
        }
    }

    // The focus is looked up by name (it may be spawned or renamed later); the
    // system remembers its id and follows it every frame.
    if (system && check) system->setColliderFocus(colliderFocus.empty() ? nullptr : scene().find(colliderFocus));
}

void Foliage::rebuild()
{
    m_dirty = false;
    m_sinceBuild = 0.0f;
    m_state = state();
    // Instance states (chopped trees, picked bushes) outlive the rebuild,
    // unless a save game handed us its own (loadState before the first build).
    if (FoliageSystem* old = system()) {
        if (!m_hasPendingOverrides) {
            m_pendingOverrides = old->overrides();
            m_hasPendingOverrides = !m_pendingOverrides.empty();
        }
        old->destroy();
    }
    m_system.reset();
    m_layerNames.clear();

    Terrain* terrain = Terrain::find(scene());
    m_terrain = terrain;
    if (!terrain) return;
    m_terrainVersion = terrain->version();
    auto td = terrain->data();

    std::vector<FoliageType*> types;
    collect(*this, types);
    if (types.empty()) return;

    m_densities.clear();
    const FoliageWorld world = makeWorld(td);
    FoliageSettings settings;
    settings.cellSize = std::max(cellSize, 4.0f);
    settings.instancesPerFrame = uint32_t(std::max(instancesPerFrame, 1));
    settings.bodiesPerFrame = uint32_t(std::max(bodiesPerFrame, 1));

    auto& sys = scene().spawn<FoliageSystem>(world, settings);
    sys.setName(name() + " (system)");
    sys.hideInHierarchy = true;
    sys.runInEditor = true;
    sys.setParent(this, true);
    m_system = &sys;
    m_exclusions = exclusionShapes(scene());
    for (const FoliageExclusionShape& e : m_exclusions) sys.addExclusion(e);

    const std::string self = "Foliage '" + name() + "'";
    for (FoliageType* t : types) {
        const std::string layerName = t->gameObject().name();
        auto meshes = t->meshes(renderer());
        if (!meshes) {
            if (t->mesh.empty() && t->library.empty()) log::warnOnce(self + ": layer '" + layerName + "' has no mesh or library");
            continue;
        }
        FoliageLayer l;
        l.name = layerName;
        l.variants = meshes->variants;
        l.colliders = meshes->colliders;
        l.colliderUserValue = meshes->colliderUserValue;
        l.colliderSurface = t->colliderSurface;
        for (const auto& [stateName, sm] : t->stateMeshes()) l.states.push_back({stateName, sm->variants, sm->colliders});
        l.promoteDistance = t->promoteDistance;
        if (!t->promoteType.empty()) {
            const std::string type = t->promoteType;
            l.promote = [type, self](Scene& s, const FoliageInstanceInfo&) -> GameObject* {
                const TypeInfo* info = TypeRegistry::get().find(type);
                if (!info || !info->spawn) {
                    log::warnOnce(self + ": promoteType '" + type + "' isn't a registered GameObject type");
                    return &s.spawn(type);
                }
                return info->spawn(s);
            };
        }
        static_cast<FoliageRules&>(l) = *t; // the shared placement / streaming fields
        l.spacing = std::max(l.spacing, 0.1f);
        l.maxScale = std::max(l.maxScale, l.minScale);
        if (l.seed == 0) l.seed = nameSeed(layerName);
        const LayerDensity density{parseBiomeWeights(t->biomes, *td, "FoliageType '" + layerName + "'"), t->biomeThreshold};
        l.density = makeDensity(td, density);
        for (const auto& [other, radius] : parseNameValues(t->avoid)) {
            const int index = layerIndex(other);
            if (index < 0) log::warnOnce(self + ": layer '" + layerName + "' avoids '" + other + "', which isn't an earlier layer");
            else l.avoid.push_back({uint32_t(index), radius});
        }
        try {
            sys.addLayer(std::move(l));
            m_layerNames.push_back(layerName);
            m_densities.push_back(density);
        } catch (const std::exception& e) {
            log::warnOnce(self + ": " + e.what());
        }
    }

    GameObject* focus = colliderFocus.empty() ? nullptr : scene().find(colliderFocus);
    sys.setColliderFocus(focus);
    if (preload && !m_built) {
        if (engine().loading()) {
            // Behind a loading screen: the workers fill the cells over the
            // next frames instead of blocking the main thread.
            const uint64_t id = sys.id();
            Scene* s = &scene();
            engine().addLoadTask("Placing vegetation", [s, id] {
                auto* f = dynamic_cast<FoliageSystem*>(s->findById(id));
                return f ? f->streamProgress() : 1.0f;
            });
        } else {
            glm::vec3 viewer(0.0f);
            if (Camera* cam = scene().mainCamera()) viewer = cam->transform().position();
            if (focus) viewer = focus->transform().position();
            sys.preload(viewer);
        }
    }
    m_built = true;
    if (m_hasPendingOverrides) {
        sys.setOverrides(m_pendingOverrides);
        m_hasPendingOverrides = false;
    }
}

void Foliage::saveState(Json& out) const
{
    Json list = Json::array();
    const FoliageSystem* sys = system();
    const auto overrides = sys ? sys->overrides() : m_pendingOverrides;
    for (const auto& [id, state] : overrides) list.push_back({id, state});
    out["overrides"] = std::move(list);
}

void Foliage::loadState(const Json& in)
{
    std::vector<std::pair<uint64_t, uint32_t>> overrides;
    for (const Json& e : in.value("overrides", Json::array()))
        if (e.is_array() && e.size() == 2) overrides.push_back({e[0].get<uint64_t>(), e[1].get<uint32_t>()});
    if (FoliageSystem* sys = system()) {
        sys->setOverrides(overrides);
    } else {
        m_pendingOverrides = std::move(overrides); // applied when the system is built
        m_hasPendingOverrides = true;
    }
}

FoliageWorld Foliage::makeWorld(const std::shared_ptr<const TerrainData>& td) const
{
    FoliageWorld world;
    const glm::vec2 lo = td->origin, hi = td->origin + glm::vec2(float(td->res - 1) * td->cellSize);
    world.boundsMin = lo + edgeMargin;
    world.boundsMax = glm::max(hi - edgeMargin, world.boundsMin + 1.0f);
    world.height = [td](glm::vec2 p) { return td->height(p.x, p.y); };
    world.normal = [td](glm::vec2 p) { return td->normal(p.x, p.y); };
    return world;
}

std::function<float(const FoliageSample&)> Foliage::makeDensity(const std::shared_ptr<const TerrainData>& td, const LayerDensity& d)
{
    if (d.weights.empty() && d.threshold <= 0.0f) return {};
    return [td, weights = d.weights, threshold = d.threshold](const FoliageSample& s) {
        const float v = biomeDensity(weights, *td, s.position);
        return threshold > 0.0f ? (v >= threshold ? 1.0f : 0.0f) : v;
    };
}

void Foliage::refresh(glm::vec4 area)
{
    const Terrain* terrain = m_terrain.get(scene());
    FoliageSystem* sys = system();
    if (!terrain || !sys) { // deleted since the check: rebuild instead
        m_dirty = true;
        return;
    }
    m_terrainVersion = terrain->version();
    auto td = terrain->data();
    std::vector<std::function<float(const FoliageSample&)>> densities;
    for (const LayerDensity& d : m_densities) densities.push_back(makeDensity(td, d));
    sys->refresh(makeWorld(td), densities, area);
}

// -----------------------------------------------------------------------------
// GrassLayer
// -----------------------------------------------------------------------------

GrassLayer::GrassLayer() : GameObject("Grass")
{
    runInEditor = true;
}

GrassLayer::~GrassLayer()
{
    removePass();
}

void GrassLayer::start()
{
    rebuild();
}

void GrassLayer::onDestroy()
{
    removePass();
}

void GrassLayer::removePass()
{
    if (m_pass) renderer().graph().removePass(m_passName);
    m_pass = nullptr;
}

std::string GrassLayer::state() const
{
    // What needs new textures / meshes / shaders (the rest is applied live).
    std::ostringstream s;
    s << mesh << '|' << lodDistances.x << ',' << lodDistances.y << ',' << lodDistances.z << '|' << texture << '|' << cards
      << '|' << bladesPerClump << '|' << clumpRadius << '|' << fragmentShader << '|' << biomes << '|' << biomeThreshold
      << '|' << minGroundHeight << '|' << maxGroundHeight << '|' << maxSlope << '|' << patchFrequency << '|'
      << patchContrast << '|' << patchOffset << '|' << colorFrequency << '|' << heightFrequency << '|' << heightVariation
      << '|' << seed << '|' << maskResolution << '|' << castShadows << '|' << activeInHierarchy();
    return s.str();
}

void GrassLayer::update(float dt)
{
    VE_PROFILE("Grass layers");
    m_sinceBuild += dt;
    m_sinceCheck += dt;
    if (m_sinceBuild >= 0.25f) {
        const Terrain* terrain = Terrain::find(scene());
        const Terrain* builtOn = m_terrain.get(scene());
        if (terrain != builtOn || (terrain && terrain->version() != m_terrainVersion)) {
            // Brush edits only: re-bake the grass textures in the background, keep the pass.
            glm::vec4 area;
            if (!m_dirty && m_pass && terrain == builtOn && terrain->changedOnlyIn(m_terrainVersion, area) && state() == m_state) {
                m_terrainVersion = terrain->version();
                auto td = terrain->data();
                m_exclusions = exclusions();
                m_pass->refresh([td](glm::vec2 p) { return td->height(p.x, p.y); }, makeMask(td, m_exclusions));
            } else {
                m_dirty = true;
            }
        }
        // Serializing the settings and scanning the scene is too costly for
        // every frame: edits are picked up within a quarter second.
        const bool check = m_sinceCheck >= 0.25f;
        if (check) m_sinceCheck = 0.0f;
        if (check && !m_dirty && state() != m_state) m_dirty = true;
        if (m_dirty) {
            rebuild();
        } else if (m_pass && builtOn && check) {
            // Exclusions changed (a building placed / removed): re-bake in the background, keep the pass.
            auto current = exclusions();
            if (current != m_exclusions) {
                m_exclusions = std::move(current);
                auto td = builtOn->data();
                m_pass->refresh([td](glm::vec2 p) { return td->height(p.x, p.y); }, makeMask(td, m_exclusions));
                m_sinceBuild = 0.0f;
            }
        }
    }
    if (!m_pass) return;
    // Live parameters.
    GrassDesc& d = m_pass->desc();
    d.colorA = colorA;
    d.colorB = colorB;
    d.alphaCutoff = alphaCutoff;
    d.minHeight = minHeight;
    d.maxHeight = std::max(maxHeight, minHeight);
    d.bladeWidth = bladeWidth;
    d.maxTilt = maxTilt;
    d.sway = sway;
    d.spacing = std::max(spacing, 0.05f);
    d.radius = radius;
    d.fadeStart = fadeStart;
    d.farDensity = farDensity;
    d.shadowDistance = shadowDistance;
    d.lodBlend = lodBlend;
}

std::vector<GrassLayer::Exclusion> GrassLayer::exclusions() const
{
    // Like exclusionShapes() for grass, plus each one's falloff (fades the grass).
    std::vector<Exclusion> out;
    scene().forEach<FoliageExclusion>([&](FoliageExclusion& e) {
        if (!e.isActive() || !e.grass || PlacementGhost::is(e.gameObject())) return;
        out.push_back({e.shape(), std::max(e.falloff, 0.01f)});
    });
    return out;
}

std::function<glm::vec4(glm::vec2)> GrassLayer::makeMask(const std::shared_ptr<const TerrainData>& td,
                                                         std::vector<Exclusion> exclusions) const
{
    const std::string self = "GrassLayer '" + name() + "'";
    auto weights = parseBiomeWeights(biomes, *td, self);
    const auto s = uint32_t(seed);
    // Copies of the settings (not `this`): windowed grass bakes on a worker
    // thread, which must not read a layer being edited or destroyed.
    const float minGroundHeight = this->minGroundHeight, maxGroundHeight = this->maxGroundHeight;
    const float biomeThreshold = this->biomeThreshold, maxSlope = this->maxSlope;
    const float patchFrequency = this->patchFrequency, patchContrast = this->patchContrast, patchOffset = this->patchOffset;
    const float colorFrequency = this->colorFrequency, heightFrequency = this->heightFrequency;
    const float heightVariation = this->heightVariation;
    return [=](glm::vec2 p) {
        const float h = td->height(p.x, p.y);
        if (h < minGroundHeight || h > maxGroundHeight) return glm::vec4(0.0f);
        float density = biomeDensity(weights, *td, p);
        if (biomeThreshold > 0.0f) density = density >= biomeThreshold ? 1.0f : 0.0f;
        if (maxSlope < 1.0f) density *= 1.0f - glm::smoothstep(maxSlope - 0.05f, maxSlope + 0.05f, 1.0f - td->normal(p.x, p.y).y);
        if (patchFrequency > 0.0f)
            density *= std::clamp(noise::fbm(p * patchFrequency, 2, 61 + s) * patchContrast - patchOffset, 0.0f, 1.0f);
        for (const Exclusion& e : exclusions) density *= glm::smoothstep(0.0f, e.falloff, e.shape.distance(p));
        if (density <= 0.0f) return glm::vec4(0.0f);
        const float colorMix = colorFrequency > 0.0f ? noise::fbm(p * colorFrequency, 3, 31 + s) : 0.5f;
        const float heightScale =
            heightFrequency > 0.0f ? glm::mix(1.0f - heightVariation, 1.0f, noise::fbm(p * heightFrequency, 2, 13 + s)) : 1.0f;
        return glm::vec4(density, colorMix, heightScale, 0.0f);
    };
}

void GrassLayer::rebuild()
{
    m_dirty = false;
    m_sinceBuild = 0.0f;
    m_state = state();
    removePass();
    Terrain* terrain = Terrain::find(scene());
    m_terrain = terrain;
    if (!terrain || !activeInHierarchy()) return;
    m_terrainVersion = terrain->version();
    auto td = terrain->data();
    const std::string self = "GrassLayer '" + name() + "'";

    GrassDesc d;
    static uint32_t counter = 0;
    m_passName = name() + " #" + std::to_string(++counter);
    d.name = m_passName;
    d.boundsMin = td->origin;
    d.boundsMax = td->origin + glm::vec2(float(td->res - 1) * td->cellSize);
    d.resolution = uint32_t(td->res);
    const float size = d.boundsMax.x - d.boundsMin.x;
    d.maskResolution = uint32_t(std::clamp(size * maskResolution, 16.0f, 4096.0f));
    d.height = [td](glm::vec2 p) { return td->height(p.x, p.y); };
    // Big worlds: bake only a window around the camera (it follows on a worker thread).
    d.window = 4.0f * radius + 64.0f;

    m_exclusions = exclusions();
    d.mask = makeMask(td, m_exclusions);
    d.bladesPerClump = uint32_t(std::max(bladesPerClump, 1));
    d.clumpRadius = clumpRadius;
    d.castShadows = castShadows;
    d.fragmentShader = fragmentShader;

    std::shared_ptr<Texture> overrideTexture;
    if (!texture.empty()) {
        try {
            overrideTexture = assets().loadTexture(texture, true);
        } catch (const std::exception& e) {
            log::warn("{}: {}", self, e.what());
        }
    }
    if (!mesh.empty()) {
        try {
            const LodModel model = loadLodModel(assets(), mesh);
            const LodVariant& v = model.variants.front();
            if (model.variants.size() > 1) log::warnOnce(self + ": " + mesh + " has several variants; grass uses the first");
            log::info("{}: {} LOD(s) from {}", self, v.levels.size(), mesh);
            // Unit height: every level scaled like LOD0 (root at the origin).
            float top = 0.0f;
            for (const Vertex& vx : v.levels.front().data.vertices) top = std::max(top, vx.position.y);
            const float scale = top > 1e-4f ? 1.0f / top : 1.0f;
            for (size_t i = 0; i < v.levels.size(); ++i) {
                const LodGeometry& g = v.levels[i];
                GrassLod lod;
                lod.mesh = g.data;
                // Base colour factors go into the vertex colours (one texture per LOD).
                for (const Submesh& sm : g.submeshes) {
                    const auto& m = g.materials[sm.materialSlot];
                    const glm::vec4 factor = m ? m->vector(0) : glm::vec4(1.0f);
                    for (uint32_t k = 0; k < sm.indexCount; ++k) {
                        Vertex& vx = lod.mesh.vertices[lod.mesh.indices[sm.firstIndex + k]];
                        vx.color = g.data.vertices[g.data.indices[sm.firstIndex + k]].color * factor;
                    }
                }
                for (Vertex& vx : lod.mesh.vertices) vx.position *= scale;
                lod.texture = overrideTexture;
                for (const auto& m : g.materials)
                    if (!lod.texture && m && m->texture(0)) lod.texture = m->texture(0);
                lod.maxDistance = i < 3 ? lodDistances[int(i)] : FLT_MAX;
                d.lods.push_back(std::move(lod));
            }
        } catch (const std::exception& e) {
            log::warnOnce(self + ": " + e.what());
        }
    } else if (cards > 0) {
        d.cardMesh = GrassDesc::cardMeshCrossed(uint32_t(cards), 1.1f);
        d.cardTexture = overrideTexture;
        d.fragmentShader.clear();
    }
    m_pass = &renderer().graph().addPass<GrassPass>(d);
    update(0.0f); // live parameters
}

// -----------------------------------------------------------------------------
// Reflection
// -----------------------------------------------------------------------------

void registerVegetationTypes(TypeRegistry& r)
{
    // Model marker nodes (MeshComponents.hpp): EXCL (a box) or EXCL_circle.
    registerModelMarker("EXCL", [](ModelMarker& m) {
        const glm::vec3 scale = m.object.transform().localScale();
        auto& e = m.object.addComponent<FoliageExclusion>(); // its box follows the object's scale itself
        e.box = std::find(m.args.begin(), m.args.end(), "circle") == m.args.end();
        e.size = glm::vec2(m.bounds.extents().x, m.bounds.extents().z) * 2.0f;
        e.offset = glm::vec2(m.bounds.center().x, m.bounds.center().z);
        e.radius = std::max(m.bounds.extents().x * std::abs(scale.x), m.bounds.extents().z * std::abs(scale.z));
        e.radius = m.extra("radius", e.radius);
        e.falloff = m.extra("falloff", e.falloff);
        e.grass = m.extra("grass", e.grass);
        e.foliage = m.extra("foliage", e.foliage);
    });

    r.add<FoliageExclusion>("FoliageExclusion", [](TypeBuilder<FoliageExclusion>& t) {
        t.field("box", &FoliageExclusion::box).tooltip("a box (size, turning with the object) instead of a circle (radius)");
        t.field("size", &FoliageExclusion::size).tooltip("box: full extents along the object's local x and z");
        t.field("offset", &FoliageExclusion::offset).tooltip("centre in the object's local x, z");
        t.category("World");
        t.field("radius", &FoliageExclusion::radius).range(0.0f, 1000.0f).speed(0.05f);
        t.field("falloff", &FoliageExclusion::falloff).range(0.0f, 100.0f).speed(0.05f).tooltip("grass thins out over this distance");
        t.field("foliage", &FoliageExclusion::foliage);
        t.field("grass", &FoliageExclusion::grass);
    });
    r.add<FoliageType>("FoliageType", [](TypeBuilder<FoliageType>& t) {
        t.category("World");
        t.field("mesh", &FoliageType::mesh).assets(".gltf .glb").tooltip("glTF with *_LOD0, *_LOD1... (and *_COL) nodes, or 'a.glb; b.glb'");
        t.field("library", &FoliageType::library).tooltip("MeshLibrary name (meshes built in code); used when mesh is empty");
        t.enumField("lodMode", &FoliageType::lodMode, {"Screen size", "Distance"});
        t.field("lodThresholds", &FoliageType::lodThresholds).tooltip("LOD0->1, 1->2, 2->3: screen fraction or metres");
        t.field("autoLods", &FoliageType::autoLods).range(0.0f, 2.0f).tooltip("one-level meshes: add simplified levels");
        t.field("impostor", &FoliageType::impostor);
        t.enumField("collision", &FoliageType::collision,
                    {"Auto", "None", "Capsule", "Cylinder", "Sphere", "Box", "Convex hull"});
        t.field("colliderSize", &FoliageType::colliderSize).tooltip("capsule/cylinder: radius, height; sphere: radius; box: size");
        t.field("colliderUserValue", &FoliageType::colliderUserValue);
        t.field("colliderSurface", &FoliageType::colliderSurface).tooltip("physics surface: Wood, Stone... (footsteps, impacts)");
        t.field("biomes", &FoliageType::biomes).tooltip("density per terrain biome: 'Savannah:1, Swamp:-1' (empty = everywhere)");
        t.field("biomeThreshold", &FoliageType::biomeThreshold).range(0.0f, 1.0f).speed(0.01f)
            .tooltip("0 = biome density is a probability; >0 = all or nothing at this density");
        t.field("spacing", &FoliageType::spacing).range(0.1f, 1000.0f).speed(0.05f);
        t.field("jitter", &FoliageType::jitter).range(0.0f, 1.0f).speed(0.01f);
        t.field("probability", &FoliageType::probability).range(0.0f, 1.0f).speed(0.01f);
        t.field("minHeight", &FoliageType::minHeight);
        t.field("maxHeight", &FoliageType::maxHeight);
        t.field("maxSlope", &FoliageType::maxSlope).range(0.0f, 1.0f).speed(0.01f);
        t.field("clusterFrequency", &FoliageType::clusterFrequency).range(0.0f, 1.0f).speed(0.0005f);
        t.field("clusterCoverage", &FoliageType::clusterCoverage).range(0.0f, 1.0f).speed(0.01f);
        t.field("avoid", &FoliageType::avoid).tooltip("'Acacias:2.5, Rocks:1': keep this far from earlier layers");
        t.field("minScale", &FoliageType::minScale).range(0.01f, 100.0f).speed(0.01f);
        t.field("maxScale", &FoliageType::maxScale).range(0.01f, 100.0f).speed(0.01f);
        t.field("tint", &FoliageType::tint).range(0.0f, 1.0f).speed(0.01f);
        t.field("alignToNormal", &FoliageType::alignToNormal).range(0.0f, 1.0f).speed(0.01f);
        t.field("sink", &FoliageType::sink).speed(0.01f);
        t.field("viewDistance", &FoliageType::viewDistance).range(1.0f, 100000.0f);
        t.field("shadowDistance", &FoliageType::shadowDistance).range(0.0f, 1e6f);
        t.field("castShadows", &FoliageType::castShadows);
        t.field("colliderDistance", &FoliageType::colliderDistance).range(0.0f, 10000.0f);
        t.field("maxInstances", &FoliageType::maxInstances).range(0.0f, 1e7f);
        t.field("seed", &FoliageType::seed).tooltip("0 = from the object name");
        t.field("states", &FoliageType::states).tooltip("extra looks: 'Stump = library name; Picked = model.glb'");
        t.field("promoteDistance", &FoliageType::promoteDistance).range(0.0f, 200.0f)
            .tooltip("> 0: instances this close to the player become GameObjects (with a FoliageLink)");
        t.field("promoteType", &FoliageType::promoteType).tooltip("registered GameObject type to promote to (empty = plain)");
    });
    r.add<Foliage>("Foliage", [](TypeBuilder<Foliage>& t) {
        t.category("World");
        t.field("edgeMargin", &Foliage::edgeMargin).range(0.0f, 10000.0f);
        t.field("cellSize", &Foliage::cellSize).range(4.0f, 1024.0f);
        t.field("instancesPerFrame", &Foliage::instancesPerFrame).range(1.0f, 1e6f);
        t.field("bodiesPerFrame", &Foliage::bodiesPerFrame).range(1.0f, 1e5f);
        t.field("colliderFocus", &Foliage::colliderFocus).tooltip("object name; colliders stream around it (else the camera)");
        t.field("preload", &Foliage::preload);
    });
    r.add<GrassBender>("GrassBender", [](TypeBuilder<GrassBender>& t) {
        t.category("World");
        t.field("radius", &GrassBender::radius).range(0.0f, 20.0f).speed(0.01f).tooltip("grass within this distance of the object leans away");
        t.field("strength", &GrassBender::strength).range(0.0f, 1.0f).speed(0.01f);
    });
    r.add<GrassLayer>("GrassLayer", [](TypeBuilder<GrassLayer>& t) {
        t.category("World");
        t.field("mesh", &GrassLayer::mesh).assets(".gltf .glb").tooltip("empty = procedural blades; glTF with *_LOD0, *_LOD1... nodes");
        t.field("lodDistances", &GrassLayer::lodDistances).tooltip("switch distances LOD0->1, 1->2, 2->3 (m)");
        t.field("lodBlend", &GrassLayer::lodBlend).range(0.0f, 50.0f).speed(0.1f);
        t.field("texture", &GrassLayer::texture).assets(".png .jpg .jpeg .tga").tooltip("overrides the mesh texture; card texture");
        t.field("cards", &GrassLayer::cards).range(0.0f, 8.0f).tooltip("no mesh: crossed quads textured with `texture`");
        t.field("colorA", &GrassLayer::colorA).color();
        t.field("colorB", &GrassLayer::colorB).color();
        t.field("alphaCutoff", &GrassLayer::alphaCutoff).range(0.0f, 1.0f).speed(0.01f);
        t.field("minHeight", &GrassLayer::minHeight).range(0.01f, 20.0f).speed(0.01f);
        t.field("maxHeight", &GrassLayer::maxHeight).range(0.01f, 20.0f).speed(0.01f);
        t.field("bladeWidth", &GrassLayer::bladeWidth).range(0.001f, 1.0f).speed(0.001f);
        t.field("bladesPerClump", &GrassLayer::bladesPerClump).range(1.0f, 32.0f);
        t.field("clumpRadius", &GrassLayer::clumpRadius).range(0.0f, 5.0f).speed(0.01f);
        t.field("maxTilt", &GrassLayer::maxTilt).range(0.0f, 1.5f).speed(0.01f);
        t.field("sway", &GrassLayer::sway).range(0.0f, 5.0f).speed(0.01f);
        t.field("fragmentShader", &GrassLayer::fragmentShader).tooltip("blades: custom fragment shader (empty = engine)");
        t.field("biomes", &GrassLayer::biomes).tooltip("density per terrain biome: 'Savannah:1, Camp:1' (empty = everywhere)");
        t.field("biomeThreshold", &GrassLayer::biomeThreshold).range(0.0f, 1.0f).speed(0.01f);
        t.field("minGroundHeight", &GrassLayer::minGroundHeight);
        t.field("maxGroundHeight", &GrassLayer::maxGroundHeight);
        t.field("maxSlope", &GrassLayer::maxSlope).range(0.0f, 1.0f).speed(0.01f);
        t.field("patchFrequency", &GrassLayer::patchFrequency).range(0.0f, 1.0f).speed(0.001f);
        t.field("patchContrast", &GrassLayer::patchContrast).range(0.0f, 10.0f).speed(0.01f);
        t.field("patchOffset", &GrassLayer::patchOffset).range(-2.0f, 2.0f).speed(0.01f);
        t.field("colorFrequency", &GrassLayer::colorFrequency).range(0.0f, 1.0f).speed(0.001f);
        t.field("heightFrequency", &GrassLayer::heightFrequency).range(0.0f, 1.0f).speed(0.001f);
        t.field("heightVariation", &GrassLayer::heightVariation).range(0.0f, 1.0f).speed(0.01f);
        t.field("seed", &GrassLayer::seed);
        t.field("spacing", &GrassLayer::spacing).range(0.05f, 10.0f).speed(0.01f);
        t.field("radius", &GrassLayer::radius).range(1.0f, 1000.0f);
        t.field("fadeStart", &GrassLayer::fadeStart).range(0.0f, 1000.0f);
        t.field("farDensity", &GrassLayer::farDensity).range(0.0f, 1.0f).speed(0.01f);
        t.field("maskResolution", &GrassLayer::maskResolution).range(0.05f, 8.0f).speed(0.01f).tooltip("mask texels per metre");
        t.field("castShadows", &GrassLayer::castShadows);
        t.field("shadowDistance", &GrassLayer::shadowDistance).range(0.0f, 500.0f);
    });
}

} // namespace ve
