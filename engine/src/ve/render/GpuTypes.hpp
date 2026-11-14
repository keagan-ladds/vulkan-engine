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

#include <cstdint>

namespace ve {

constexpr uint32_t kMaxDirectionalLights = 4;
constexpr uint32_t kMaxPointLights = 256;
constexpr uint32_t kMaxCascades = 4;
constexpr uint32_t kMaxShadowViews = 128; // directional cascades + 6 per shadowed point light
constexpr uint32_t kMaxMaterials = 4096;
constexpr uint32_t kGlobalParamCount = 8;
constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

// set 0, binding 0 (uniform buffer)
struct FrameGlobals {
    glm::mat4 view;
    glm::mat4 proj;
    glm::mat4 viewProj;
    glm::mat4 invView;
    glm::mat4 invProj;
    glm::mat4 invViewProj;
    glm::mat4 prevViewProj;
    glm::vec4 cameraPosition; // xyz, w = near plane
    glm::vec4 cameraForward;  // xyz, w = far plane
    glm::vec4 screenSize;     // width, height, 1/width, 1/height
    glm::vec4 time;           // x = time, y = delta time, z = frame index, w = real time
    glm::vec4 ambientSky;     // rgb (pre-multiplied by intensity), w unused
    glm::vec4 ambientGround;  // rgb, w unused
    glm::uvec4 lightCounts;   // x = directional, y = point, z = cascade count, w = debug flags
    glm::vec4 shadowParams;   // x = normal bias (texels), y = PCF radius (texels), z = dir shadow map size, w = point shadow map size
    glm::uvec4 builtinTextures; // x = SceneDepth bindless index, y = SceneColor (previous pass copy if any), zw = reserved
    glm::vec4 globalParams[kGlobalParamCount]; // free-form game/effect globals (see vfx.glsl for the effects convention)
};

struct GpuDirectionalLight {
    glm::vec4 direction;     // xyz = direction the light travels (normalized), w unused
    glm::vec4 color;         // rgb = colour * intensity, w unused
    glm::ivec4 shadow;       // x = first shadow view index (-1 = no shadow), y = cascade count, z = first layer in DirShadowMap
    glm::vec4 cascadeSplits; // far distance (view depth) of each cascade
    glm::vec4 cascadeTexel;  // world-space size of one shadow texel, per cascade (for normal-offset bias)
};

struct GpuPointLight {
    glm::vec4 positionRange; // xyz = position, w = range
    glm::vec4 color;         // rgb = colour * intensity
    glm::ivec4 shadow;       // x = cube index in PointShadowMap (-1 = none), y = first shadow view index
    glm::vec4 params;        // x = shadow bias, yzw reserved
};

// set 0, binding 1 (storage buffer)
struct LightBuffer {
    GpuDirectionalLight directional[kMaxDirectionalLights];
    GpuPointLight point[kMaxPointLights];
    glm::mat4 shadowViews[kMaxShadowViews]; // view-projection matrices used to render/sample shadows
};

// set 0, binding 2 (storage buffer, array). 256 bytes per material.
// The meaning of params/textures is defined by each shader; see
// StandardMaterial for the built-in layout.
struct GpuMaterial {
    glm::vec4 params[12];
    uint32_t textures[16];
};
static_assert(sizeof(GpuMaterial) == 256);

// Default push-constant block for mesh draws (96 of the 128 available bytes).
struct DrawPush {
    glm::mat4 model;
    uint32_t materialIndex;
    uint32_t viewIndex;  // shadow passes: index into LightBuffer.shadowViews
    uint32_t lightIndex; // point shadow pass: index of the point light
    uint32_t flags;      // VE_DRAW_FLAG_* (see ve.glsl)
    glm::vec4 custom;    // free for custom shaders
};
static_assert(sizeof(DrawPush) <= 128);

constexpr uint32_t kDrawFlagPointShadow = 1u << 0;

} // namespace ve
