#include "AutoTest.hpp"

#include "Building.hpp"
#include "Harvest.hpp"
#include "Hud.hpp"
#include "Inventory.hpp"
#include "MainMenu.hpp"
#include "Pickups.hpp"
#include "Player.hpp"
#include "Props.hpp"
#include "SavannaLayout.hpp"
#include "Villager.hpp"

#include <ve/anim/Animation.hpp>
#include <ve/audio/Ambience.hpp>
#include <ve/audio/Audio.hpp>
#include <ve/audio/Music.hpp>
#include <ve/core/Engine.hpp>
#include <ve/core/Env.hpp>
#include <ve/core/Input.hpp>
#include <ve/core/MathUtil.hpp>
#include <ve/core/Time.hpp>
#include <ve/debug/Debug.hpp>
#include <ve/gameplay/Interaction.hpp>
#include <ve/gameplay/Placement.hpp>
#include <ve/gameplay/ProxySet.hpp>
#include <ve/nav/Navigation.hpp>
#include <ve/physics/Physics.hpp>
#include <ve/render/Instancing.hpp>
#include <ve/render/Mesh.hpp>
#include <ve/render/Renderer.hpp>
#include <ve/scene/Components.hpp>
#include <ve/scene/Decal.hpp>
#include <ve/assets/AssetManager.hpp>
#include <ve/assets/DataAsset.hpp>
#include <ve/assets/Model.hpp>
#include <ve/render/Texture.hpp>
#include <ve/scene/MeshComponents.hpp>
#include <ve/scene/SaveGame.hpp>
#include <ve/scene/Scene.hpp>
#include <ve/world/Foliage.hpp>
#include <ve/world/Terrain.hpp>
#include <ve/world/Vegetation.hpp>
#include <vfx/DayNight.hpp>
#include <vfx/Weather.hpp>

#include <cmath>
#include <cstdio>
#include <functional>
#include <stdexcept>

