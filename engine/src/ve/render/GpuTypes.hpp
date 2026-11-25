#pragma once

// =============================================================================
// GpuTypes - C++ mirrors of the GPU-side structures declared in
// engine/shaders/include/ve.glsl.
//
// !!! Keep this file and ve.glsl in sync. !!!
// Every struct here is made of 16-byte aligned members (vec4/mat4/uvec4) so
// that the C++ layout matches both std140 (uniform buffers) and std430
// (storage buffers) without any padding surprises.
// =============================================================================

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>

namespace ve {

constexpr uint32_t kMaxDirectionalLights = 4;
constexpr uint32_t kMaxPointLights = 256;
constexpr uint32_t kHiZLevels = 10; ///< depth pyramid 512x256 .. 1x1 (occlusion culling)
constexpr uint32_t kMaxGrassBenders = 8; ///< objects pushing grass aside (ve::GrassBender), nearest to the camera
/// Clustered point lights: the view frustum is split into a grid of clusters
/// (screen tiles x logarithmic depth slices), each listing the lights that reach it.
constexpr uint32_t kClusterX = 16, kClusterY = 9, kClusterZ = 24;
constexpr uint32_t kClusterCount = kClusterX * kClusterY * kClusterZ;
constexpr uint32_t kClusterIndexCapacity = 65536; ///< light indices (8 bits each, 4 per uint)
constexpr uint32_t kMaxCascades = 4;
constexpr uint32_t kMaxShadowViews = 128; ///< directional cascades + 6 per shadowed point light
constexpr uint32_t kMaxMaterials = 4096;
constexpr uint32_t kGlobalParamCount = 8;
constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;
/// Decals (DecalProjector, DecalSystem): the nearest kMaxDecals in view are
/// binned into the same clusters as the point lights (16-bit indices, 2 per uint).
constexpr uint32_t kMaxDecals = 1024;
constexpr uint32_t kDecalIndexCapacity = 32768;

/// set 0, binding 0 (uniform buffer)
struct FrameGlobals {
    glm::mat4 view;  ///< World to view.
    glm::mat4 proj;  ///< View to clip (reversed Z, Y flipped).
    glm::mat4 viewProj;  ///< World to clip (jittered).
    glm::mat4 invView;  ///< View to world.
    glm::mat4 invProj;  ///< Clip to view.
    glm::mat4 invViewProj;  ///< Clip to world (reconstruct positions from depth).
    glm::mat4 prevViewProj;  ///< Last frame's world to clip (motion vectors, TAA).
    glm::vec4 cameraPosition; ///< xyz, w = near plane
    glm::vec4 cameraForward;  ///< xyz, w = far plane
    glm::vec4 screenSize;     ///< width, height, 1/width, 1/height
    glm::vec4 time;           ///< x = time, y = delta time, z = frame index, w = real time
    glm::vec4 ambientSky;     ///< rgb (pre-multiplied by intensity), w unused
    glm::vec4 ambientGround;  ///< rgb, w = sunlit-bounce fraction (Environment::groundSunFraction)
    glm::uvec4 lightCounts;   ///< x = directional, y = point, z = cascade count, w = debug flags
    glm::vec4 shadowParams;   ///< x = normal bias (texels), y = PCF radius (texels), z = dir shadow map size, w = point shadow map size
    glm::uvec4 builtinTextures; ///< x = SceneDepth, y = SceneColor, z = SceneNormal (bindless indices), w = reserved
    glm::vec4 globalParams[kGlobalParamCount]; ///< free-form game/effect globals (see vfx.glsl for the effects convention)
    glm::vec4 jitter; ///< xy = how far this frame's sub-pixel camera jitter moved the image, in UV units (TAA, photo mode)
    glm::vec4 grassBenders[kMaxGrassBenders]; ///< xyz = position (feet), w = radius (0 = unused); see ve::GrassBender
};

/// A directional light as shaders see it (LightBuffer).
struct GpuDirectionalLight {
    glm::vec4 direction;     ///< xyz = direction the light travels (normalized), w unused
    glm::vec4 color;         ///< rgb = colour * intensity, w unused
    glm::ivec4 shadow;       ///< x = first shadow view index (-1 = no shadow), y = cascade count, z = first layer in DirShadowMap
    glm::vec4 cascadeSplits; ///< far distance (view depth) of each cascade
    glm::vec4 cascadeTexel;  ///< world-space size of one shadow texel, per cascade (for normal-offset bias)
};

/// A point light as shaders see it (LightBuffer).
struct GpuPointLight {
    glm::vec4 positionRange; ///< xyz = position, w = range
    glm::vec4 color;         ///< rgb = colour * intensity
    glm::ivec4 shadow;       ///< x = cube index in PointShadowMap (-1 = none), y = first shadow view index
    glm::vec4 params;        ///< x = shadow bias, y = spot: 1 / (cos inner - cos outer), z = cookie texture index (-1 = none), w = chromatic aberration (0 = off)
    glm::vec4 spot;          ///< spot lights: xyz = direction it shines, w = cos(outer angle); point lights: w = -2
    glm::vec4 spotBasis;     ///< spot lights: xyz = the light's right axis (cookie orientation), w = 1 / tan(outer angle)
};
static_assert(sizeof(GpuPointLight) == 96);

/// set 0, binding 1 (storage buffer)
struct LightBuffer {
    GpuDirectionalLight directional[kMaxDirectionalLights];  ///< Directional lights (lightCounts.x used).
    GpuPointLight point[kMaxPointLights];  ///< Point lights (lightCounts.y used).
    glm::mat4 shadowViews[kMaxShadowViews]; ///< view-projection matrices used to render/sample shadows
    /// x, y = depth slice = log(view depth) * x + y; z = 1 if the clusters are valid
    /// (0 = shaders loop over every point light), w unused
    glm::vec4 clusterParams;
    uint32_t clusters[kClusterCount];                       ///< first index << 9 | light count
    uint32_t clusterLights[kClusterIndexCapacity / 4];      ///< point light indices, 8 bits each
};

/// One decal: an oriented box that projects its textures onto whatever
/// surface lies inside it, along its local -Z (see ve_decals.glsl). 128 bytes.
struct GpuDecal {
    glm::vec4 rows[3];   ///< world -> decal space (the box is -0.5..0.5 on each axis), row-major affine
    glm::vec4 color;     ///< rgb = linear colour, a = opacity (distance / lifetime fade included)
    glm::vec4 opacity;   ///< per channel: x = albedo, y = normal, z = roughness, w = metallic
    glm::vec4 params;    ///< x = roughness, y = metallic, z = normal strength, w = flatten (smooths the surface's own bumps)
    glm::vec4 fade;      ///< x = cos(angle fade start), y = cos(angle fade end), z = depth fade (fraction of half the box), w = blend (0 normal, 1 multiply)
    glm::uvec4 textures; ///< x = albedo (rgba), y = normal, z = mask (G = roughness, B = metallic, A = opacity), w = layer mask
};
static_assert(sizeof(GpuDecal) == 128);

/// set 0, binding 14 (storage buffer). Only the used parts are uploaded.
struct DecalBuffer {
    glm::uvec4 info;         ///< x = decal count, y = 1 if the clusters are valid (0 = every pixel tests every decal)
    glm::vec4 clusterParams; ///< depth slice = log(view depth) * x + y (same grid as the light clusters)
    uint32_t clusters[kClusterCount];                 ///< first index << 16 | decal count
    GpuDecal decals[kMaxDecals];                      ///< sorted: later decals are drawn over earlier ones
    uint32_t clusterDecals[kDecalIndexCapacity / 2];  ///< decal indices, 16 bits each
};
static_assert(offsetof(DecalBuffer, decals) % 16 == 0);

/// set 0, binding 2 (storage buffer, array). 256 bytes per material.
/// The meaning of params/textures is defined by each shader; see
/// StandardMaterial for the built-in layout.
constexpr uint32_t kMaterialParamCount = 12;   ///< vec4 parameters per material
constexpr uint32_t kMaterialTextureCount = 16; ///< bindless texture slots per material
/// One material's GPU data (veMaterial() in shaders).
struct GpuMaterial {
    glm::vec4 params[kMaterialParamCount];  ///< Shader-defined vec4 parameters (Material::setVector).
    uint32_t textures[kMaterialTextureCount];  ///< Bindless texture indices (Material::setTexture).
};
static_assert(sizeof(GpuMaterial) == 256);

/// Size of the push-constant range of the shared pipeline layout: the minimum
/// every Vulkan device guarantees (maxPushConstantsSize >= 128).
constexpr uint32_t kPushConstantBytes = 128;

/// Default push-constant block for mesh draws (96 of the 128 available bytes).
struct DrawPush {
    glm::mat4 model;  ///< Object to world.
    uint32_t materialIndex;  ///< Index into the material buffer.
    uint32_t viewIndex;  ///< shadow passes: index into LightBuffer.shadowViews
    uint32_t lightIndex; ///< point shadow pass: index of the point light
    uint32_t flags;      ///< VE_DRAW_FLAG_* (see ve.glsl)
    glm::vec4 custom;    ///< free for custom shaders
};
static_assert(sizeof(DrawPush) <= kPushConstantBytes);

constexpr uint32_t kDrawFlagPointShadow = 1u << 0;
constexpr uint32_t kDrawFlagInstanced = 1u << 1; ///< model matrix comes from the instance buffer

// ---- GPU-driven instancing (see render/Instancing.hpp, instance_cull.comp) ----

/// One instance (set 0, binding 5). 64 bytes.
struct GpuInstance {
    glm::vec4 rows[3]; ///< affine transform, row-major (= transposed mat3x4)
    glm::vec4 custom;  ///< free per-instance data; standard.vert: if w > 0, rgb tints the vertex colour
};
static_assert(sizeof(GpuInstance) == 64);

/// One InstanceGroup as the cull shader sees it (binding 8). 96 bytes.
struct GpuInstanceGroup {
    glm::vec4 localSphere;    ///< bounding sphere of LOD 0 in mesh space (xyz centre, w radius)
    glm::vec4 lodDistances;   ///< max camera distance of LOD i
    glm::uvec4 lodBatchStart; ///< first draw batch of LOD i
    glm::uvec4 lodBatchCount; ///< number of batches (= submeshes) of LOD i
    uint32_t firstInstance;   ///< into the instance buffer
    uint32_t count;  ///< Instances in the group.
    uint32_t lodCount;  ///< LODs used (1..4).
    uint32_t flags;           ///< bit 0: casts shadows
    float cullDistance;  ///< Not drawn beyond this camera distance.
    float shadowDistance; ///< no shadow casting beyond this camera distance
    uint32_t pad[2];  ///< (Padding to 96 bytes.)
};
static_assert(sizeof(GpuInstanceGroup) == 96);

/// A view to cull against (binding 9): view 0 = camera, then directional shadow cascades.
struct GpuCullView {
    glm::vec4 planes[6];  ///< Frustum planes (xyz normal, w distance), pointing inwards.
    glm::vec4 position; ///< xyz camera position (LOD distances use view 0), w = min bounding radius drawn (shadow texel size)
};

/// Matches VkDrawIndexedIndirectCommand (20 bytes) - written by the cull shader.
struct DrawIndexedIndirect {
    uint32_t indexCount;  ///< Indices per instance.
    uint32_t instanceCount;  ///< Instances to draw (written by culling).
    uint32_t firstIndex;  ///< First index in the index buffer.
    int32_t vertexOffset;  ///< Added to each index.
    uint32_t firstInstance;  ///< First entry in the visible-instance list.
};
static_assert(sizeof(DrawIndexedIndirect) == 20);

} // namespace ve
