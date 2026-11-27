#pragma once

// =============================================================================
// FrameData - everything the renderer gathered from the scene for this frame.
// Passed to every RenderPass (prepare() and execute()).
// =============================================================================

#include "ve/render/Geometry.hpp"
#include "ve/render/GpuTypes.hpp"

#include <vector>

namespace ve {

class GameObject;

class Mesh;
class Material;
class Scene;
struct Submesh;

/// One submesh to draw this frame (gathered from MeshRenderers).
struct DrawItem {
    const Mesh* mesh = nullptr;  ///< The mesh (vertex / index buffers).
    const Submesh* submesh = nullptr;  ///< Which part of it (index range, material slot).
    Material* material = nullptr;  ///< The material to draw it with.
    glm::mat4 world{1.0f};  ///< Object-to-world matrix.
    glm::mat4 prevWorld{1.0f}; ///< last frame's world matrix (motion vectors)
    AABB bounds;          ///< world space
    float distance = 0.0f; ///< distance from the camera (sorting)
    bool castShadows = true;  ///< Drawn into shadow maps.
    bool deforming = false; ///< vertices change on the GPU (skinning): never cache shadows of it
    /// A batched static renderer (StaticBatcher): its instance group draws it
    /// for the camera and the directional cascades; this item is only for the
    /// point-light cube faces in range (never in opaque / transparent).
    bool instanced = false;
    const GameObject* owner = nullptr; ///< the MeshRenderer's object (editor picking)
};

/// The camera this frame renders from.
struct CameraData {
    glm::mat4 view{1.0f};  ///< World-to-view matrix.
    glm::mat4 proj{1.0f};     ///< reversed-Z, Y flipped for Vulkan
    glm::mat4 viewProj{1.0f};  ///< proj * view (with the TAA / photo jitter).
    glm::mat4 viewProjNoJitter{1.0f}; ///< without the TAA / photo sub-pixel jitter
    glm::vec3 position{0.0f};  ///< World position.
    glm::vec3 forward{0.0f, 0.0f, -1.0f};  ///< World forward direction (-Z of the camera).
    glm::vec3 up{0.0f, 1.0f, 0.0f};  ///< World up direction.
    glm::vec3 right{1.0f, 0.0f, 0.0f};  ///< World right direction.
    float fovY = 1.0f;        ///< radians
    float aspect = 1.0f;  ///< Width / height.
    float nearPlane = 0.1f;  ///< Near clip distance (m).
    float farPlane = 1000.0f;  ///< Far clip distance (m).
    Frustum frustum;  ///< View frustum planes (world space) for culling.
};

/// One shadow map "view": a cascade of a directional light or a cube face of a point light.
struct ShadowView {
    glm::mat4 viewProj{1.0f};  ///< The shadow view's projection * view.
    Frustum frustum;  ///< Its frustum (culling casters).
    uint32_t viewIndex = 0;  ///< index into LightBuffer::shadowViews
    uint32_t layer = 0;      ///< layer in DirShadowMap or PointShadowMap
    uint32_t lightIndex = 0; ///< index into LightBuffer::directional / point
    bool point = false;  ///< A point-light cube face (else a directional cascade).
    glm::vec3 lightPosition{0.0f}; ///< point lights: for range culling
    float range = 0.0f;  ///< Point lights: their range (m).
    uint32_t cullView = 0xFFFFFFFFu; ///< GPU instance cull view (directional only), set by InstanceSystem
    float texelSize = 0.0f;          ///< directional: world size of one shadow texel in this cascade
};

/// Counters of the last frame (F1 overlay, benchmarks).
struct RenderStats {
    uint32_t drawCalls = 0;  ///< Draw calls in the camera passes.
    uint32_t shadowDrawCalls = 0;  ///< Draw calls into shadow maps.
    uint32_t triangles = 0;  ///< Triangles drawn (camera passes).
    uint32_t visibleObjects = 0;      ///< CPU-drawn submeshes in view (batched statics: InstanceStats)
    uint32_t totalObjects = 0;        ///< all submeshes, batched statics included
    uint32_t staticObjects = 0;       ///< batched static submeshes (StaticBatcher)
    uint32_t directionalLights = 0;  ///< Directional lights in use.
    uint32_t pointLights = 0;  ///< Point lights in use.
    uint32_t shadowViews = 0;  ///< Shadow views rendered (cascades + cube faces).
    uint32_t decals = 0;              ///< decals in view (DecalBuffer)
};

/// Everything gathered for one frame: camera, lights, draw lists, shadow views. Renderer::frameData().
struct FrameData {
    Scene* scene = nullptr;  ///< The scene being rendered.
    FrameGlobals globals{};      ///< editable in RenderPass::prepare()
    LightBuffer* lights = nullptr;  ///< This frame's light buffer (mapped).
    DecalBuffer* decals = nullptr;   ///< decals in view: decals->decals[0, info.x), sorted (see Decals.hpp)
    std::vector<glm::mat4> decalBoxes; ///< their world matrices (unit box -> world), same order
    CameraData camera;  ///< The camera.
    /// Every visible-flagged MeshRenderer submesh (for shadows), then the
    /// batched static ones near shadowed point lights (DrawItem::instanced).
    std::vector<DrawItem> items;
    std::vector<uint32_t> opaque;     ///< indices into items, camera-culled, sorted
    std::vector<uint32_t> transparent;  ///< Indices into items, back to front.
    std::vector<uint32_t> moving;      ///< opaque items whose world matrix changed since last frame (motion vectors)
    std::vector<ShadowView> shadowViews;  ///< Shadow views to render this frame.
    RenderStats stats;  ///< Counters for this frame.
    float deltaTime = 0.0f;  ///< Frame time in seconds.
    float time = 0.0f;  ///< Time since start in seconds (scaled).
    uint64_t frameNumber = 0;  ///< Frames rendered so far.
};

} // namespace ve
