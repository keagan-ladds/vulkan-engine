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
//   3. one DrawItem per MeshRenderer submesh, with last frame's world matrix
//      (motion vectors); camera-culled, sorted opaque / transparent lists
//   4. directional + point lights into the LightBuffer, shadow views via
//      ShadowSetup
//
// It keeps last frame's world matrix per renderer and its scratch lists
// between frames (no per-frame allocations once warmed up).
// =============================================================================

#include "ve/render/FrameData.hpp"

#include "ve/gfx/Vk.hpp"

#include <cstdint>
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
struct RendererSettings;

class SceneGatherer {
public:
    struct Inputs {
        VkExtent2D extent{1, 1};        // render size (camera aspect)
        glm::vec2 jitter{0.0f};         // sub-pixel camera offset this frame (pixels)
        SkinningSystem* skinning = nullptr;
        Material* fallbackMaterial = nullptr; // for MeshRenderers without a material
        const RendererSettings* settings = nullptr;
        const ShadowSetup* shadows = nullptr;
    };

    // Resets `frame`'s per-frame lists and stats and fills them from `scene`
    // (frame.lights must point at the LightBuffer to fill). Returns the
    // number of point lights written (input to LightClusterer).
    uint32_t gather(Scene& scene, const Inputs& in, FrameData& frame);

private:
    void setupCamera(Scene& scene, const Inputs& in, CameraData& cam) const;
    void gatherDrawItems(Scene& scene, const Inputs& in, FrameData& frame);
    uint32_t gatherLights(Scene& scene, const Inputs& in, FrameData& frame);

    struct PrevWorld {
        glm::mat4 world;
        uint64_t stamp; // gather() call that last saw the renderer
    };
    std::unordered_map<const MeshRenderer*, PrevWorld> m_prevWorlds; // motion vectors
    uint64_t m_prevWorldStamp = 0;
    std::vector<const MeshRenderer*> m_deforming; // this frame's skinned renderers, sorted
    float m_sceneTop = 0.0f; // highest point of this frame's draw items

    // Per-frame scratch of gatherLights (reused, no allocations).
    struct PointCandidate {
        PointLight* light;
        float distance;
    };
    std::vector<DirectionalLight*> m_dirScratch;
    std::vector<PointCandidate> m_pointScratch;
};

} // namespace ve
