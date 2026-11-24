#include "ve/render/SceneGatherer.hpp"

#include "ve/render/Decals.hpp"
#include "ve/render/Mesh.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/render/ShadowSetup.hpp"
#include "ve/render/Skinning.hpp"
#include "ve/render/StaticBatcher.hpp"
#include "ve/core/Log.hpp"
#include "ve/scene/Components.hpp"
#include "ve/scene/Decal.hpp"
#include "ve/scene/Scene.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cfloat>

namespace ve {

uint32_t SceneGatherer::gather(Scene& scene, const Inputs& in, FrameData& frame)
{
    m_sceneTop = -FLT_MAX; // highest drawn geometry (shadow caster reach, ShadowSetup::buildCascades)
    frame.items.clear();
    frame.opaque.clear();
    frame.transparent.clear();
    frame.moving.clear();
    frame.shadowViews.clear();
    frame.stats = {};

    setupCamera(scene, in, frame.camera);
    gatherDrawItems(scene, in, frame);
    const uint32_t points = gatherLights(scene, in, frame);
    gatherDecals(scene, in, frame);
    return points;
}

void SceneGatherer::setupCamera(Scene& scene, const Inputs& in, CameraData& cam) const
{
    const VkExtent2D ext = in.extent;
    cam.aspect = static_cast<float>(ext.width) / static_cast<float>(std::max(1u, ext.height));

    glm::vec3 pos(0.0f, 2.0f, 5.0f);
    glm::quat rot(1.0f, 0.0f, 0.0f, 0.0f);
    if (Camera* c = scene.mainCamera()) {
        pos = c->transform().position();
        rot = c->transform().rotation();
        cam.fovY = glm::radians(c->fovDegrees);
        cam.nearPlane = c->nearPlane;
        cam.farPlane = c->farPlane;
    }
    cam.position = pos;
    cam.forward = rot * glm::vec3(0, 0, -1);
    cam.up = rot * glm::vec3(0, 1, 0);
    cam.right = rot * glm::vec3(1, 0, 0);
    // View = inverse of the camera's (unscaled) world transform.
    cam.view = glm::mat4_cast(glm::conjugate(rot)) * glm::translate(glm::mat4(1.0f), -pos);
    // Reversed-Z: pass far as "near" and near as "far" so depth is 1 at the
    // near plane and 0 at the far plane. Then flip Y for Vulkan's clip space.
    cam.proj = glm::perspective(cam.fovY, cam.aspect, cam.farPlane, cam.nearPlane);
    cam.proj[1][1] *= -1.0f;
    cam.viewProjNoJitter = cam.proj * cam.view;
    // Sub-pixel jitter (TAA, photo capture): shift the image by a fraction of a pixel.
    cam.proj[2][0] += in.jitter.x * 2.0f / float(std::max(1u, ext.width));
    cam.proj[2][1] += in.jitter.y * 2.0f / float(std::max(1u, ext.height));
    cam.viewProj = cam.proj * cam.view;
    cam.frustum = Frustum::fromMatrix(cam.viewProj);
}

void SceneGatherer::gatherDrawItems(Scene& scene, const Inputs& in, FrameData& frame)
{
    const CameraData& cam = frame.camera;

    // Skinned meshes: joint matrices for this frame's skinning pass (and their bounds).
    // Their vertices are rewritten on the GPU every frame, so their draw items
    // are flagged `deforming` (the point-shadow face cache must not trust them).
    m_deforming.clear();
    for (const auto& gather : in.skinning->gatherers()) gather(scene, *in.skinning, m_deforming);
    std::sort(m_deforming.begin(), m_deforming.end());
    // Static renderers: apply what changed since last frame (no scan).
    m_staticsActive = updateStatics(scene, in);
    // Last frame's world matrices (motion vectors for moving objects). One
    // persistent map, stamped with the frame that last saw each renderer:
    // no per-frame rebuild, only new renderers allocate.
    const uint64_t stamp = ++m_prevWorldStamp;
    size_t seen = 0;
    scene.forEach<MeshRenderer>([&](MeshRenderer& mr) {
        if (mr.staticEntry() != MeshRenderer::kNoStaticEntry) return; // batched: its instance group draws it
        if (!mr.isActive() || !mr.visible || !mr.mesh) return;
        const glm::mat4& world = mr.transform().worldMatrix();
        const auto prev = m_prevWorlds.try_emplace(&mr, PrevWorld{world, stamp}).first; // new: prev = world
        const glm::mat4 prevWorld = prev->second.world;
        prev->second = {world, stamp};
        ++seen;
        const bool deforming = !m_deforming.empty() && std::binary_search(m_deforming.begin(), m_deforming.end(), &mr);
        for (const Submesh& sm : mr.mesh->submeshes()) {
            Material* mat = mr.material(sm.materialSlot);
            if (!mat) mat = in.fallbackMaterial;
            DrawItem item;
            item.mesh = mr.mesh.get();
            item.submesh = &sm;
            item.material = mat;
            item.world = world;
            item.prevWorld = prevWorld;
            item.bounds = sm.bounds.transformed(world);
            item.distance = glm::length(item.bounds.center() - cam.position);
            item.castShadows = mr.castShadows;
            item.deforming = deforming;
            item.owner = &mr.gameObject();
            m_sceneTop = std::max(m_sceneTop, item.bounds.max.y);
            frame.items.push_back(item);
        }
    });
    // Forget renderers not drawn this frame (destroyed or hidden): a new
    // renderer at a recycled address must not inherit a stale matrix.
    if (m_prevWorlds.size() != seen) std::erase_if(m_prevWorlds, [&](const auto& kv) { return kv.second.stamp != stamp; });
    frame.stats.totalObjects = static_cast<uint32_t>(frame.items.size());
    if (m_staticsActive) {
        frame.stats.staticObjects = in.statics->submeshCount();
        frame.stats.totalObjects += frame.stats.staticObjects;
        m_sceneTop = std::max(m_sceneTop, in.statics->top());
    }

    for (uint32_t i = 0; i < frame.items.size(); ++i) {
        const DrawItem& it = frame.items[i];
        if (!cam.frustum.intersects(it.bounds)) continue;
        const bool transparent = it.material->shader().transparent();
        (transparent ? frame.transparent : frame.opaque).push_back(i);
        if (!transparent && it.prevWorld != it.world) frame.moving.push_back(i);
    }
    frame.stats.visibleObjects = static_cast<uint32_t>(frame.opaque.size() + frame.transparent.size());

    // Opaque: group by pipeline (fewer state changes) then front-to-back
    // (early depth rejection). Transparent: back-to-front for correct blending.
    auto& items = frame.items;
    std::sort(frame.opaque.begin(), frame.opaque.end(), [&](uint32_t a, uint32_t b) {
        const Shader* sa = &items[a].material->shader();
        const Shader* sb = &items[b].material->shader();
        const int qa = int(sa->desc().queue), qb = int(sb->desc().queue);
        if (qa != qb) return qa < qb;
        if (sa != sb) return sa < sb;
        return items[a].distance < items[b].distance;
    });
    std::sort(frame.transparent.begin(), frame.transparent.end(),
              [&](uint32_t a, uint32_t b) { return items[a].distance > items[b].distance; });
}

// -----------------------------------------------------------------------------
// Static batching (StaticBatcher)
// -----------------------------------------------------------------------------

bool SceneGatherer::updateStatics(Scene& scene, const Inputs& in)
{
    StaticBatcher& batcher = *in.statics;
    ++m_staticStamp;
    // The batched scene was destroyed: its entries went with its renderers.
    if (m_staticScene && m_staticSceneAlive.expired()) {
        batcher.clear();
        m_staticScene = nullptr;
    }
    Scene::StaticChanges& changes = scene.staticChanges();
    if (!in.settings->staticBatching) {
        // Turned off: every renderer back on the per-item path, exactly as before.
        if (m_staticScene) {
            m_staticScene->resetStaticEntries();
            batcher.clear();
            m_staticScene = nullptr;
        }
        return false;
    }
    if (!m_staticScene) {
        if (changes.known == 0 && changes.dirty.empty()) return false; // nothing static here (yet)
        // Start batching this scene: one scan queues every static renderer
        // (after this, only changes are reported).
        m_staticScene = &scene;
        m_staticSceneAlive = changes.alive;
        changes.released.clear();
        scene.queueAllStatic();
    }
    // Another scene is being rendered (e.g. a loading screen's): its
    // renderers have no entries, so they all take the per-item path.
    batcher.setHidden(&scene != m_staticScene);
    if (&scene != m_staticScene) return false;

    for (uint32_t entry : changes.released) batcher.remove(entry);
    changes.released.clear();
    // Skinned meshes are rewritten on the GPU every frame: never batched.
    for (const MeshRenderer* d : m_deforming) {
        auto* mr = const_cast<MeshRenderer*>(d); // (the skinning gatherers hand out const pointers)
        if (mr->staticEntry() == MeshRenderer::kNoStaticEntry) continue;
        batcher.remove(mr->staticEntry());
        scene.setStaticEntry(*mr, MeshRenderer::kNoStaticEntry);
    }
    scene.takeStaticDirty(m_staticDirty);
    for (MeshRenderer* mr : m_staticDirty) {
        if (mr) updateStatic(scene, *mr, in);
    }
    m_staticDirty.clear();
    return true;
}

void SceneGatherer::updateStatic(Scene& scene, MeshRenderer& mr, const Inputs& in)
{
    StaticBatcher& batcher = *in.statics;
    const uint32_t entry = mr.staticEntry();
    bool eligible = mr.isStatic && mr.isActive() && mr.visible && mr.mesh &&
                    !std::binary_search(m_deforming.begin(), m_deforming.end(), &mr);
    // The materials per submesh slot, resolved like MeshRenderer::material().
    std::vector<std::shared_ptr<Material>> materials;
    if (eligible) {
        uint32_t slots = 0;
        for (const Submesh& sm : mr.mesh->submeshes()) slots = std::max(slots, sm.materialSlot + 1);
        materials.resize(slots);
        if (!mr.materials.empty()) {
            for (uint32_t i = 0; i < slots; ++i) materials[i] = mr.materials[i < mr.materials.size() ? i : 0];
        }
        // Instance groups can't sort transparent draws back to front.
        for (const Submesh& sm : mr.mesh->submeshes()) {
            const Material* m = materials[sm.materialSlot].get();
            if ((m ? m : in.fallbackMaterial)->shader().transparent()) eligible = false;
        }
    }
    if (!eligible) {
        if (entry != MeshRenderer::kNoStaticEntry) {
            batcher.remove(entry);
            scene.setStaticEntry(mr, MeshRenderer::kNoStaticEntry);
        }
        return;
    }
    const glm::mat4& world = mr.transform().worldMatrix(); // (cleans it: the next move reports again)
    if (entry != MeshRenderer::kNoStaticEntry) {
        if (batcher.matches(entry, &mr, mr.mesh.get(), materials, mr.castShadows)) {
            batcher.setTransform(entry, world);
            return;
        }
        batcher.remove(entry); // another batch now
        scene.setStaticEntry(mr, MeshRenderer::kNoStaticEntry);
    }
    StaticBatcher::Desc desc;
    desc.mesh = mr.mesh;
    desc.materials = std::move(materials);
    desc.castShadows = mr.castShadows;
    desc.world = world;
    desc.owner = &mr.gameObject();
    desc.source = &mr;
    try {
        scene.setStaticEntry(mr, batcher.add(desc));
    } catch (const std::exception& e) {
        // (Drawn the normal way instead.)
        if (!m_staticFullWarned) log::warn("Static batching: {}", e.what());
        m_staticFullWarned = true;
    }
}

uint32_t SceneGatherer::gatherLights(Scene& scene, const Inputs& in, FrameData& frame)
{
    const RendererSettings& settings = *in.settings;
    const ShadowSetup& shadows = *in.shadows;
    LightBuffer& lights = *frame.lights;
    const CameraData& cam = frame.camera;

    // --- Directional lights ---------------------------------------------------
    std::vector<DirectionalLight*>& dirs = m_dirScratch;
    dirs.clear();
    scene.forEach<DirectionalLight>([&](DirectionalLight& l) {
        if (l.isActive() && l.intensity > 0.0f && dirs.size() < kMaxDirectionalLights) dirs.push_back(&l);
    });
    // Brightest lights get shadow maps first.
    std::sort(dirs.begin(), dirs.end(), [](auto* a, auto* b) { return a->intensity > b->intensity; });
    uint32_t dirShadowSlots = 0;
    for (uint32_t i = 0; i < dirs.size(); ++i) {
        const DirectionalLight& l = *dirs[i];
        GpuDirectionalLight& g = lights.directional[i];
        const glm::vec3 dir = glm::normalize(l.transform().forward());
        g.direction = glm::vec4(dir, 0.0f);
        g.color = glm::vec4(l.color * l.intensity, 0.0f);
        g.shadow = glm::ivec4(-1, 0, 0, 0);
        g.cascadeSplits = glm::vec4(0.0f);
        g.cascadeTexel = glm::vec4(0.0f);
        if (l.castShadows && dirShadowSlots < settings.maxShadowedDirectionalLights) {
            shadows.buildCascades(frame, m_sceneTop, shadows.shadowDirection(dir), i, dirShadowSlots++, g);
        }
    }

    // --- Point lights ---------------------------------------------------------
    std::vector<PointCandidate>& points = m_pointScratch;
    points.clear();
    scene.forEach<PointLight>([&](PointLight& l) {
        if (!l.isActive() || l.intensity <= 0.0f) return;
        const glm::vec3 p = l.transform().position();
        if (!cam.frustum.intersectsSphere(p, l.range)) return; // can't affect anything we see
        points.push_back({&l, glm::length(p - cam.position)});
    });
    std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.distance < b.distance; });
    if (points.size() > kMaxPointLights) points.resize(kMaxPointLights);

    uint32_t cubeSlots = 0;
    for (uint32_t i = 0; i < points.size(); ++i) {
        const PointLight& l = *points[i].light;
        GpuPointLight& g = lights.point[i];
        const glm::vec3 pos = l.transform().position();
        g.positionRange = glm::vec4(pos, l.range);
        g.color = glm::vec4(l.color * l.intensity, 0.0f);
        g.shadow = glm::ivec4(-1, 0, 0, 0);
        g.params = glm::vec4(l.shadowBias, 0, 0, 0);

        const bool wantsShadow = l.castShadows && cubeSlots < settings.maxShadowedPointLights &&
                                 points[i].distance - l.range < settings.maxShadowDistance;
        if (wantsShadow) {
            shadows.buildPointFaces(frame, i, cubeSlots++, g);
            // Instance groups don't draw into cube faces: the batched statics
            // in range join this frame's items for them (each once).
            if (m_staticsActive) in.statics->appendPointShadowCasters(pos, l.range, m_staticStamp, in.fallbackMaterial, frame.items);
        }
    }

    frame.stats.directionalLights = static_cast<uint32_t>(dirs.size());
    frame.stats.pointLights = static_cast<uint32_t>(points.size());
    frame.stats.shadowViews = static_cast<uint32_t>(frame.shadowViews.size());
    frame.globals.lightCounts = glm::uvec4(dirs.size(), points.size(), settings.cascadeCount, 0);
    return static_cast<uint32_t>(points.size());
}

