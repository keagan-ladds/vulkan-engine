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
//
// LOD chain (e.g. a detailed modelled clump near the camera, a simpler one
// further out, cards beyond): fill GrassDesc::lods, nearest first, each with
// its own mesh (empty = procedural blades), texture and switch distance.
// Every clump picks its LOD from its own jittered distance, so the switch is
// dithered over lodBlend metres. ve::GrassLayer builds this from a glTF file
// with *_LOD0, *_LOD1... nodes.
// =============================================================================

#include "ve/render/Mesh.hpp"
#include "ve/render/RenderGraph.hpp"
#include "ve/render/Renderer.hpp"

#include <glm/glm.hpp>

#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace ve {

class Material;
class Mesh;
class Shader;
class Texture;

struct GrassLod {
    MeshData mesh;                     // empty = procedural blades; else root at origin, height 1 (like cardMesh)
    std::shared_ptr<Texture> texture;  // RGBA (alpha = coverage); null = white
    float maxDistance = FLT_MAX;       // camera distance up to which this LOD is drawn
    std::string fragmentShader;        // empty = grass_field.frag (blades) / grass_card.frag (meshes)
};

struct GrassDesc {
    std::string name = "Grass";
    glm::vec2 boundsMin{-256.0f}, boundsMax{256.0f};
    uint32_t resolution = 513; // height samples per side (include both edges)
    uint32_t maskResolution = 512;
    // > 0: the height / mask textures cover only a window this wide around
    // the camera, re-baked on a worker thread as it moves (any world size).
    // Keep it >= 4 x radius. 0 = the whole bounds, baked once.
    float window = 0.0f;
    // Sampled into the textures (whole bounds: once in setup(), then released
    // with whatever they capture; windowed: on a worker thread whenever the
    // window moves, so they must be thread-safe).
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

    // LOD chain, nearest first. Empty = one LOD from cardMesh / cardTexture /
    // fragmentShader above (procedural blades if cardMesh is empty).
    std::vector<GrassLod> lods;
    float lodBlend = 4.0f; // m over which neighbouring LODs are dithered
};

class GrassPass : public RenderPass, public ShadowCaster {
public:
    explicit GrassPass(GrassDesc desc);
    ~GrassPass() override;

    void setup(RenderGraph& graph) override;
    void prepare(FrameData& frame) override; // window streaming + material, before the shadow and colour draws
    void declare(PassBuilder& builder) override;
    void execute(RenderContext& ctx) override;
    void onGui() override;
    void drawShadow(RenderContext& ctx, const ShadowView& view) override;

    // Parameters are live: change them any time (LOD meshes / shaders excepted).
    GrassDesc& desc() { return m_desc; }
    Material& material(size_t lod = 0) { return *m_lods.at(lod).material; }
    size_t lodCount() const { return m_lods.size(); }

private:
    // Height + mask samples and per-tile height ranges for one area.
    struct Baked {
        glm::vec2 lo{0.0f}, size{0.0f}; // world area covered
        uint32_t res = 0, maskRes = 0;
        std::vector<float> heights;
        std::vector<uint8_t> mask;
        std::vector<glm::vec2> tileHeights; // min/max ground height per tile
        int tilesX = 0, tilesZ = 0;
    };
    static Baked bake(const GrassDesc& d, glm::vec2 lo, glm::vec2 size, uint32_t res, uint32_t maskRes);
    void apply(Baked&& baked); // upload the textures, switch to the new area
    glm::vec2 windowFor(glm::vec2 eye) const; // window origin for a camera position

    struct Lod {
        std::shared_ptr<Mesh> mesh;
        std::shared_ptr<Shader> shader;
        std::shared_ptr<Material> material;
        bool cards = false;
        float minDistance = 0.0f, maxDistance = FLT_MAX;
        uint32_t clumps = 0;
    };
    void updateMaterial();
    MeshData bladeClump() const;
    // Draws the visible tiles of one LOD; returns {tiles, clumps}.
    glm::uvec2 drawTiles(RenderContext& ctx, const Lod& lod, const Frustum& frustum, float maxDistance, uint32_t viewIndex);

    GrassDesc m_desc;
    std::shared_ptr<Texture> m_height, m_mask;
    std::vector<Lod> m_lods;
    Baked m_area;                      // what the textures hold now (samples freed after upload)
    float m_windowSize = 0.0f;         // 0 = whole bounds
    std::future<Baked> m_pending;      // a window being re-baked
    uint32_t m_lastTiles = 0, m_lastClumps = 0, m_shadowClumps = 0, m_lastShadowClumps = 0;
    bool m_registered = false;
};

} // namespace ve
