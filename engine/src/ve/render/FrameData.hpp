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

struct DrawItem {
    const Mesh* mesh = nullptr;
    const Submesh* submesh = nullptr;
    Material* material = nullptr;
    glm::mat4 world{1.0f};
    glm::mat4 prevWorld{1.0f}; // last frame's world matrix (motion vectors)
    AABB bounds;          // world space
    float distance = 0.0f; // distance from the camera (sorting)
    bool castShadows = true;
    const GameObject* owner = nullptr; // the MeshRenderer's object (editor picking)
};

struct CameraData {
    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f};     // reversed-Z, Y flipped for Vulkan
    glm::mat4 viewProj{1.0f};
    glm::mat4 viewProjNoJitter{1.0f}; // without the TAA / photo sub-pixel jitter
    glm::vec3 position{0.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    glm::vec3 right{1.0f, 0.0f, 0.0f};
    float fovY = 1.0f;        // radians
    float aspect = 1.0f;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
    Frustum frustum;
};

// One shadow map "view": a cascade of a directional light or a cube face of a point light.
struct ShadowView {
    glm::mat4 viewProj{1.0f};
    Frustum frustum;
    uint32_t viewIndex = 0;  // index into LightBuffer::shadowViews
    uint32_t layer = 0;      // layer in DirShadowMap or PointShadowMap
    uint32_t lightIndex = 0; // index into LightBuffer::directional / point
    bool point = false;
    glm::vec3 lightPosition{0.0f}; // point lights: for range culling
    float range = 0.0f;
    uint32_t cullView = 0xFFFFFFFFu; // GPU instance cull view (directional only), set by InstanceSystem
    float texelSize = 0.0f;          // directional: world size of one shadow texel in this cascade
};

struct RenderStats {
    uint32_t drawCalls = 0;
    uint32_t shadowDrawCalls = 0;
    uint32_t triangles = 0;
    uint32_t visibleObjects = 0;
    uint32_t totalObjects = 0;
    uint32_t directionalLights = 0;
    uint32_t pointLights = 0;
    uint32_t shadowViews = 0;
};

struct FrameData {
    Scene* scene = nullptr;
    FrameGlobals globals{};      // editable in RenderPass::prepare()
    LightBuffer* lights = nullptr;
    CameraData camera;
    std::vector<DrawItem> items;      // every visible-flagged MeshRenderer submesh (for shadows)
    std::vector<uint32_t> opaque;     // indices into items, camera-culled, sorted
    std::vector<uint32_t> transparent;
    std::vector<uint32_t> moving;      // opaque items whose world matrix changed since last frame (motion vectors)
    std::vector<ShadowView> shadowViews;
    RenderStats stats;
    float deltaTime = 0.0f;
    float time = 0.0f;
    uint64_t frameNumber = 0;
};

} // namespace ve
