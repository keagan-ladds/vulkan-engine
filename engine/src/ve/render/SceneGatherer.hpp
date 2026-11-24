#pragma once

// =============================================================================
// SceneGatherer - reads this frame's camera, draw items and lights out of the
// Scene (part of Renderer).
//
// The only part of the renderer that looks at scene components (Camera,
// MeshRenderer, DirectionalLight, PointLight): it turns them into the plain
// data in FrameData that passes consume. Per frame (gather()):
//
//   1. camera matrices + frustum (with the TAA / photo sub-pixel jitter)
//   2. skinned meshes (SkinningSystem gatherers) -> `deforming` flags
//   3. static renderers: applies what the scene reports changed
//      (Scene::StaticChanges) to the StaticBatcher; batched ones are skipped
//      below (their instance groups draw them)
//   4. one DrawItem per other MeshRenderer submesh, with last frame's world
//      matrix (motion vectors); camera-culled, sorted opaque / transparent lists
//   5. directional + point lights into the LightBuffer, shadow views via
//      ShadowSetup; batched statics near shadowed point lights as DrawItems
//   6. decals in view (DecalProjector components + DecalSystem's spawned
//      ones) into the DecalBuffer: nearest kMaxDecals, sorted by sortOrder
//
// It keeps last frame's world matrix per renderer and its scratch lists
// between frames (no per-frame allocations once warmed up).
// =============================================================================

#include "ve/render/FrameData.hpp"

#include "ve/gfx/Vk.hpp"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace ve {

class Scene;
class Material;
class MeshRenderer;
class DirectionalLight;
class PointLight;
class SkinningSystem;
class ShadowSetup;
class StaticBatcher;
class DecalSystem;
struct Decal;
struct RendererSettings;

/// Collects a frame's draw items, lights, decals and static changes from the scene (Renderer internal).
class SceneGatherer {
public:
    /// What it needs from the renderer.
    struct Inputs {
        VkExtent2D extent{1, 1};        ///< render size (camera aspect)
        glm::vec2 jitter{0.0f};         ///< sub-pixel camera offset this frame (pixels)
        SkinningSystem* skinning = nullptr;  ///< GPU skinning (skinned renderers).
        Material* fallbackMaterial = nullptr; ///< for MeshRenderers without a material
        const RendererSettings* settings = nullptr;  ///< The renderer's settings (shadow limits...).
        const ShadowSetup* shadows = nullptr;  ///< Builds shadow views.
        StaticBatcher* statics = nullptr;  ///< Static batching (null: off).
        DecalSystem* decals = nullptr;  ///< Fire-and-forget decals.
    };

    /// Resets `frame`'s per-frame lists and stats and fills them from `scene`
    /// (frame.lights / frame.decals must point at the buffers to fill). Returns the
    /// number of point lights written (input to LightClusterer).
    uint32_t gather(Scene& scene, const Inputs& in, FrameData& frame);

private:
    void setupCamera(Scene& scene, const Inputs& in, CameraData& cam) const;
    void gatherDrawItems(Scene& scene, const Inputs& in, FrameData& frame);
    uint32_t gatherLights(Scene& scene, const Inputs& in, FrameData& frame);
    void gatherDecals(Scene& scene, const Inputs& in, FrameData& frame);
    /// Static batching: binds the batcher to a scene, applies its changes.
    /// False: nothing of `scene` is batched this frame (all drawn per item).
    bool updateStatics(Scene& scene, const Inputs& in);
    void updateStatic(Scene& scene, MeshRenderer& mr, const Inputs& in);

    struct PrevWorld {
        glm::mat4 world;
        uint64_t stamp; ///< gather() call that last saw the renderer
    };
    std::unordered_map<const MeshRenderer*, PrevWorld> m_prevWorlds; ///< motion vectors
    uint64_t m_prevWorldStamp = 0;
    std::vector<const MeshRenderer*> m_deforming; ///< this frame's skinned renderers, sorted
    float m_sceneTop = 0.0f; ///< highest point of this frame's draw items

    /// Static batching: the scene whose renderers have batch entries (null =
    /// none) and its token (expired = that scene is gone, entries with it).
    Scene* m_staticScene = nullptr;
    std::weak_ptr<const int> m_staticSceneAlive;
    bool m_staticsActive = false;            ///< this frame's scene is the batched one
    uint64_t m_staticStamp = 0;              ///< per gather (point-shadow casters appended once)
    std::vector<MeshRenderer*> m_staticDirty; ///< scratch: the scene's queued renderers
    bool m_staticFullWarned = false;

    /// Per-frame scratch of gatherLights (reused, no allocations).
    struct PointCandidate {
        PointLight* light;
        float distance;
    };
    std::vector<DirectionalLight*> m_dirScratch;
    std::vector<PointCandidate> m_pointScratch;
    struct DecalCandidate {
        const Decal* decal;
        glm::mat4 world;
        float opacity;
        float distance;
        uint64_t order; ///< within a sortOrder: placed decals first (by id), then spawned ones (oldest first)
    };
    std::vector<DecalCandidate> m_decalScratch;
};

} // namespace ve