void SceneGatherer::gatherDecals(Scene& scene, const Inputs& in, FrameData& frame)
{
    std::vector<DecalCandidate>& list = m_decalScratch;
    list.clear();
    frame.decalBoxes.clear();
    const CameraData& cam = frame.camera;
    if (in.decals) in.decals->update(frame.time); // (expires spawned decals even while they're off)

    // In view and within its draw distance; fades out over the last 15% of it.
    auto consider = [&](const Decal& d, const glm::mat4& world, float opacity, uint64_t order) {
        if (opacity <= 0.0f || d.color.a <= 0.0f || d.layers == 0) return;
        const AABB box = AABB{glm::vec3(-0.5f), glm::vec3(0.5f)}.transformed(world);
        const float dist = glm::length(glm::max(glm::abs(cam.position - box.center()) - box.extents(), glm::vec3(0.0f)));
        if (dist >= d.drawDistance || !cam.frustum.intersects(box)) return;
        const float fade = std::clamp((d.drawDistance - dist) / (0.15f * d.drawDistance), 0.0f, 1.0f);
        list.push_back({&d, world, opacity * fade, dist, order});
    };
    if (in.settings->decals) {
        scene.forEach<DecalProjector>([&](DecalProjector& p) {
            if (p.isActive()) consider(p, p.transform().worldMatrix(), 1.0f, p.gameObject().id() >> 1);
        });
        if (in.decals) {
            for (const DecalSystem::Entry& e : in.decals->entries()) {
                if (e.serial != 0) consider(e.decal, e.world, DecalSystem::lifeFade(e, frame.time), (1ull << 63) | e.serial);
            }
        }
    }
    // Too many: keep the nearest. Then the drawing order (later on top).
    if (list.size() > kMaxDecals) {
        std::nth_element(list.begin(), list.begin() + kMaxDecals, list.end(),
                         [](const auto& a, const auto& b) { return a.distance < b.distance; });
        list.resize(kMaxDecals);
    }
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
        return a.decal->sortOrder != b.decal->sortOrder ? a.decal->sortOrder < b.decal->sortOrder : a.order < b.order;
    });
    DecalBuffer& out = *frame.decals;
    for (size_t i = 0; i < list.size(); ++i) {
        list[i].decal->toGpu(list[i].world, list[i].opacity, out.decals[i]);
        frame.decalBoxes.push_back(list[i].world);
    }
    frame.stats.decals = static_cast<uint32_t>(list.size());
}

} // namespace ve
