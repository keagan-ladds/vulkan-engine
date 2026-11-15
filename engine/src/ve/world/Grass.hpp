#pragma once

// =============================================================================
// GrassPass - dense grass around the camera, placed entirely on the GPU.
//
//   ve::GrassDesc g;
//   g.boundsMin = {-320, -320}; g.boundsMax = {320, 320};
//   g.resolution = 321;                                        // height samples per side
//   g.height = [&](glm::vec2 p) { return terrain.height(p.x, p.y); };
//   g.mask = [&](glm::vec2 p) { return glm::vec4(density, colourMix, heightScale, 0); };
//   g.colorA = {0.78f, 0.63f, 0.32f}; g.colorB = {0.55f, 0.55f, 0.26f};
//   renderer.graph().addPass<ve::GrassPass>(g);
//
// Nothing is stored per blade. The height and mask functions are sampled once
// into textures. Each frame the pass walks square tiles around the camera,
// frustum-culls them on the CPU, and draws a small clump mesh instanced per
// tile. The vertex shader (ve_grass.glsl) turns gl_InstanceIndex into a
// position from a low-discrepancy (R2) sequence. A clump survives if its index
// fraction is below mask density × distance falloff, so grass thins smoothly
// with distance and never flickers. Wind comes from
// frame.globalParams[windSlot] (xy direction, z strength; the vfx convention).
//
// Runs in RenderStage::Opaque after OpaquePass (writes colour, the thin
// G-buffer and depth; casts no shadows). For a custom look, keep
// grass_field.vert and write your own fragment shader with the same inputs
// (see grass_field.frag), or start your vertex shader from ve_grass.glsl.
// Alpha-clipped grass cards: set GrassDesc::cardMesh + cardTexture.
// =============================================================================

#include "ve/render/Mesh.hpp"
#include "ve/render/RenderGraph.hpp"
#include "ve/render/Renderer.hpp"

#include <glm/glm.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ve {

class Material;
class Mesh;
class Shader;
class Texture;

struct GrassDesc {
    std::string name = "Grass";
    glm::vec2 boundsMin{-256.0f}, boundsMax{256.0f};
    uint32_t resolution = 513; // height samples per side (include both edges)
    uint32_t maskResolution = 512;
    std::function<float(glm::vec2)> height;   // required; must match the rendered ground
    std::function<glm::vec4(glm::vec2)> mask; // x density 0..1, y colour mix A->B, z height scale, w free; null = full
    glm::vec3 colorA{0.55f, 0.6f, 0.25f};
    glm::vec3 colorB{0.75f, 0.65f, 0.35f};

    float spacing = 0.8f;      // clump spacing at full density (m)
    float radius = 110.0f;     // drawn within this distance of the camera
    float fadeStart = 30.0f;   // density starts thinning here...
    float farDensity = 0.3f;   // ...down to this fraction at the radius
    float minHeight = 0.45f, maxHeight = 1.0f;
    float bladeWidth = 0.05f;
    uint32_t bladesPerClump = 4;
    float clumpRadius = 0.25f;
    // Per-clump random rotation: always a full random yaw (around up), plus a
    // tilt of up to maxTilt radians around a random horizontal axis, so clumps
    // don't all look the same from the side.
    float maxTilt = 0.2f;
    float tileSize = 16.0f;

    // ---- Grass cards (alpha-clipped textured quads, Valheim-style) ----
    // Set cardMesh to draw it instead of the procedural blades: authored with
    // the root at the origin and height 1 (scaled per clump to min..maxHeight,
    // width scales with it). uv goes to the card texture; vertex y (0..1) is
    // the sway weight. cardMeshCrossed() builds the usual crossed quads.
    // colorA/colorB multiply the texture (use white for the texture's own colours).
    MeshData cardMesh;
    std::shared_ptr<Texture> cardTexture; // RGBA, alpha = blade coverage
    float alphaCutoff = 0.5f;
    static MeshData cardMeshCrossed(uint32_t quads = 3, float width = 1.0f);
    int windSlot = 0;          // frame.globalParams index with wind; -1 = no wind
    // Shadows: clumps within shadowDistance of the camera are drawn into the
    // sun's shadow cascades (cards alpha-tested), so grass shadows the ground,
    // itself and anything under it. Cascades whose texels are coarser than
    // shadowMaxTexel (m) are skipped: blades would be sub-texel there anyway.
    bool castShadows = true;
    float shadowDistance = 30.0f;
    float shadowMaxTexel = 0.12f;
    float sway = 1.0f;

    std::string vertexShader = "grass_field.vert";
    std::string fragmentShader;        // empty = grass_field.frag (blades) or grass_card.frag (cards)
};

class GrassPass : public RenderPass, public ShadowCaster {
public:
    explicit GrassPass(GrassDesc desc);
    ~GrassPass() override;

    void setup(RenderGraph& graph) override;
    void declare(PassBuilder& builder) override;
    void execute(RenderContext& ctx) override;
    void onGui() override;
    void drawShadow(RenderContext& ctx, const ShadowView& view) override;

    // Parameters are live: change them any time.
    GrassDesc& desc() { return m_desc; }
    Material& material() { return *m_material; }

private:
    void updateMaterial();
    // Draws the visible tiles; returns {tiles, clumps}.
    glm::uvec2 drawTiles(RenderContext& ctx, const Frustum& frustum, float maxDistance, uint32_t viewIndex);

    GrassDesc m_desc;
    std::shared_ptr<Texture> m_height, m_mask;
    std::shared_ptr<Mesh> m_clump;
    std::shared_ptr<Shader> m_shader;
    std::shared_ptr<Material> m_material;
    std::vector<glm::vec2> m_tileHeights; // min/max ground height per tile
    int m_tilesX = 0, m_tilesZ = 0;
    uint32_t m_lastTiles = 0, m_lastClumps = 0, m_shadowClumps = 0, m_lastShadowClumps = 0;
    bool m_registered = false;
};

} // namespace ve
