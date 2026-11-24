#pragma once

// =============================================================================
// RendererSettings - the renderer's options as plain data (EngineConfig holds
// one, the F1 overlay edits them live). Kept apart from Renderer.hpp so code
// that only configures the renderer (e.g. the Engine header) needs no GPU types.
// =============================================================================

#include <glm/glm.hpp>

#include <cstdint>

namespace ve {

struct RendererSettings {
    bool vsync = true;
    // Render into an offscreen image instead of presenting (window hidden):
    // automated tests that shouldn't take over the screen. Env: VE_HEADLESS=1.
    bool headless = false;
    // Build mesh pipelines (a shader's first use per pass) on the job pool:
    // no frame hitch, but an object with a new shader isn't drawn for the few
    // frames its pipeline takes (Renderer::compileEvents() to show that).
    // Pipelines start compiling when a shader is created, so loading screens
    // cover most of them. Env: VE_ASYNC_PIPELINES=0 builds them in the frame.
    bool asyncPipelines = true;
    // --- Directional (sun/moon) shadows: cascaded shadow maps ---
    uint32_t maxShadowedDirectionalLights = 2; // 0..kMaxDirectionalLights
    uint32_t cascadeCount = 4;                 // 1..kMaxCascades
    uint32_t directionalShadowResolution = 2048;
    float cascadeSplitLambda = 0.8f; // 0 = uniform splits, 1 = logarithmic
    float maxShadowDistance = 200.0f;
    // Tallest instanced geometry (trees) above the highest regular mesh: bounds
    // how far towards the sun directional shadow casters are gathered.
    float shadowInstanceHeight = 40.0f;
    // Directional shadows: casters are gathered at most this far towards the
    // sun beyond each cascade (distant mountains at sunset within it still
    // shadow the scene). Larger = longer sunset shadows, more overdraw.
    float shadowCasterReach = 600.0f;
    // > 0: directional shadows are rendered from the light direction snapped
    // to steps of this many degrees, so a moving sun doesn't make shadow edges
    // crawl every frame; they jump in visible steps instead. 0 = continuous
    // (TAA hides most of the crawl).
    float shadowDirectionStep = 0.0f;
    // --- Point light shadows: cube maps ---
    uint32_t maxShadowedPointLights = 4;
    uint32_t pointShadowResolution = 512;
    // Re-render a point light's cube face only when something it depends on
    // changed (light position/range, or any caster in range moved, appeared or
    // changed material). Static lights over a static camp then cost nothing;
    // intensity flicker doesn't affect the depth cubes.
    bool cachePointShadows = true;
    // --- Bias (tweak in the F1 overlay) ---
    float shadowNormalBias = 1.5f;   // in shadow texels
    float shadowDepthBiasConstant = 1.0f;
    float shadowDepthBiasSlope = 2.0f;
    float shadowPcfRadius = 1.0f;    // in texels
    bool debugCascades = false;
    // --- GPU instancing (InstanceGroup) ---
    float shadowMinCasterTexels = 2.0f;           // instances smaller than this (in cascade texels) cast no shadow there
    // --- Instance LOD ---
    float lodBias = 1.0f;          // > 1 = higher detail further away (quality slider)
    uint32_t shadowLodBias = 1;    // shadows use this many LODs coarser than the camera
    bool lodCrossfade = true;      // dithered transitions instead of popping
    float lodFadeBand = 0.15f;     // transition width, fraction of the LOD threshold
    // Point lights are binned into screen/depth clusters so each pixel only
    // evaluates the lights that reach it (off = every pixel loops over all).
    bool clusteredLights = true;
    // Decals (DecalProjector, Renderer::decals()): off = none are drawn.
    bool decals = true;
    // Instances (trees, rocks...) hidden behind what was drawn last frame are
    // skipped (Hi-Z occlusion culling of the camera view; HiZPass). Something
    // that comes into view from behind cover can appear a frame late.
    bool occlusionCulling = true;
    // Static MeshRenderers (MeshRenderer::isStatic) go into GPU instance groups
    // once (StaticBatcher) instead of being gathered, culled and drawn one by
    // one every frame. Off = every MeshRenderer takes the per-frame path
    // (A/B comparisons; F1 > Instance culling).
    bool staticBatching = true;
    // --- Anti-aliasing ---
    // Temporal AA: the camera jitters by a sub-pixel offset every frame and
    // TemporalAAPass blends each frame into a reprojected history. Smooths
    // edges, foliage cutouts and dithering; moving objects can smear slightly.
    bool temporalAA = false;
    uint32_t maxInstances = 262144;               // total across all groups (64 B each)
    uint32_t maxVisibleInstancesPerView = 1u << 20; // sum over batches of group capacity (4 B each, per cull view)
    // --- GPU skinning (SkinnedMeshRenderer) ---
    uint32_t maxSkinnedVertices = 262144; // rest + output vertices of all skinned meshes (64 B each, twice)
    uint32_t maxSkinJoints = 8192;        // joint matrices per frame (64 B each, per frame in flight)
};

} // namespace ve