namespace savanna {

namespace {
void forSubtree(ve::Transform& t, const std::function<void(ve::Transform&)>& fn)
{
    fn(t);
    for (ve::Transform* c : t.children()) forSubtree(*c, fn);
}
} // namespace

namespace {

// Counts physics events on its object (autotest).
class EventProbe : public ve::Component {
public:
    int enters = 0, stays = 0, exits = 0, triggerEnters = 0, triggerExits = 0;
    float maxImpact = 0.0f;
    std::string lastTrigger;
    void onCollisionEnter(const ve::Collision& c) override
    {
        ++enters;
        maxImpact = std::max(maxImpact, c.impactSpeed);
    }
    void onCollisionStay(const ve::Collision&) override { ++stays; }
    void onCollisionExit(const ve::Collision&) override { ++exits; }
    void onTriggerEnter(ve::GameObject& other) override
    {
        ++triggerEnters;
        lastTrigger += (lastTrigger.empty() ? "" : ", ") + other.name();
    }
    void onTriggerExit(ve::GameObject&) override { ++triggerExits; }
};

// A PointLight subclass added under its own type: Scene::forEach<PointLight>
// (which the renderer uses) must still find it.
class ProbeLight : public ve::PointLight {};

// Saveable state on a promoted pickup: the ProxySet must capture it when the
// pickup goes back to being data, and give it back (re-adding the component)
// when it's promoted again.
class ProbeState : public ve::Component, public ve::Saveable {
public:
    int value = 0;
    void saveState(ve::Json& out) const override { out["value"] = value; }
    void loadState(const ve::Json& in) override { value = in.value("value", 0); }
};
VE_REGISTER_TYPE(ProbeState, "AutoTestProbeState")
{
    t.category("Tests");
    t.field("value", &ProbeState::value);
}

// Spawns another object from onDestroy() (Scene::flushDestroyed must cope).
class SpawnOnDestroy : public ve::GameObject {
public:
    SpawnOnDestroy() : GameObject("SpawnOnDestroy") {}
    void onDestroy() override { scene().spawn("SpawnedFromOnDestroy"); }
};

} // namespace

namespace {
std::string stateLabel(const ve::FoliageSystem& f, ve::FoliageInstanceId id)
{
    const std::string& n = f.stateName(id.layer, f.state(id));
    return n.empty() ? std::string("(default)") : n;
}
} // namespace

void AutoTest::shot(const char* name)
{
    // SAVANNA_SHOTS=<folder>: screenshots of the interactive foliage steps (headless runs).
    if (const auto dir = ve::env::get("SAVANNA_SHOTS"); dir && !dir->empty() && !m_shotTaken.count(name)) {
        m_shotTaken.insert(name);
        renderer().requestCapture(std::string(*dir) + "/" + name + ".png");
    }
}

const ve::Terrain& AutoTest::terrain() const
{
    const ve::Terrain* t = m_terrainRef.get(scene());
    if (!t) throw std::runtime_error("AutoTest: the terrain is gone");
    return *t;
}

glm::vec3 AutoTest::groundAt(glm::vec2 p) const
{
    return {p.x, terrain().height(p.x, p.y) + 0.1f, p.y};
}

int AutoTest::foliageAt(const glm::vec3& p, float radius) const
{
    // What the placement rules put there, in every layer (deterministic; loaded or not).
    int n = 0;
    if (auto* f = scene().findFirst<ve::Foliage>(); f && f->system())
        for (uint32_t l = 0; l < f->system()->layerCount(); ++l) n += int(f->system()->query(l, {p.x, p.z}, radius).size());
    return n;
}

void AutoTest::buildStep(int piece, const glm::vec3& aim, float yawDegrees)
{
    auto* b = scene().findFirst<Builder>();
    b->setActive(true);
    b->select(piece);
    b->setYawDegrees(yawDegrees);
    b->setAimOverride(aim);
    m_t = 0.0f;
    m_stepFrame = m_frames;
    ++m_step;
}

void AutoTest::update(float dt)
{
    m_t += dt;
    ++m_frames;
    auto log = [](const std::string& s) { std::fprintf(stderr, "[autotest] %s\n", s.c_str()); };
    // Music mood and ambient layer levels (target levels: the fades take a few seconds).
    auto logAudio = [&](const std::string& where) {
        std::string layers;
        scene().forEach<ve::AmbientLayer>([&](ve::AmbientLayer& l) {
            char b[96];
            std::snprintf(b, sizeof(b), "%s%s %.2f", layers.empty() ? "" : ", ", l.gameObject().name().c_str(), l.targetLevel());
            layers += b;
        });
        log("audio @ " + where + ": music '" + audio().music().currentMood() + "' (" + audio().music().currentTrack() +
            "), Ambient bus x" + std::to_string(audio().bus("Ambient").zoneVolume) + ", Music bus x" +
            std::to_string(audio().bus("Music").zoneVolume) + "; layers: " + layers);
    };
    if (m_step == 0 && m_t > 3.0f) {
        auto stones = scene().findAllWithTag("collectible");
        log("sunstones in scene: " + std::to_string(stones.size()));
        if (auto* stone = stones.empty() ? nullptr : dynamic_cast<Sunstone*>(stones.front())) stone->collect();
        ++m_step;
        // Scene registry: subclass components, and spawning from onDestroy().
        {
            ve::GameObject& probe = scene().spawn("ProbeLight");
            probe.addComponent<ProbeLight>().intensity = 0.0f;
            int seen = 0;
            scene().forEach<ve::PointLight>([&](ve::PointLight& l) { seen += &l.gameObject() == &probe; });
            log("PointLight subclass seen by forEach<PointLight>: " + std::string(seen == 1 ? "yes" : "NO"));
            probe.destroy();
            m_destroyProbe = &scene().spawn<SpawnOnDestroy>();
        }
        // Physics: ray straight down onto the terrain, player standing, crates resting.
        const glm::vec3 above(layout::kCamp.x + 3.0f, 200.0f, layout::kCamp.y + 3.0f);
        if (auto hit = physics().raycast(above, {0, -1, 0}, 400.0f)) {
            log("raycast down hit '" + (hit->gameObject ? hit->gameObject->name() : std::string("?")) +
                "' at y=" + std::to_string(hit->point.y) + " (terrain says " +
                std::to_string(terrain().height(above.x, above.z)) + ")");
        } else {
            log("raycast down MISSED");
        }
        if (auto* p = localPlayer(scene())) {
            log(std::string("player grounded: ") + (p->grounded() ? "yes" : "no") + ", feet y=" +
                std::to_string(p->transform().position().y));
            const auto look = physics().raycast(p->eyePosition(), p->lookDirection(), 25.0f);
            log("eye raycast: " + (look ? (look->gameObject ? look->gameObject->name() : std::string("<no object>")) +
                                               " at " + std::to_string(look->distance) + " m"
                                        : std::string("nothing")));
            p->throwRock();
        }
        // Physics queries, and a trigger box the player is teleported into.
        {
            const glm::vec2 tp = layout::kCamp + glm::vec2(-4.0f, 4.0f); // clear ground between the fire and the reed screens
            const glm::vec3 center(tp.x, terrain().height(tp.x, tp.y) + 1.0f, tp.y);
            auto& trigger = scene().spawn("TestTrigger");
            trigger.transform().setPosition(center);
            auto& rb = trigger.addComponent<ve::Rigidbody>(ve::Collider::box({1.5f, 1.5f, 1.5f}), ve::BodyType::Static);
            rb.isTrigger = true;
            trigger.addComponent<EventProbe>();
            if (auto* p = localPlayer(scene())) {
                p->addComponent<EventProbe>();
                m_triggerFeet = center - glm::vec3(0.0f, 1.0f, 0.0f);
            }
            const auto down = physics().sphereCast(above, 0.5f, {0, -1, 0}, 400.0f);
            log("sphereCast down: " + (down ? (down->gameObject ? down->gameObject->name() : std::string("?")) + " at y=" +
                                                  std::to_string(down->point.y) + ", normal.y " + std::to_string(down->normal.y)
                                           : std::string("MISSED")));
            auto crateList = scene().findAllWithTag("crate");
            if (!crateList.empty()) {
                const glm::vec3 c = crateList.front()->transform().position();
                int crateHits = 0;
                for (const auto& h : physics().overlapSphere(c, 2.0f))
                    if (h.gameObject && h.gameObject->tag == "crate") ++crateHits;
                const auto box = physics().boxCast(c + glm::vec3(0.0f, 0.0f, 6.0f), glm::vec3(0.2f), glm::quat(1, 0, 0, 0),
                                                   {0, 0, -1}, 10.0f);
                log("overlapSphere(2 m) at a crate: " + std::to_string(crateHits) + " crates; boxCast towards it hit " +
                    (box ? (box->gameObject ? box->gameObject->name() : std::string("?")) + " at " +
                               std::to_string(box->distance) + " m"
                         : std::string("nothing")));
            }
            m_triggerCenter = center;
        }
        auto crates = scene().findAllWithTag("crate");
        float minY = 1e9f, maxY = -1e9f;
        for (auto* c : crates) {
            minY = std::min(minY, c->transform().position().y);
            maxY = std::max(maxY, c->transform().position().y);
        }
        log("crates: " + std::to_string(crates.size()) + ", y range " + std::to_string(minY) + " .. " + std::to_string(maxY));
        logAudio("start (camp)");
        // Decals: the scene's projectors (puddles, a wall crack), a short-lived
        // spawned one (expires before step 1 ends) and a bullet hole.
        {
            int projectors = 0, textured = 0;
            scene().forEach<ve::DecalProjector>([&](ve::DecalProjector& d) {
                ++projectors;
                textured += d.albedoTexture || d.maskTexture ? 1 : 0;
            });
            ve::Decal brief;
            brief.color = {0.1f, 0.1f, 0.1f, 1.0f};
            m_briefDecal = renderer().decals().spawn(brief, ve::DecalSystem::onSurface(above - glm::vec3(0, 190, 0), {0, 1, 0}, {1, 1}), 0.3f, 0.2f);
            log("decal projectors: " + std::to_string(projectors) + " (" + std::to_string(textured) + " with textures); spawned: " +
                std::to_string(renderer().decals().count()));
        }
        // Shared assets: the hut's glTF image file is the same GPU texture as a
        // direct load of it, and its "Thatch" material comes from
        // materials/thatch.material (one Material for every model using it).
        {
            const auto hut = assets().loadModel("models/rondavel.gltf");
            const ve::Material* wall = nullptr;
            const ve::Material* thatch = nullptr;
            for (const auto& slots : hut->meshMaterials)
                for (const auto& m : slots) {
                    if (m && m->name == "MudWall") wall = m.get();
                    if (m && m->name == "thatch") thatch = m.get();
                }
            const bool texShared = wall && wall->texture(0) == assets().loadTexture("textures/mud_wall.png");
            const bool matShared = thatch && thatch == assets().loadMaterial("materials/thatch.material").get();
            log(std::string("shared assets: glTF texture = direct load: ") + (texShared ? "yes" : "NO") +
                ", thatch from material file: " + (matShared ? "yes" : "NO"));
            // Texture settings come from the files (name / .texture), and where
            // the loaded textures came from (cooked cache or source images).
            const bool namesOk = assets().textureSettings("textures/ground/soil_normal.png").type == ve::TextureType::Normal &&
                                 assets().textureSettings("textures/decals/puddle_mask.png").type == ve::TextureType::Data &&
                                 assets().textureSettings("textures/mud_wall.png").type == ve::TextureType::Color;
            const ve::TextureSettings lut = assets().textureSettings("textures/lut_neutral_32.png");
            const auto& ts = assets().cookStats();
            log(std::string("texture settings by name: ") + (namesOk ? "yes" : "NO") + ", by .texture file: " +
                (lut.type == ve::TextureType::Data && !lut.mipmaps && !lut.compress ? "yes" : "NO") + "; textures loaded: " +
                std::to_string(ts.texturesCooked) + " cooked, " + std::to_string(ts.texturesSource) + " from source images; sounds: " +
                std::to_string(ts.soundsCooked) + " cooked, " + std::to_string(ts.soundsSource) + " source files; " +
                std::to_string(ts.cooking) + " cooking");
            // A live edit (like the editor's, unsaved): checked and reverted in step 1.
            ve::Json edited = *ve::DataAssets::get("materials/thatch.material");
            edited["roughness"] = 0.25f;
            ve::DataAssets::set("materials/thatch.material", edited);
        }
    } else if (m_step == 1 && !m_inTrigger && m_t > 3.3f) {
        // The thrown rock is away: stand in the trigger box for a moment.
        if (auto* p = localPlayer(scene())) p->teleport(m_triggerFeet, 0.0f);
        ve::QueryFilter f;
        f.triggers = true;
        f.statics = f.dynamics = false;
        const auto inside = physics().overlapSphere(m_triggerCenter, 0.5f, f);
        const auto ray = physics().raycast(m_triggerCenter + glm::vec3(0.0f, 5.0f, 0.0f), {0, -1, 0}, 10.0f);
        log("trigger queries: overlap(triggers only) found " + std::to_string(inside.size()) +
            (inside.empty() || !inside[0].gameObject ? std::string() : " (" + inside[0].gameObject->name() + ")") +
            ", default raycast through it hit " + (ray && ray->gameObject ? ray->gameObject->name() : std::string("nothing")));
        m_inTrigger = true;
    } else if (m_step == 1 && m_t > 4.0f) {
        if (ve::GameObject* probe = m_destroyProbe.get(scene())) probe->destroy(); // checked at step 2
        m_destroyProbe.reset();
        if (auto* probe = scene().find("TestTrigger") ? scene().find("TestTrigger")->getComponent<EventProbe>() : nullptr)
            log("trigger after standing in it: " + std::to_string(probe->triggerEnters) + " enter(s): " + probe->lastTrigger);
        {
            const float r = assets().loadMaterial("materials/thatch.material")->vector(1).x;
            log(std::string("material file live edit applied: ") + (std::abs(r - 0.25f) < 1e-4f ? "yes" : "NO"));
            ve::DataAssets::revert("materials/thatch.material");
        }
        {
            // The brief decal is gone (0.3 s + 0.2 s fade); a shot leaves a bullet hole.
            const uint32_t before = renderer().decals().count();
            bool shot = false;
            if (auto* p = localPlayer(scene())) {
                p->setPose(glm::vec3(layout::kCamp.x + 3.0f, terrain().height(layout::kCamp.x + 3.0f, layout::kCamp.y + 2.0f), layout::kCamp.y + 2.0f), 0.0f, -60.0f);
                shot = p->shoot();
            }
            log("decals: brief one expired: " + std::string(before == 0 ? "yes" : "NO") + ", shot hit: " + (shot ? "yes" : "no") +
                ", spawned now: " + std::to_string(renderer().decals().count()) + ", in view last frame: " +
                std::to_string(renderer().stats().decals));
        }
        log("after pickup: " + std::to_string(scene().findAllWithTag("collectible").size()) + " left, collected " +
            std::to_string(gameState().collected) + "/" + std::to_string(gameState().total));
        if (auto* w = scene().findFirst<vfx::Weather>()) w->setWeather(vfx::Weather::Type::Rain, 2.0f);
        // Throw a rock at the crate pyramid from 6 m away (crates count their collisions).
        auto crates = scene().findAllWithTag("crate");
        for (auto* c : crates)
            if (!c->getComponent<EventProbe>()) c->addComponent<EventProbe>();
        m_crateStart.clear();
        glm::vec3 top(0.0f, -1e9f, 0.0f); // the top crate: nothing resting on it
        for (auto* c : crates) {
            m_crateStart.push_back(c->transform().position());
            if (c->transform().position().y > top.y) top = c->transform().position();
        }
        if (auto* p = localPlayer(scene())) {
            // Aim a little above it to allow for the drop over 6 m.
            const glm::vec3 eye = top + glm::vec3(0.0f, 0.4f, 6.0f);
            const glm::vec3 d = glm::normalize(top + glm::vec3(0.0f, 0.75f, 0.0f) - eye);
            p->setPose(eye - glm::vec3(0.0f, p->eyeHeight, 0.0f), glm::degrees(ve::math::yawOf(d)),
                       glm::degrees(std::asin(d.y)));
            m_throwPending = true;
        }
        ++m_step;
    } else if (m_step == 2 && m_throwPending && m_t > 4.2f) {
        // (one frame later, once the head transform follows the new pose)
        if (auto* p = localPlayer(scene())) {
            p->throwRock();
        }
        m_throwPending = false;
    } else if (m_step == 2 && !m_throwPending && m_t > 7.0f) {
        auto crates = scene().findAllWithTag("crate");
        int moved = 0;
        for (size_t i = 0; i < crates.size() && i < m_crateStart.size(); ++i) {
            if (glm::length(crates[i]->transform().position() - m_crateStart[i]) > 0.2f) ++moved;
        }
        log("crates knocked by the thrown rock: " + std::to_string(moved) + " / " + std::to_string(crates.size()));
        log(std::string("object spawned from onDestroy(): ") + (scene().find("SpawnedFromOnDestroy") ? "yes" : "NO") +
            ", destroyed one gone: " + (scene().find("SpawnOnDestroy") ? "NO" : "yes"));
        int enters = 0, stays = 0, exits = 0;
        float impact = 0.0f;
        for (auto* c : crates) {
            if (auto* probe = c->getComponent<EventProbe>()) {
                enters += probe->enters;
                stays += probe->stays;
                exits += probe->exits;
                impact = std::max(impact, probe->maxImpact);
            }
        }
        log("crate collision events: " + std::to_string(enters) + " enter, " + std::to_string(stays) + " stay, " +
            std::to_string(exits) + " exit, max impact " + std::to_string(impact) + " m/s");
        if (auto* t = scene().find("TestTrigger"))
            if (auto* probe = t->getComponent<EventProbe>())
                log("trigger box: " + std::to_string(probe->triggerEnters) + " enter(s) (" + probe->lastTrigger + "), " +
                    std::to_string(probe->triggerExits) + " exit(s)");
        if (auto* pl = localPlayer(scene()))
            if (auto* probe = pl->getComponent<EventProbe>())
                log("player saw " + std::to_string(probe->triggerEnters) + " trigger enter(s) ('" + probe->lastTrigger +
                    "'), " + std::to_string(probe->triggerExits) + " exit(s)");
        if (auto* w = scene().findFirst<vfx::Weather>()) {
            log(std::string("weather now: ") + vfx::Weather::name(w->weather()) + ", rain=" + std::to_string(w->current().rain));
        }
        for (auto* rock : scene().findAllWithTag("thrown")) {
            auto* rb = rock->getComponent<ve::Rigidbody>();
            const glm::vec3 p = rock->transform().position();
            log("thrown rock at y=" + std::to_string(p.y) + " (ground " + std::to_string(terrain().height(p.x, p.z)) +
                "), speed " + std::to_string(rb ? glm::length(rb->linearVelocity()) : -1.0f));
        }
        log("physics bodies: " + std::to_string(physics().bodyCount()) + ", step ms: " + std::to_string(physics().lastStepMs));
        const ve::InstanceStats& is = renderer().instances().stats();
        log("instances: " + std::to_string(is.instances) + ", camera draws per LOD " + std::to_string(is.visibleLod[0]) +
            "/" + std::to_string(is.visibleLod[1]) + "/" + std::to_string(is.visibleLod[2]) + ", shadow draws " +
            std::to_string(is.visibleShadow));
        log("objects: " + std::to_string(scene().objectCount()) + ", draw calls: " +
            std::to_string(renderer().stats().drawCalls) + ", gpu ms: " + std::to_string(renderer().gpuFrameTimeMs()));
        // Foliage streaming: jump to the far corner and let cells stream.
        auto* foliageObject = scene().findFirst<ve::Foliage>();
        auto* foliage = foliageObject ? foliageObject->system() : nullptr;
        auto* player = localPlayer(scene());
        if (foliage && player) {
            const ve::FoliageStats& fs = foliage->stats();
            if (auto* fire = scene().find("Campfire"))
                if (auto* src = fire->getComponent<ve::AudioSource>())
                    log("audio: fire occlusion in the open: " + std::to_string(src->occluded()));
            log("foliage before: " + std::to_string(fs.instances) + " instances, " + std::to_string(fs.cells) + " cells, " +
                std::to_string(fs.bodies) + " bodies");
            const glm::vec2 corner(-240.0f, -240.0f);
            player->teleport({corner.x, terrain().height(corner.x, corner.y), corner.y}, 0.0f);
            m_t = 0.0f;
            ++m_step;
        } else {
            engine().quit();
            m_step = 100;
        }
    } else if (m_step == 3 && m_t > 4.0f) {
        auto* foliageObject = scene().findFirst<ve::Foliage>();
        ve::FoliageSystem* foliage = foliageObject->system();
        const ve::FoliageStats& fs = foliage->stats();
        log("foliage after teleport: " + std::to_string(fs.instances) + " instances, " + std::to_string(fs.cells) +
            " cells, " + std::to_string(fs.pending) + " pending, " + std::to_string(fs.bodies) + " bodies, " +
            std::to_string(fs.dropped) + " dropped, " + std::to_string(fs.generateMs) + " ms per cell");
        // Bushes are deterministic: the instances near the player must match a fresh query.
        const glm::vec3 p = localPlayer(scene())->transform().position();
        logAudio("far corner");
        audio().music().pushMood("Combat", 10, 1.0f); // as a fight would
        if (auto* dn = scene().findFirst<vfx::DayNightCycle>()) dn->timeOfDay = 22.0f;
        log("bushes within 30 m (query): " + std::to_string(foliage->query(uint32_t(foliageObject->layerIndex("Bushes")), glm::vec2(p.x, p.z), 30.0f).size()));
        // Audio: step inside a hut (zone muffles the Weather bus) and check the fire's occlusion.
        if (auto* hutZone = scene().find("HutZone")) {
            const glm::vec3 hp = hutZone->transform().position();
            localPlayer(scene())->teleport(hp + glm::vec3(0.0f, 0.2f, 0.0f), 0.0f);
        }
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 4 && m_t > 2.0f) {
        const ve::AudioBus& weather = audio().bus("Weather");
        int inside = 0;
        scene().forEach<ve::AudioZone>([&](ve::AudioZone& z) { inside += z.listenerInside() ? 1 : 0; });
        float fireOcclusion = -1.0f;
        if (auto* fire = scene().find("Campfire"))
            if (auto* src = fire->getComponent<ve::AudioSource>()) fireOcclusion = src->occluded();
        log("audio: inside " + std::to_string(inside) + " hut zone(s), Weather bus x" + std::to_string(weather.zoneVolume) +
            " / " + std::to_string(weather.zoneLowpass) + " Hz, fire occlusion " + std::to_string(fireOcclusion));
        logAudio("hut, 22:00, combat pushed");
        audio().music().popMood("Combat");
        log("audio: combat popped -> mood '" + audio().music().currentMood() + "'");
        // The big world: walk into the desert, then the jungle (terrain LOD,
        // physics tiles and foliage all stream around the player).
        if (auto* dn = scene().findFirst<vfx::DayNightCycle>()) dn->timeOfDay = 12.0f;
        localPlayer(scene())->teleport(groundAt({1200.0f, -300.0f}), 0.0f);
        m_t = 0.0f;
        ++m_step;
    } else if ((m_step == 5 || m_step == 6) && m_t > 4.0f) {
        const bool desert = m_step == 5;
        const char* biome = desert ? "Desert" : "Jungle";
        auto* player = localPlayer(scene());
        const glm::vec3 p = player->transform().position();
        auto* foliageObject = scene().findFirst<ve::Foliage>();
        const int layer = foliageObject ? foliageObject->layerIndex(desert ? "Quiver trees" : "Jungle trees") : -1;
        const size_t trees = layer >= 0 ? foliageObject->system()->query(uint32_t(layer), glm::vec2(p.x, p.z), 100.0f).size() : 0;
        log(std::string(biome) + ": weight " + std::to_string(terrain().biomeWeight(biome, p.x, p.z)) + ", player grounded: " +
            (player->grounded() ? "yes" : "NO") + " (feet " + std::to_string(p.y) + ", ground " +
            std::to_string(terrain().height(p.x, p.z)) + "), " + std::to_string(trees) + " " +
            (desert ? "quiver trees" : "jungle trees") + " within 100 m, terrain nodes " + std::to_string(terrain().drawnNodes()) +
            ", collider tiles " + std::to_string(terrain().colliderTiles()) + ", foliage " +
            std::to_string(foliageObject ? foliageObject->system()->stats().instances : 0) + " instances");
        if (desert) {
            player->teleport(groundAt({-1150.0f, 450.0f}), 0.0f);
        } else {
            // Interactive foliage: stand next to an acacia near the camp.
            const int acacias = foliageObject ? foliageObject->layerIndex("Acacias") : -1;
            const auto tree = acacias >= 0 ? foliageObject->system()->nearest(uint32_t(acacias), {90.0f, 60.0f}, 150.0f) : std::nullopt;
            if (!tree) {
                log("interactive foliage: no acacia found");
                engine().quit();
            } else {
                m_tree = tree->id;
                // 7 m east of it (inside its promoteDistance), looking west at it.
                player->setPose(groundAt(glm::vec2(tree->position.x + 7.0f, tree->position.z)), 90.0f, 8.0f);
            }
        }
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 7 && m_t > 1.0f) {
        shot("1-acacia");
        // Step up to the trunk and look at it: the player's Interactor should focus it.
        if (const auto info = scene().findFirst<ve::Foliage>()->system()->instance(m_tree))
            localPlayer(scene())->setPose(groundAt(glm::vec2(info->position.x + 1.3f, info->position.z)), 90.0f, -10.0f);
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 8 && m_t > 0.3f) {
        ve::FoliageSystem* foliage = scene().findFirst<ve::Foliage>()->system();
        auto* tree = dynamic_cast<ChoppableTree*>(foliage->promoted(m_tree));
        log(std::string("acacia promoted to a ChoppableTree: ") + (tree ? "yes" : "NO"));
        // Through the engine's interaction system, like pressing E.
        auto* who = localPlayer(scene())->getComponent<ve::Interactor>();
        const ve::Interactable* focus = who ? who->focus() : nullptr;
        log("interactor focus: " + std::string(focus && &focus->gameObject() == tree ? "the acacia" : focus ? focus->gameObject().name() : "nothing") +
            ", prompt '" + (who ? who->prompt() : std::string()) + "'");
        int uses = 0;
        for (int i = 0; who && i < ChoppableTree::kChops; ++i) uses += who->interact() ? 1 : 0;
        const bool felled = foliage->state(m_tree) != 0;
        log("chopped " + std::to_string(uses) + " times: felled " + std::to_string(felled ? 1 : 0) + ", state '" +
            stateLabel(*foliage, m_tree) + "', wood " + std::to_string(gameState().inventory.count("wood")));
        localPlayer(scene())->teleport(groundAt({1200.0f, -300.0f}), 0.0f); // far away: the cell unloads
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 9 && m_t > 2.0f) {
        const auto info = scene().findFirst<ve::Foliage>()->system()->instance(m_tree);
        localPlayer(scene())->setPose(groundAt(glm::vec2(info->position.x + 3.0f, info->position.z)), 90.0f, -28.0f);
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 10 && m_t > 2.0f) {
        shot("2-stump");
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 11 && m_t > 0.3f) {
        ve::FoliageSystem* foliage = scene().findFirst<ve::Foliage>()->system();
        auto* obj = foliage->promoted(m_tree);
        auto* link = obj ? obj->getComponent<ve::FoliageLink>() : nullptr;
        log("back at the tree: state '" + stateLabel(*foliage, m_tree) + "', promoted object shows '" +
            (link ? link->stateName() : std::string("none")) + "', " + std::to_string(foliage->overrides().size()) +
            " override(s) to save");
        // Berries: pick a bush, then let them grow back.
        auto* foliageObject = scene().findFirst<ve::Foliage>();
        const int layer = foliageObject->layerIndex("Berry bushes");
        const auto bush = layer >= 0 ? foliage->nearest(uint32_t(layer), {60.0f, 40.0f}, 400.0f) : std::nullopt;
        if (!bush) {
            log("interactive foliage: no berry bush found");
            engine().quit();
        } else {
            m_bush = bush->id;
            localPlayer(scene())->setPose(groundAt(glm::vec2(bush->position.x + 2.5f, bush->position.z)), 90.0f, -25.0f);
            if (Regrowth* r = Regrowth::find(scene())) r->regrowSeconds = 2.0f;
        }
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 12 && m_t > 1.0f) {
        shot("3-berry-bush");
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 13 && m_t > 0.3f) {
        ve::FoliageSystem* foliage = scene().findFirst<ve::Foliage>()->system();
        auto* bush = dynamic_cast<BerryBush*>(foliage->promoted(m_bush));
        const bool picked = bush && bush->pick();
        log(std::string("berry bush promoted: ") + (bush ? "yes" : "NO") + ", picked: " + (picked ? "yes" : "no") + ", state '" +
            stateLabel(*foliage, m_bush) + "', berries " + std::to_string(gameState().inventory.count("berries")));
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 14 && m_t > 3.0f) {
        ve::FoliageSystem* foliage = scene().findFirst<ve::Foliage>()->system();
        log("after the regrowth time: berry bush state '" + stateLabel(*foliage, m_bush) + "'");
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 14 && m_t > 0.5f) {
        shot("4-berries-picked");
    } else if ((m_step == 15 || m_step == 16) && m_t > 1.5f) {
        // F2: the camp villagers (skinned, animated) wander and swing their legs.
        std::string line;
        size_t i = 0;
        for (const auto& obj : scene().objects()) {
            auto* vp = dynamic_cast<Villager*>(obj.get());
            if (!vp || vp->isDestroyed()) continue;
            Villager& v = *vp;
            const ve::Animator* a = v.animator();
            const ve::GameObject* thigh = nullptr;
            forSubtree(v.transform(), [&](ve::Transform& t) {
                if (!thigh && t.gameObject().name() == "UpperLeg.L") thigh = &t.gameObject();
            });
            const float angle = thigh ? glm::degrees(glm::angle(thigh->transform().localRotation())) : -1.0f;
            const glm::vec3 p = v.transform().position();
            float top = 0.0f;
            forSubtree(v.transform(), [&](ve::Transform& t) {
                if (const auto* sm = t.gameObject().getComponent<ve::SkinnedMeshRenderer>(); sm && sm->mesh) top = sm->mesh->bounds().max.y;
            });
            line += (i++ ? "; " : "") + std::string("clip '") + (a ? a->current() : "?") + "' thigh " + std::to_string(angle) + " deg at (" +
                    std::to_string(p.x) + ", " + std::to_string(p.z) + "), skinned top " + std::to_string(top);
        }
        log("villagers " + std::to_string(i) + ": " + line + "; skinned this frame " + std::to_string(renderer().skinning().skinnedThisFrame()));
        if (m_step == 15) shot("5-villagers");
        if (m_step == 16) {
            // Interaction without a collider: face a sunstone 2 m away (the Interactor's view cone).
            auto stones = scene().findAllWithTag("collectible");
            if (!stones.empty()) {
                const glm::vec3 s = stones.front()->transform().position();
                auto* player = localPlayer(scene());
                player->setPose(groundAt(glm::vec2(s.x + 2.0f, s.z)), 90.0f, -12.0f);
            }
        }
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 17 && m_t > 0.5f) {
        auto* who = localPlayer(scene())->getComponent<ve::Interactor>();
        const int before = gameState().collected;
        const std::string prompt = who ? who->prompt() : std::string();
        const bool used = who && who->interact();
        log("sunstone via the interactor: prompt '" + prompt + "', used " + (used ? "yes" : "no") + ", collected " +
            std::to_string(before) + " -> " + std::to_string(gameState().collected));
        // Save games: save now, change things, load, and check they're back.
        const GameState& gs = gameState();
        s_expect = {gs.collected, gs.inventory.count("wood"), gs.inventory.count("berries"), Sunstone::remaining(scene()), localPlayer(scene())->transform().position(), m_tree.packed()};
        log(std::string("saved: ") + (saveProgress(scene(), "autotest") ? "yes" : "NO") + " (collected " + std::to_string(gs.collected) +
            ", wood " + std::to_string(gs.inventory.count("wood")) + ", " + std::to_string(s_expect.stones) + " stones left)");
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 18 && m_t > 0.5f) {
        auto stones = scene().findAllWithTag("collectible");
        if (auto* stone = stones.empty() ? nullptr : dynamic_cast<Sunstone*>(stones.front())) stone->collect();
        gameState().inventory.add("wood", 5);
        localPlayer(scene())->teleport(groundAt({1200.0f, -300.0f}), 0.0f);
        log("after the save: collected " + std::to_string(gameState().collected) + ", wood " + std::to_string(gameState().inventory.count("wood")) +
            ", player moved 1.2 km");
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 19 && m_t > 0.5f) {
        s_resume = 20; // the reloaded level's AutoTest carries on
        log(std::string("loading the save (level rebuilt behind the loading screen): ") + (loadProgress("autotest") ? "started" : "NO SAVE"));
        ++m_step;
    } else if (m_step == 20 && m_t > 3.0f) {
        const GameState& gs = gameState();
        const glm::vec3 p = localPlayer(scene())->transform().position();
        auto* foliage = scene().findFirst<ve::Foliage>();
        const ve::FoliageInstanceId tree = ve::FoliageInstanceId::unpack(s_expect.tree);
        const std::string treeState = foliage && foliage->system() ? stateLabel(*foliage->system(), tree) : "?";
        log("after loading: collected " + std::to_string(gs.collected) + " (saved " + std::to_string(s_expect.collected) + "), wood " +
            std::to_string(gs.inventory.count("wood")) + " (" + std::to_string(s_expect.wood) + "), stones left " + std::to_string(Sunstone::remaining(scene())) +
            " (" + std::to_string(s_expect.stones) + "), player " + std::to_string(glm::distance(p, s_expect.player)) +
            " m from the saved spot, chopped acacia '" + treeState + "'");
        ve::save::remove("autotest");
        // Gamepad: a virtual SDL controller drives the player (stick forward, then Y = stop flying).
        const bool pad = input().attachVirtualGamepad();
        m_padStart = localPlayer(scene())->transform().position();
        input().setVirtualGamepadAxis(ve::GamepadAxis::LeftY, -1.0f); // stick fully forward
        log(std::string("virtual gamepad: ") + (pad ? "attached" : "NOT AVAILABLE") + ", connected '" + input().gamepadName() + "'");
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 21 && m_t > 1.0f) {
        auto* player = localPlayer(scene());
        const float moved = glm::distance(player->transform().position(), m_padStart);
        log("gamepad: MoveY " + std::to_string(input().axisValue("MoveY")) + ", moved " + std::to_string(moved) + " m in 1 s (flying " +
            (player->flying() ? "yes" : "no") + ")");
        input().setVirtualGamepadAxis(ve::GamepadAxis::LeftY, 0.0f);
        input().setVirtualGamepadButton(ve::GamepadButton::Y, true); // ToggleFly
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 22 && m_t > 0.3f) {
        input().setVirtualGamepadButton(ve::GamepadButton::Y, false);
        auto* player = localPlayer(scene());
        log(std::string("gamepad: Y pressed -> flying ") + (player->flying() ? "yes" : "no") + "; ToggleFly is bound to " +
            input().bindingText("ToggleFly"));
        // Bindings survive a JSON round trip (the F10 controls file).
        const ve::Json saved = input().saveBindings();
        input().clearAction("ToggleFly");
        input().loadBindings(saved);
        log("bindings JSON round trip: ToggleFly has " + std::to_string(input().bindingNames("ToggleFly").size()) + " binding(s)");
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 23 && m_t > 0.5f) {
        // Navigation: the level's navmesh (built behind the loading screen) routes around huts.
        ve::NavMeshSurface* nav = ve::NavMeshSurface::find(scene());
        if (!nav) {
            log("navmesh: NONE");
            engine().quit();
            return;
        }
        const auto& st = nav->stats();
        log("navmesh: ready " + std::string(nav->ready() ? "yes" : "no") + ", tiles " + std::to_string(st.tiles) + ", polygons " +
            std::to_string(st.polygons) + ", " + std::to_string(st.lastTileMs) + " ms per tile");
        ve::GameObject* hut = scene().find("Hut");
        if (hut) {
            const glm::vec3 h = hut->transform().position();
            const glm::vec3 a = groundAt({h.x - 6.0f, h.z}), b = groundAt({h.x + 6.0f, h.z});
            const ve::NavPath path = nav->findPath(a, b);
            const auto blocked = nav->raycast(a, b);
            log("path across a hut: complete " + std::string(path.complete ? "yes" : "no") + ", " + std::to_string(path.corners.size()) +
                " corners, " + std::to_string(path.length()) + " m (straight 12 m), straight line blocked " + (blocked ? "yes" : "no"));
        }
        // A villager follows the player (the game's AI on the engine's NavAgent).
        Villager* v = nullptr;
        for (const auto& obj : scene().objects())
            if (!v && !obj->isDestroyed()) v = dynamic_cast<Villager*>(obj.get());
        auto* player = localPlayer(scene());
        if (v && player) {
            const glm::vec3 vp = v->transform().position();
            const glm::vec3 goal = nav->randomPointNear(vp + glm::vec3(9.0f, 0.0f, 8.0f), 3.0f).value_or(vp + glm::vec3(9.0f, 0.0f, 8.0f));
            player->setPose(goal, 0.0f, -10.0f);
            v->follow(*player);
            m_followStart = glm::distance(vp, goal);
            m_follower = v;
            log("villager follows: start " + std::to_string(m_followStart) + " m from the player, on navmesh " +
                (v->agent().onNavMesh() ? "yes" : "no"));
        }
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 24 && m_t > 9.0f) {
        auto* v = m_follower.get(scene());
        auto* player = localPlayer(scene());
        if (v && player) {
            const glm::vec3 vp = v->transform().position(), pp = player->transform().position();
            log("villager after 9 s: " + std::to_string(glm::distance(glm::vec2(vp.x, vp.z), glm::vec2(pp.x, pp.z))) + " m from the player (was " +
                std::to_string(m_followStart) + "), following " + (v->mode() == Villager::Mode::Follow ? "yes" : "no") + ", walking " +
                (v->walking() ? "yes" : "no"));
            // Look at the villager from a few metres back for the screenshot, navmesh drawn.
            const glm::vec3 d = vp - pp;
            player->setPose(pp - glm::normalize(glm::vec3(d.x, 0.0f, d.z)) * 3.0f + glm::vec3(0.0f, 1.0f, 0.0f),
                            glm::degrees(ve::math::yawOf(d)), -18.0f);
            if (ve::NavMeshSurface* nav = ve::NavMeshSurface::find(scene())) nav->showDebug = true;
        }
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 25 && m_t > 0.5f) {
        shot("6-villager-follows");
        if (m_t > 1.0f) {
            if (ve::NavMeshSurface* nav = ve::NavMeshSurface::find(scene())) nav->showDebug = false;
            // Building: the build mode's own code path (snap, fit, place), aimed by the test.
            m_site = groundAt(layout::kCamp + glm::vec2(-13.0f, 13.0f));
            m_site.y -= 0.1f;
            buildStep(0, m_site, 0.0f); // stone foundation
        }
    } else if (m_step >= 26 && m_step <= 44 && m_t > 0.4f && m_frames - m_stepFrame >= 4) {
        auto* b = scene().findFirst<Builder>();
        auto pos = [&](size_t i) {
            ve::GameObject* o = i < m_built.size() ? m_built[i].get(scene()) : nullptr;
            return o ? o->transform().position() : glm::vec3(-9999.0f);
        };
        auto placeLog = [&](const char* what, const glm::vec3& expected) {
            const bool valid = b->previewValid(), snapped = b->previewSnapped();
            ve::GameObject* o = b->place();
            if (o) m_built.push_back(o);
            const float err = o ? glm::distance(o->transform().position(), expected) : -1.0f;
            log(std::string("build: ") + what + " placed " + (o ? "yes" : "NO") + ", valid " + (valid ? "yes" : "no") + ", snapped " +
                (snapped ? "yes" : "no") + ", " + std::to_string(err) + " m from the expected spot");
        };
        const glm::vec3 a = pos(0);
        switch (m_step) {
        case 26:
            placeLog("foundation", b->previewPosition());
            buildStep(0, m_site + glm::vec3(2.3f, 0.2f, 0.15f), 0.0f); // next to it, a little off
            break;
        case 27:
            placeLog("2nd foundation (snaps 2 m east)", a + glm::vec3(2.0f, 0.0f, 0.0f));
            buildStep(0, a + glm::vec3(0.1f, 0.0f, 0.1f), 0.0f); // on top of the first
            break;
        case 28:
            log(std::string("build: a 3rd foundation inside the 1st is ") + (b->previewValid() ? "ALLOWED" : "blocked") +
                (b->place() ? " (PLACED)" : ""));
            buildStep(2, a + glm::vec3(0.25f, 0.6f, -0.85f), 0.0f); // mud wall on the north edge of the first
            break;
        case 29:
            placeLog("mud wall", a + glm::vec3(0.0f, BuildPiece::kFoundation, -1.0f));
            buildStep(4, pos(1) + glm::vec3(-0.2f, 0.6f, -0.9f), 0.0f); // door wall on the second's north edge
            break;
        case 30:
            placeLog("door wall", pos(1) + glm::vec3(0.0f, BuildPiece::kFoundation, -1.0f));
            buildStep(5, a + glm::vec3(-0.9f, 0.6f, 0.2f), 90.0f); // window wall along the west edge
            break;
        case 31:
            placeLog("window wall", a + glm::vec3(-1.0f, BuildPiece::kFoundation, 0.0f));
            // Roofs: eaves (the low edge) on the north walls' tops, rising south over the rooms.
            buildStep(8, a + glm::vec3(0.1f, BuildPiece::kFoundation + BuildPiece::kWall + 0.15f, 0.1f), 180.0f);
            break;
        case 32:
            placeLog("metal roof", a + glm::vec3(0.0f, BuildPiece::kFoundation + BuildPiece::kWall, 0.0f));
            buildStep(9, pos(1) + glm::vec3(0.1f, BuildPiece::kFoundation + BuildPiece::kWall + 0.15f, 0.1f), 180.0f); // thatch beside it
            break;
        case 33:
            placeLog("thatched roof", pos(1) + glm::vec3(0.0f, BuildPiece::kFoundation + BuildPiece::kWall, 0.0f));
            buildStep(10, a + glm::vec3(0.2f, 0.05f, 1.6f), 0.0f); // stairs up to the first foundation from the south
            // Look at the site: the screenshot shows the stairs' ghost and the build panel.
            localPlayer(scene())->setPose(a + glm::vec3(-4.5f, 1.0f, 6.5f), glm::degrees(ve::math::yawOf(glm::vec3(4.5f, 0.0f, -6.5f))), -14.0f);
            break;
        case 34:
            if (!m_buildShot) { // one frame with the stairs' ghost on screen first
                shot("7a-build-mode");
                m_buildShot = true;
                break;
            }
            placeLog("stairs", a + glm::vec3(0.0f, 0.0f, 1.5f));
            // A door into the door wall's opening, aimed with a skewed rotation: it turns to match.
            buildStep(13, pos(3) + glm::vec3(0.25f, 1.2f, 0.3f), 37.0f);
            break;
        case 35: {
            placeLog("door (into the opening)", pos(3));
            auto yawGap = [&](size_t piece, size_t host) { // degrees between their x axes, mod 180
                ve::GameObject* x = piece < m_built.size() ? m_built[piece].get(scene()) : nullptr;
                ve::GameObject* y = host < m_built.size() ? m_built[host].get(scene()) : nullptr;
                if (!x || !y) return -1.0f;
                const glm::vec3 ax = x->transform().rotation() * glm::vec3(1, 0, 0), ay = y->transform().rotation() * glm::vec3(1, 0, 0);
                return glm::degrees(std::acos(std::clamp(std::abs(glm::dot(ax, ay)), 0.0f, 1.0f)));
            };
            log("build: door turned to its wall: " + std::to_string(yawGap(8, 3)) + " deg off (aimed at 37 deg)");
            buildStep(14, pos(4) + glm::vec3(0.2f, 1.5f, 0.1f), 100.0f); // a window into the window wall
            break;
        }
        case 36: {
            placeLog("window (into the opening)", pos(4));
            ve::GameObject* x = m_built.size() > 9 ? m_built[9].get(scene()) : nullptr;
            ve::GameObject* y = m_built[4].get(scene());
            const float gap = x && y ? glm::degrees(std::acos(std::clamp(std::abs(glm::dot(x->transform().rotation() * glm::vec3(1, 0, 0),
                                                                                             y->transform().rotation() * glm::vec3(1, 0, 0))),
                                                                       0.0f, 1.0f)))
                                     : -1.0f;
            log("build: window turned to its wall: " + std::to_string(gap) + " deg off (aimed at 100 deg)");
            // Open the door the way the player does (Use on its Interactable).
            if (ve::GameObject* door = m_built[8].get(scene()))
                if (auto* use = door->getComponent<ve::Interactable>()) use->use(*localPlayer(scene()));
            buildStep(12, pos(1) + glm::vec3(3.2f, 0.0f, 0.4f), 0.0f); // a fence off the second foundation, then remove it
            break;
        }
        case 37: {
            float leaf = -1.0f;
            if (ve::GameObject* door = m_built[8].get(scene()))
                for (ve::Transform* c : door->transform().children())
                    if (c->gameObject().name() == "Door leaf") leaf = glm::degrees(glm::angle(c->localRotation()));
            auto* hinge = m_built[8].get(scene()) ? m_built[8].get(scene())->getComponent<DoorHinge>() : nullptr;
            log(std::string("build: door opened with Use: ") + (hinge && hinge->open() ? "yes" : "NO") + ", leaf swung " +
                std::to_string(leaf) + " deg");
            placeLog("fence", b->previewPosition());
            ve::GameObject* fence = m_built.empty() ? nullptr : m_built.back().get(scene());
            const int removed = b->remove(fence);
            if (removed) m_built.pop_back();
            log("build: fence removed " + std::to_string(removed));
            // Removing a wall takes the door fitted into it along.
            buildStep(4, pos(1) + glm::vec3(0.1f, 0.6f, 0.85f), 0.0f);
            break;
        }
        case 38:
            placeLog("2nd door wall", pos(1) + glm::vec3(0.0f, BuildPiece::kFoundation, 1.0f));
            buildStep(13, pos(m_built.size() - 1) + glm::vec3(0.2f, 1.0f, 0.2f), 200.0f);
            break;
        case 39:
            placeLog("2nd door", pos(m_built.size() - 1));
            m_t = 0.0f;
            m_stepFrame = m_frames;
            ++m_step;
            break;
        case 40: {
            const ve::ObjectRef<> wall = m_built[m_built.size() - 2], door = m_built.back();
            const int removed = b->remove(wall.get(scene()));
            log("build: removing the 2nd door wall removed " + std::to_string(removed) + " piece(s), its door gone " +
                (door.get(scene()) ? "NO" : "yes"));
            m_built.resize(m_built.size() - 2);
            b->setAimOverride(std::nullopt);
            b->setActive(false);
            // Exclusions while playing: a box dropped on an existing foliage instance clears it
            // (the path a foundation's FoliageExclusion takes), and taking it away brings it back.
            m_exclusionAt = glm::vec3(0.0f);
            if (auto* f = scene().findFirst<ve::Foliage>(); f && f->system())
                for (uint32_t l = 0; l < f->system()->layerCount() && m_exclusionAt == glm::vec3(0.0f); ++l) {
                    auto near = f->system()->query(l, layout::kCamp + glm::vec2(30.0f, 30.0f), 25.0f);
                    if (!near.empty()) m_exclusionAt = near.front();
                }
            m_foliageOnSite = foliageAt(m_exclusionAt, 0.3f);
            auto& ex = scene().spawn("Exclusion test");
            ex.transform().setPosition(m_exclusionAt);
            ex.transform().setRotation(glm::angleAxis(glm::radians(30.0f), glm::vec3(0, 1, 0)));
            auto& fe = ex.addComponent<ve::FoliageExclusion>();
            fe.box = true;
            fe.size = {2.0f, 2.0f};
            m_exclusionObject = &ex;
            m_t = 0.0f;
            m_stepFrame = m_frames;
            ++m_step;
            break;
        }
        case 41: {
            const int during = foliageAt(m_exclusionAt, 0.3f);
            if (ve::GameObject* ex = m_exclusionObject.get(scene())) ex->destroy();
            log("build: foliage at a spot " + std::to_string(m_foliageOnSite) + " before an exclusion box, " + std::to_string(during) +
                " with it");
            m_foliageDuring = during;
            m_t = 0.0f;
            m_stepFrame = m_frames;
            ++m_step;
            break;
        }
        case 42: {
            log("build: ... and " + std::to_string(foliageAt(m_exclusionAt, 0.3f)) + " after removing it");
            m_t = 0.0f;
            m_stepFrame = m_frames;
            ++m_step;
            break;
        }
        case 43: {
            // Save, then load (the level rebuilt behind the loading screen): the pieces come back.
            s_expect.pieces.clear();
            for (size_t i = 0; i < m_built.size(); ++i)
                if (ve::GameObject* o = m_built[i].get(scene())) s_expect.pieces.push_back({o->name(), o->transform().position()});
            const bool saved = ve::save::saveGame(scene(), "buildtest");
            log("build: " + std::to_string(s_expect.pieces.size()) + " pieces, saved " + (saved ? "yes" : "NO"));
            s_resume = 45;
            log(std::string("build: loading the save: ") + (loadProgress("buildtest") ? "started" : "NO SAVE"));
            ++m_step;
            break;
        }
        default: break;
        }
    } else if (m_step == 45 && m_t > 3.0f) {
        int found = 0;
        float worst = 0.0f;
        for (const auto& [name, p] : s_expect.pieces) {
            float best = 1e9f;
            for (const auto& o : scene().objects()) {
                if (o->isDestroyed() || !dynamic_cast<BuildPiece*>(o.get()) || o->name() != name) continue;
                best = std::min(best, glm::distance(o->transform().position(), p));
            }
            if (best < 0.01f) ++found;
            worst = std::max(worst, best);
        }
        log("build: after loading, " + std::to_string(found) + " of " + std::to_string(s_expect.pieces.size()) +
            " pieces back in place (worst " + std::to_string(worst) + " m)");
        bool doorOpen = false;
        scene().forEach<DoorHinge>([&](DoorHinge& h) { doorOpen = doorOpen || h.open(); });
        log(std::string("build: the door is still open after loading: ") + (doorOpen ? "yes" : "NO"));
        ve::save::remove("buildtest");
        // Look at the little house for the screenshot.
        if (!s_expect.pieces.empty()) {
            // From the south-west: the late sun lights the faces we see.
            const glm::vec3 c = s_expect.pieces.front().second + glm::vec3(1.0f, 0.0f, 0.0f);
            localPlayer(scene())->setPose(c + glm::vec3(-6.0f, 1.2f, 6.5f), glm::degrees(ve::math::yawOf(glm::vec3(6.0f, 0.0f, -6.5f))), -12.0f);
        }
        m_t = 0.0f;
        ++m_step;
    } else if (m_step == 46 && m_t > 1.5f) {
        shot("7-building");
        if (m_t > 2.0f) {
            m_t = 0.0f;
            m_step = 63; // pickups (a ProxySet), then physics layers / surfaces, then the UI
        }
    } else if (m_step >= 63 && m_step <= 69) {
        pickupSteps(log);
    } else if (m_step >= 70) {
        physicsSteps(log);
    } else if (m_step >= 47) {
        uiSteps(log);
    }
}

namespace {
// Physics test helpers: what a body touched, and a character that just stands.
class ContactRecorder : public ve::Component {
public:
    std::string surface;
    int layer = -1;
    void onCollisionEnter(const ve::Collision& c) override
    {
        if (surface.empty()) {
            surface = c.otherSurface;
            layer = c.otherLayer;
        }
    }
};
class StandStill : public ve::Component {
public:
    void fixedUpdate(float dt) override
    {
        if (auto* c = gameObject().getComponent<ve::CharacterController>()) c->move(glm::vec3(0.0f), dt);
    }
};
} // namespace

// Collision layers and physics surfaces (engine), then on to the UI steps.
void AutoTest::physicsSteps(const std::function<void(const std::string&)>& log)
{
    ve::PhysicsWorld& ph = physics();
    const glm::vec2 spot = layout::kCamp + glm::vec2(-14.0f, 16.0f);
    const glm::vec3 ground = groundAt(spot);
    const float platformY = ground.y + 4.0f;
    switch (m_step) {
    case 70: {
        const int platformLayer = ph.addLayer("TestPlatform"), ghostLayer = ph.addLayer("TestGhost");
        ph.setCollision(platformLayer, ghostLayer, false);
        auto& platform = scene().spawn("Test platform");
        platform.transform().setPosition({spot.x, platformY, spot.y});
        auto& pb = platform.addComponent<ve::Rigidbody>(ve::Collider::box({1.5f, 0.15f, 1.5f}), ve::BodyType::Static);
        pb.layer = "TestPlatform";
        pb.surface = "Stone";
        auto box = [&](const char* name, const char* layer, float dx) -> ve::GameObject& {
            auto& b = scene().spawn(name);
            b.transform().setPosition({spot.x + dx, platformY + 1.5f, spot.y});
            b.addComponent<ve::Rigidbody>(ve::Collider::box(glm::vec3(0.2f)), ve::BodyType::Dynamic).layer = layer;
            b.addComponent<ContactRecorder>();
            return b;
        };
        m_physGhost = &box("Ghost box", "TestGhost", -0.6f);
        m_physSolid = &box("Solid box", "Default", 0.6f);
        m_physPlatform = &platform;
        log("layers: " + std::to_string(ph.layerCount()) + " (Default, Debris, TestPlatform, TestGhost), TestPlatform x TestGhost collide: " +
            (ph.collides(platformLayer, ghostLayer) ? "YES (wrong)" : "no"));
        m_stepFrame = m_frames;
        m_t = 0.0f;
        ++m_step;
        break;
    }
    case 71: {
        if (m_t < 2.5f) break;
        ve::GameObject* ghost = m_physGhost.get(scene());
        ve::GameObject* solid = m_physSolid.get(scene());
        if (!ghost || !solid) break;
        const float gy = ghost->transform().position().y, sy = solid->transform().position().y;
        const auto* rec = solid->getComponent<ContactRecorder>();
        log("layers: the TestGhost box fell through the platform: " + std::string(gy < platformY - 1.0f ? "yes" : "NO") +
            ", the Default box rests on it: " + (sy > platformY ? "yes" : "NO") + " (contact surface '" + (rec ? rec->surface : "?") +
            "', layer " + ph.layerName(rec ? rec->layer : -1) + ")");
        // Queries: all layers -> the platform; without TestPlatform -> the ground under it.
        const glm::vec3 from{spot.x + 1.2f, platformY + 3.0f, spot.y};
        const auto all = ph.raycast(from, {0, -1, 0}, 20.0f, ve::QueryFilter{});
        ve::QueryFilter noPlatform;
        noPlatform.layers = ~ph.layerMask({"TestPlatform"});
        const auto masked = ph.raycast(from, {0, -1, 0}, 20.0f, noPlatform);
        log("queries: unmasked ray hits the platform: " + std::string(all && all->gameObject && all->gameObject->id() == m_physPlatform.id() ? "yes" : "NO") +
            " (surface '" + (all ? all->surface : "") + "', layer " + (all ? ph.layerName(all->layer) : "") +
            "), masked ray reaches the ground: " + (masked && masked->point.y < platformY - 1.0f ? "yes" : "NO") + " (surface '" +
            (masked ? masked->surface : "") + "')");
        for (const ve::ObjectRef<>& ref : {m_physGhost, m_physSolid, m_physPlatform})
            if (ve::GameObject* o = ref.get(scene())) o->destroy();
        // A character standing on a stone foundation, then on the ground.
        const BuildPiece* stone = nullptr;
        for (const auto& o : scene().objects())
            if (auto* p = dynamic_cast<BuildPiece*>(o.get()); p && !p->isDestroyed() && p->kind == BuildPiece::Kind::Foundation &&
                                                            p->material == BuildPiece::Material::Stone && !ve::PlacementGhost::is(*p))
                stone = p;
        auto& probe = scene().spawn("Ground probe");
        probe.transform().setPosition(stone ? stone->transform().position() + glm::vec3(0.0f, BuildPiece::kFoundation + 0.05f, 0.0f)
                                            : ground + glm::vec3(0, 0.05f, 0));
        probe.addComponent<ve::CharacterController>();
        probe.addComponent<StandStill>();
        m_physProbe = &probe;
        m_physOnStone = stone != nullptr;
        m_t = 0.0f;
        ++m_step;
        break;
    }
    case 72:
    case 73: {
        if (m_t < 1.0f) break;
        ve::GameObject* probe = m_physProbe.get(scene());
        auto* cc = probe ? probe->getComponent<ve::CharacterController>() : nullptr;
        if (!cc) break;
        if (m_step == 72) {
            log("surfaces: a character on " + std::string(m_physOnStone ? "a stone foundation" : "(no stone foundation found)") +
                " stands on '" + cc->groundSurface() + "'");
            const glm::vec3 g = groundAt(spot + glm::vec2(3.0f, 0.0f));
            cc->teleport(g + glm::vec3(0, 0.05f, 0));
            m_t = 0.0f;
            ++m_step;
        } else {
            log("surfaces: ... and on the terrain '" + cc->groundSurface() + "'");
            probe->destroy();
            m_t = 0.0f;
            m_stepFrame = m_frames;
            m_step = 74; // animation layers / events / IK
        }
        break;
    }
    case 74: { // a villager: wave (layer), walk (footstep events), a hand reaching (IK)
        Villager* v = scene().findFirst<Villager>();
        if (!v || !v->animator() || !v->body()) {
            log("anim: no villager");
            m_step = 47;
            break;
        }
        m_animVillager = v;
        m_animSteps = v->steps();
        v->wave(3.0f);
        // Walk somewhere 6 m away (footstep events while walking).
        if (auto* nav = ve::NavMeshSurface::find(scene()))
            if (auto p = nav->randomPointNear(v->transform().position() + glm::vec3(6.0f, 0.0f, 0.0f), 2.0f)) v->agent().setDestination(*p);
        // The left hand reaches for a point in front of the shoulder (two-bone IK).
        auto& ik = v->body()->addComponent<ve::TwoBoneIK>("UpperArm.L", "LowerArm.L", "Hand.L");
        ik.pole = {0.0f, 0.0f, -1.0f}; // elbow back
        m_t = 0.0f;
        ++m_step;
        break;
    }
    case 75: {
        auto* v = m_animVillager.get(scene());
        ve::TwoBoneIK* ik = nullptr; // ours (the villager's FootIK has two more)
        if (v && v->body())
            for (const auto& c : v->body()->components())
                if (auto* k = dynamic_cast<ve::TwoBoneIK*>(c.get()); k && k->upperJoint == "UpperArm.L") ik = k;
        if (!v || !ik) {
            m_step = 47;
            break;
        }
        // Keep the target in front of the shoulder (the villager moves).
        ve::GameObject* shoulder = nullptr;
        std::function<ve::GameObject*(ve::GameObject&, const char*)> below = [&](ve::GameObject& o, const char* n) -> ve::GameObject* {
            if (o.name() == n) return &o;
            for (ve::Transform* c : o.transform().children())
                if (ve::GameObject* f = below(c->gameObject(), n)) return f;
            return nullptr;
        };
        shoulder = below(*v->body(), "UpperArm.L");
        if (shoulder) ik->target = shoulder->transform().position() + v->transform().rotation() * glm::vec3(0.1f, 0.05f, -0.35f);
        if (m_t > 3.0f) { // look at it (the next frame's screenshot)
            const glm::vec3 p = v->transform().position();
            const glm::vec3 eye = p + v->transform().rotation() * glm::vec3(1.2f, 0.0f, 3.2f);
            const glm::vec3 d = p - eye;
            localPlayer(scene())->setPose(eye, glm::degrees(ve::math::yawOf(d)), -8.0f);
        }
        if (m_t < 4.0f) break;
        ve::Animator* a = v->animator();
        const int layer = a->layerIndex("Wave");
        ve::GameObject* hand = below(*v->body(), "Hand.R");
        ve::GameObject* head = below(*v->body(), "Head");
        ve::GameObject* handL = below(*v->body(), "Hand.L");
        log("anim: layers " + std::to_string(a->layerCount()) + ", Wave layer weight " + std::to_string(a->layerWeight(layer)) + " playing '" +
            a->current(layer) + "', base layer '" + a->current() + "', right hand above the head: " +
            (hand && head && hand->transform().position().y > head->transform().position().y ? "yes" : "NO"));
        log("anim: footstep events while walking: " + std::to_string(v->steps() - m_animSteps) + (v->steps() > m_animSteps ? "" : " (NO)"));
        ik->solve(); // (measured right after solving: the next frame's animation resets the arm first)
        const float miss = handL ? glm::distance(handL->transform().position(), ik->target) : 99.0f;
        log("anim: two-bone IK hand to target: " + std::to_string(miss) + " m off" + (miss < 0.02f ? "" : " (NO)"));
        if (auto* feet = v->body()->getComponent<ve::FootIK>()) {
            const glm::vec3 l = feet->footTarget(0), r = feet->footTarget(1);
            log("anim: foot IK targets " + std::to_string(l.y - groundAt({l.x, l.z}).y) + " / " + std::to_string(r.y - groundAt({r.x, r.z}).y) +
                " m above the ground, hips offset " + std::to_string(feet->hipsOffset()) + " m");
        }
        shot("8a-villager");
        v->body()->removeComponent(ik);
        m_t = 0.0f;
        m_stepFrame = m_frames;
        m_step = 76; // joints
        break;
    }
    case 76: { // a yard of joints: pendulum, motor wheel, slider, rope, a weld that breaks
        const glm::vec2 yard = layout::kCamp + glm::vec2(-24.0f, 24.0f);
        const float g = groundAt(yard).y;
        auto body = [&](const char* name, glm::vec3 at, const ve::Collider& c, float mass, glm::vec4 color, glm::vec3 size,
                        ve::PrimitiveRenderer::Shape shape = ve::PrimitiveRenderer::Shape::Cube) -> ve::GameObject& {
            auto& o = scene().spawn(name);
            o.transform().setPosition(at);
            auto& pr = o.addComponent<ve::PrimitiveRenderer>();
            pr.shape = shape;
            pr.size = size;
            pr.color = color;
            auto& rb = o.addComponent<ve::Rigidbody>(c, ve::BodyType::Dynamic);
            rb.mass = mass;
            m_jointObjects.push_back(&o);
            return o;
        };
        // Pendulum: a weight 1.2 m under a hinge (Z axis), limited to +-60 degrees, pushed.
        auto& bob = body("Pendulum", {yard.x, g + 2.0f, yard.y}, ve::Collider::box(glm::vec3(0.15f)), 4.0f, {0.9f, 0.5f, 0.2f, 1}, glm::vec3(0.3f));
        auto& pend = bob.addComponent<ve::HingeJoint>();
        pend.anchor = {0.0f, 1.2f, 0.0f};
        pend.axis = {0.0f, 0.0f, 1.0f};
        pend.useLimits = true;
        pend.limitMin = -60.0f;
        pend.limitMax = 60.0f;
        bob.getComponent<ve::Rigidbody>()->setLinearVelocity({7.0f, 0.0f, 0.0f});
        // A wheel (a bar) turned by a velocity motor at 180 deg/s.
        auto& wheel = body("Motor wheel", {yard.x + 3.0f, g + 1.6f, yard.y}, ve::Collider::box({0.6f, 0.06f, 0.06f}), 3.0f, {0.3f, 0.6f, 1.0f, 1},
                           {1.2f, 0.12f, 0.12f});
        auto& motor = wheel.addComponent<ve::HingeJoint>();
        motor.axis = {0.0f, 0.0f, 1.0f};
        motor.motor = ve::Joint::Motor::Velocity;
        motor.motorTarget = 180.0f;
        // A slider driven to +1.5 m along X (limits +-2 m).
        auto& sled = body("Slider", {yard.x + 6.0f, g + 1.0f, yard.y}, ve::Collider::box(glm::vec3(0.2f)), 5.0f, {0.4f, 0.9f, 0.4f, 1}, glm::vec3(0.4f));
        auto& slide = sled.addComponent<ve::SliderJoint>();
        slide.useLimits = true;
        slide.limitMin = -2.0f;
        slide.limitMax = 2.0f;
        slide.motor = ve::Joint::Motor::Position;
        slide.motorTarget = 1.5f;
        slide.motorMaxForce = 2000.0f;
        // A rope: four links of 0.5 m from a point in the air.
        const glm::vec3 top{yard.x + 10.0f, g + 3.0f, yard.y};
        ve::GameObject* prev = nullptr;
        for (int i = 0; i < 4; ++i) {
            auto& link = body("Rope link", top + glm::vec3(0.35f * float(i + 1), -0.35f * float(i + 1), 0.0f), ve::Collider::sphere(0.08f), 0.5f,
                              {0.8f, 0.7f, 0.5f, 1}, glm::vec3(0.16f), ve::PrimitiveRenderer::Shape::Sphere);
            auto& rope = link.addComponent<ve::DistanceJoint>();
            if (prev) rope.connect(prev);
            else rope.connectedAnchor = top; // (the world)
            rope.minDistance = 0.0f;
            rope.maxDistance = 0.5f;
            prev = &link;
        }
        m_ropeEnd = prev;
        m_ropeTop = top;
        // A plank welded to the world, breaking under 1500 N; a 120 kg block lands on its end.
        auto& plank = body("Plank", {yard.x + 14.0f, g + 1.0f, yard.y}, ve::Collider::box({0.8f, 0.05f, 0.2f}), 4.0f, {0.6f, 0.45f, 0.3f, 1},
                           {1.6f, 0.1f, 0.4f});
        auto& weld = plank.addComponent<ve::FixedJoint>();
        weld.anchor = {-0.8f, 0.0f, 0.0f};
        weld.breakForce = 1500.0f;
        weld.breakTorque = 1500.0f;
        weld.onBreak = [this](ve::Joint& j) { m_weldBroke = j.lastForce() > j.lastTorque() ? j.lastForce() : j.lastTorque(); };
        body("Heavy block", {yard.x + 14.6f, g + 2.6f, yard.y}, ve::Collider::box(glm::vec3(0.25f)), 120.0f, {0.4f, 0.4f, 0.45f, 1}, glm::vec3(0.5f));
        ve::debug::setChannel("Physics joints", true);
        // Look at the yard.
        localPlayer(scene())->setPose({yard.x + 7.0f, g + 0.5f, yard.y + 9.0f}, 0.0f, -8.0f);
        m_t = 0.0f;
        ++m_step;
        break;
    }
    case 77: {
        if (m_t < 3.0f) break;
        auto find = [&](const char* name) -> ve::GameObject* {
            for (const auto& ref : m_jointObjects)
                if (ve::GameObject* o = ref.get(scene()); o && o->name() == name) return o;
            return nullptr;
        };
        const auto* pend = find("Pendulum") ? find("Pendulum")->getComponent<ve::HingeJoint>() : nullptr;
        const auto* wheel = find("Motor wheel") ? find("Motor wheel")->getComponent<ve::Rigidbody>() : nullptr;
        const auto* slide = find("Slider") ? find("Slider")->getComponent<ve::SliderJoint>() : nullptr;
        const auto* weld = find("Plank") ? find("Plank")->getComponent<ve::FixedJoint>() : nullptr;
        const ve::GameObject* end = m_ropeEnd.get(scene());
        if (!m_jointsLogged) {
        m_jointsLogged = true;
        log("joints: pendulum " + std::string(pend && pend->attached() ? "attached" : "NOT attached") + ", angle " +
            std::to_string(pend ? pend->angle() : 0.0f) + " deg (within +-60: " + (pend && std::abs(pend->angle()) <= 60.5f ? "yes" : "NO") + ")");
        log("joints: motor wheel spins at " + std::to_string(wheel ? glm::degrees(glm::length(wheel->angularVelocity())) : 0.0f) +
            " deg/s (asked 180)");
        log("joints: slider at " + std::to_string(slide ? slide->position() : 0.0f) + " m (asked 1.5)");
        const float ropeLen = end ? glm::distance(end->transform().position(), m_ropeTop) : 0.0f;
        log("joints: rope end hangs " + std::to_string(ropeLen) + " m from its anchor (4 links of 0.5 m: " +
            (ropeLen <= 2.05f && ropeLen > 1.6f ? "yes" : "NO") + ")");
        log(std::string("joints: the weld broke under the block: ") + (weld && weld->broken() ? "yes" : "NO") + " (" +
            std::to_string(m_weldBroke) + " at the break)");
        }
        shot("8b-joints");
        if (m_t < 3.5f) break;
        for (const auto& ref : m_jointObjects)
            if (ve::GameObject* o = ref.get(scene())) o->destroy();
        m_jointObjects.clear();
        ve::debug::setChannel("Physics joints", false);
        m_t = 0.0f;
        m_stepFrame = m_frames;
        m_step = 78; // look-at / aim IK
        break;
    }
    case 78:
    case 79: { // a villager looks at one point and points at another
        auto* v = m_animVillager.get(scene());
        if (!v || !v->body() || !v->looker() || !v->pointer()) {
            log("aim: no villager with IK");
            m_step = 47;
            break;
        }
        v->agent().stop();
        const glm::quat facing = v->body()->transform().rotation(); // the model's +Z = its front
        const glm::vec3 base = v->transform().position();
        m_lookAt = base + glm::vec3(0.0f, 1.9f, 0.0f) + facing * glm::vec3(1.0f, 0.2f, 2.0f);
        m_pointAt = base + glm::vec3(0.0f, 2.2f, 0.0f) + facing * glm::vec3(-2.5f, 0.0f, 1.0f);
        if (m_step == 78) {
            v->pointAt(m_pointAt, 10.0f);
            // Look from in front of it.
            const glm::vec3 eye = base + facing * glm::vec3(0.6f, 0.0f, 3.6f);
            const glm::vec3 d = base - eye;
            localPlayer(scene())->setPose(eye + glm::vec3(0.0f, -0.2f, 0.0f), glm::degrees(ve::math::yawOf(d)), -6.0f);
            m_t = 0.0f;
            ++m_step;
            break;
        }
        v->looker()->setTarget(m_lookAt); // (every frame: the villager's own update aims it at the player otherwise)
        v->pointer()->setTarget(m_pointAt);
        if (m_t < 2.0f) break;
        ve::debug::sphere(m_lookAt, 0.12f, ve::debug::kCyan);
        ve::debug::sphere(m_pointAt, 0.15f, ve::debug::kYellow);
        ve::debug::ray(v->looker()->aimOrigin(), v->looker()->aimDirection() * 2.0f, ve::debug::kCyan);
        ve::debug::ray(v->pointer()->aimOrigin(), v->pointer()->aimDirection() * 2.5f, ve::debug::kYellow);
        shot("8c-look-point");
        if (m_t < 2.3f) break;
        log("aim: look-at blend " + std::to_string(v->looker()->blend()) + ", " + std::to_string(v->looker()->lastError()) + " deg off its target" +
            (v->looker()->lastError() < 2.0f ? "" : " (NO)"));
        log("aim: pointing arm blend " + std::to_string(v->pointer()->blend()) + ", " + std::to_string(v->pointer()->lastError()) +
            " deg off its target" + (v->pointer()->lastError() < 3.0f ? "" : " (NO)"));
        v->looker()->clearTarget();
        v->pointer()->clearTarget();
        m_t = 0.0f;
        m_stepFrame = m_frames;
        m_step = 47; // the UI steps
        break;
    }
    default: break;
    }
}

// The game's UI: inventory, build menu, pause, title (MainMenu.hpp). The
// pause stops the clock, so these steps count frames.
void AutoTest::uiSteps(const std::function<void(const std::string&)>& log)
{
    const uint32_t frames = m_frames - m_stepFrame;
    auto next = [&] {
        m_stepFrame = m_frames;
        m_t = 0.0f;
        ++m_step;
    };
    auto* inv = scene().findFirst<InventoryScreen>();
    auto* builder = scene().findFirst<Builder>();
    auto* menu = scene().findFirst<MainMenu>();
    Inventory& items = gameState().inventory;
    switch (m_step) {
    case 47: {
        items.add("berries", 7);
        items.add("sunstone", 2);
        if (inv) inv->setOpen(true);
        log(std::string("ui: inventory open: ") + (inv && inv->open() ? "yes" : "NO") + ", " + std::to_string(items.count("wood")) +
            " wood, " + std::to_string(items.count("berries")) + " berries");
        next();
        break;
    }
    case 48:
        if (frames < 20) break;
        shot("8-inventory");
        if (frames < 24) break;
        if (inv) {
            const Inventory::Stack from = items.slots[0];
            inv->clickSlot(0);
            inv->clickSlot(10); // (an empty slot)
            log("ui: moved slot 0 (" + from.item + " x" + std::to_string(from.count) + ") to slot 10: " +
                (items.slots[10].item == from.item && items.slots[10].count == from.count && items.slots[0].item.empty() ? "yes" : "NO"));
            inv->setOpen(false);
        }
        next();
        break;
    case 49:
        if (builder) {
            builder->openMenu();
            builder->showCategory("Walls");
        }
        log(std::string("ui: build menu open: ") + (builder && builder->menuOpen() ? "yes" : "NO"));
        next();
        break;
    case 50: {
        if (frames < 20) break;
        shot("9-build-menu");
        if (frames < 24 || !builder) break;
        // Pick a card as the player would: the piece is selected, the menu closes, building starts.
        int want = -1;
        for (size_t i = 0; i < builder->catalogue().size(); ++i)
            if (builder->catalogue()[i].category == "Walls" && want < 0) want = int(i);
        bool clicked = false;
        if (want >= 0)
            if (ve::ui::Element* card = builder->menu().root().find("piece:" + builder->catalogue()[size_t(want)].name)) {
                card->click();
                clicked = true;
            }
        log("ui: clicked the card for " + (want >= 0 ? builder->catalogue()[size_t(want)].name : std::string("?")) + ": " +
            (clicked && builder->selection() == want && !builder->menuOpen() && builder->active() ? "selected, building" : "NO"));
        builder->setActive(false);
        next();
        break;
    }
    case 51:
        if (menu) menu->pause();
        log(std::string("ui: paused: ") + (menu && menu->screen() == MainMenu::Screen::Paused && time().timeScale == 0.0f ? "yes" : "NO"));
        next();
        break;
    case 52:
        if (frames < 20) break;
        shot("10-pause");
        if (frames < 24 || !menu) break;
        menu->showPanel("settings");
        next();
        break;
    case 53:
        if (frames < 20) break;
        shot("11-settings");
        if (frames < 24 || !menu) break;
        // Lists, text fields: a long Load list (scrolls), typing a save name, the multiplayer panel.
        {
            const ve::Json world = ve::save::capture(scene());
            for (int i = 1; i <= 14; ++i) ve::save::write("ui-test-" + std::string(i < 10 ? "0" : "") + std::to_string(i), world);
        }
        menu->showPanel("load");
        m_step = 60;
        m_stepFrame = m_frames;
        break;
    case 60: {
        if (frames < 20) break;
        auto* view = dynamic_cast<ve::ui::ScrollView*>(menu->panels().root().find("slots"));
        if (view && !view->content().children().empty()) view->scrollTo(*view->content().children().back()); // (as keyboard focus would)
        if (frames < 24) break;
        if (frames == 24)
            log(std::string("ui: the load list scrolls: ") + (view && view->overflows() ? "yes" : "NO") + " (" +
            std::to_string(view ? view->content().children().size() : 0) + " saves, scrolled to " + std::to_string(view ? view->scroll : 0.0f) +
                " of " + std::to_string(view ? view->maxScroll() : 0.0f) + ")");
        shot("11a-load-list");
        if (frames < 28) break;
        menu->showPanel("save"); // (focuses the name field)
        next();
        m_step = 61;
        break;
    }
    case 61: {
        auto* field = dynamic_cast<ve::ui::TextInput*>(menu->panels().root().find("name"));
        if (frames == 2 && field) { // type over the suggested name, with a typo fixed
            field->setText("");
            input().injectText("camp-12");
            input().injectKeyTyped(ve::Key::Backspace);
        }
        if (frames < 20) break;
        shot("11b-save");
        if (frames < 24 || !field) break;
        if (frames != 24) break;
        log("ui: typed into the save name: '" + field->text + "' " + (field->text == "camp-1" ? "(right)" : "(NO)") +
            ", being edited: " + (ve::ui::System::get().editing() == field ? "yes" : "NO") + ", gameplay keys blocked: " +
            (input().keyDown(ve::Key::W) || ve::ui::System::get().editing() ? "yes" : "NO"));
        if (ve::ui::Element* save = menu->panels().root().find("save")) save->click();
        log(std::string("ui: saved as 'camp-1': ") + (ve::save::read("camp-1") ? "yes" : "NO") + ", back in the game: " +
            (menu->screen() == MainMenu::Screen::Playing ? "yes" : "NO"));
        for (int i = 1; i <= 14; ++i) ve::save::remove("ui-test-" + std::string(i < 10 ? "0" : "") + std::to_string(i));
        ve::save::remove("camp-1");
        menu->pause();
        menu->showPanel("multiplayer");
        m_step = 62;
        m_stepFrame = m_frames;
        break;
    }
    case 62: {
        if (frames < 20) break;
        shot("11c-multiplayer");
        if (frames < 24) break;
        menu->showPanel("");
        menu->toTitle(); // Main menu
        log(std::string("ui: back on the title: ") + (menu->screen() == MainMenu::Screen::Title && time().timeScale == 1.0f ? "yes" : "NO"));
        m_step = 54;
        m_stepFrame = m_frames;
        m_t = 0.0f;
        break;
    }
    case 54:
        if (frames < 90) break; // (the camera and the light settle)
        shot("12-title");
        if (frames < 94 || !menu) break;
        // New game: the world is rebuilt as it was built; this test resumes in the new one.
        s_resume = 55;
        menu->newGame();
        log("ui: new game (the level reloads)");
        next();
        break;
    case 55: {
        if (m_t < 2.0f) break;
        int pieces = 0;
        for (const auto& o : scene().objects())
            if (!o->isDestroyed() && dynamic_cast<BuildPiece*>(o.get())) ++pieces;
        log(std::string("ui: after the new game: playing ") + (menu && menu->screen() == MainMenu::Screen::Playing ? "yes" : "NO") +
            ", inventory empty " + (items.count("berries") == 0 && items.count("wood") == 0 ? "yes" : "NO") + ", built pieces " +
            std::to_string(pieces));
        engine().quit();
        next();
        break;
    }
    default: break;
    }
}

void AutoTest::faceGround(const glm::vec3& target, float distance)
{
    // From the east, looking west and down at the point (the Interactor's view cone).
    Player* player = localPlayer(scene());
    const glm::vec3 feet = groundAt({target.x + distance, target.z});
    const glm::vec3 d = target - (feet + glm::vec3(0.0f, player->eyeHeight, 0.0f));
    player->setPose(feet, glm::degrees(ve::math::yawOf(d)), glm::degrees(ve::math::pitchOf(d)));
}

// Pickups: stones and sticks held as data in a ve::ProxySet, promoted to
// Pickup objects near the player (Pickups.hpp).
void AutoTest::pickupSteps(const std::function<void(const std::string&)>& log)
{
    ve::ProxySet* set = findPickups(scene());
    if (!set) {
        log("pickups: NO set");
        engine().quit();
        m_step = 100;
        return;
    }
    auto yes = [](bool b) { return std::string(b ? "yes" : "NO"); };
    const glm::vec3 start = groundAt(layout::kCamp + glm::vec2(9.0f, 5.0f));
    const int stoneType = set->typeIndex("stone");
    switch (m_step) {
    case 63: {
        if (m_t < 0.3f) break;
        // Only the few near the player are objects; every promoted one has its link.
        int links = 0;
        scene().forEach<ve::ProxyLink>([&](ve::ProxyLink&) { ++links; });
        log("pickups: " + std::to_string(set->size()) + " entries, " + std::to_string(set->promotedCount()) + " promoted (bounded: " +
            yes(set->promotedCount() <= 40 && set->size() > 1000) + "), links " + std::to_string(links) + " (match: " +
            yes(links == int(set->promotedCount())) + ")");
        const ve::ProxyEntry* stone = set->nearest(start, 40.0f, stoneType);
        if (!stone) {
            log("pickups: NO stone near the camp");
            m_step = 69;
            break;
        }
        s_expect.pickupGone = stone->id;
        faceGround(stone->position, 1.3f);
        m_t = 0.0f;
        ++m_step;
        break;
    }
    case 64: {
        if (m_t < 0.5f) break;
        auto* pickup = dynamic_cast<Pickup*>(set->promoted(s_expect.pickupGone));
        auto* who = localPlayer(scene())->getComponent<ve::Interactor>();
        const int before = gameState().inventory.count("stone");
        const std::string prompt = who ? who->prompt() : std::string();
        const bool used = who && who->interact();
        log("pickups: the stone is a Pickup object: " + yes(pickup != nullptr) + ", prompt '" + prompt + "', used " + yes(used) +
            ", stones " + std::to_string(before) + " -> " + std::to_string(gameState().inventory.count("stone")) + " (+1: " +
            yes(gameState().inventory.count("stone") == before + 1) + ")");
        m_t = 0.0f;
        ++m_step;
        break;
    }
    case 65: {
        if (m_t < 0.3f) break;
        log("pickups: the picked entry is gone: " + yes(!set->find(s_expect.pickupGone)) + " (consumed " +
            std::to_string(set->stats().consumed) + ")");
        // Another one, to change while it's an object and walk away from.
        const glm::vec3 near = groundAt(layout::kCamp + glm::vec2(30.0f, -30.0f));
        const ve::ProxyEntry* kept = set->nearest(near, 60.0f);
        if (!kept) {
            log("pickups: NO second entry");
            m_step = 69;
            break;
        }
        s_expect.pickupKept = kept->id;
        faceGround(kept->position, 1.5f);
        m_t = 0.0f;
        ++m_step;
        break;
    }
    case 66: {
        if (m_t < 0.5f) break;
        ve::GameObject* obj = set->promoted(s_expect.pickupKept);
        auto* link = obj ? obj->getComponent<ve::ProxyLink>() : nullptr;
        if (link) {
            link->setState(7);
            link->setData({{"mark", "autotest"}});
            obj->addComponent<ProbeState>().value = 42;
        }
        log("pickups: second entry promoted with a link: " + yes(link != nullptr) + "; state 7, a payload and a Saveable component set");
        localPlayer(scene())->teleport(groundAt({1200.0f, -300.0f}), 0.0f); // far away: it goes back to data
        m_t = 0.0f;
        ++m_step;
        break;
    }
    case 67: {
        if (m_t < 1.0f) break;
        const ve::ProxyEntry* e = set->find(s_expect.pickupKept);
        const bool captured = e && e->objectState.is_object() && e->objectState.value("components", ve::Json::object()).value("AutoTestProbeState", ve::Json::object()).value("value", 0) == 42;
        log("pickups: far away: demoted " + yes(e && !set->promoted(e->id)) + ", " + std::to_string(set->promotedCount()) +
            " promoted, state kept " + yes(e && e->state == 7) + ", payload kept " + yes(e && e->data.value("mark", "") == "autotest") +
            ", Saveable state captured " + yes(captured));
        if (e) faceGround(e->position, 1.5f);
        m_t = 0.0f;
        ++m_step;
        break;
    }
    case 68: {
        if (m_t < 1.0f) break;
        ve::GameObject* obj = set->promoted(s_expect.pickupKept);
        auto* link = obj ? obj->getComponent<ve::ProxyLink>() : nullptr;
        auto* probe = obj ? obj->getComponent<ProbeState>() : nullptr;
        log("pickups: back again: promoted " + yes(obj != nullptr) + ", link state " + std::to_string(link ? link->state() : 0) + " (7: " +
            yes(link && link->state() == 7) + "), Saveable state restored " + yes(probe && probe->value == 42));
        // Save, load (the level rebuilt): the picked stone stays picked, the marked one marked.
        s_expect.pickupStones = gameState().inventory.count("stone");
        s_expect.pickupEntries = set->size();
        log("pickups: saved " + yes(saveProgress(scene(), "pickuptest")));
        s_resume = 69;
        log(std::string("pickups: loading the save: ") + (loadProgress("pickuptest") ? "started" : "NO SAVE"));
        ++m_step;
        break;
    }
    case 69: {
        if (m_t < 3.0f) break;
        if (!m_pickupLogged) {
            m_pickupLogged = true;
            const ve::ProxyEntry* kept = set->find(s_expect.pickupKept);
            log("pickups: after loading: picked stone still gone " + yes(s_expect.pickupGone && !set->find(s_expect.pickupGone)) +
                ", marked entry state 7 " + yes(kept && kept->state == 7) + ", payload " +
                yes(kept && kept->data.value("mark", "") == "autotest") + ", entries " + std::to_string(set->size()) + " (" +
                std::to_string(s_expect.pickupEntries) + "), stones in the inventory " + std::to_string(gameState().inventory.count("stone")) +
                " (" + std::to_string(s_expect.pickupStones) + ")");
            ve::save::remove("pickuptest");
            // The screenshot: stones and sticks in the grass by the camp.
            if (const ve::ProxyEntry* e = set->nearest(start, 40.0f)) {
                const glm::vec3 eye = groundAt({e->position.x + 3.5f, e->position.z + 2.0f}) + glm::vec3(0.0f, 0.4f, 0.0f);
                const glm::vec3 d = e->position - (eye + glm::vec3(0.0f, localPlayer(scene())->eyeHeight, 0.0f));
                localPlayer(scene())->setPose(eye, glm::degrees(ve::math::yawOf(d)), glm::degrees(ve::math::pitchOf(d)));
            }
        }
        if (m_t < 4.5f) break;
        shot("7a-pickups");
        if (m_t < 5.0f) break;
        m_t = 0.0f;
        m_step = 70; // physics layers / surfaces, then the UI
        break;
    }
    default: break;
    }
}

} // namespace savanna
