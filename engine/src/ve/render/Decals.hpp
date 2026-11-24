#pragma once

// =============================================================================
// Decals - textures projected onto the scene's surfaces: bullet holes,
// puddles, cracks in walls, scorch marks, dirt, painted signs.
//
// A decal is an oriented box. It projects along its local -Z (like a light,
// it "shines" along forward()) onto every surface inside the box, and can
// change that surface's albedo, normal, roughness and metallic, each with its
// own opacity, all multiplied by the decal's alpha (colour alpha, albedo
// texture alpha, mask alpha and the fades). The box's size is its transform's
// scale: x = width, y = height (texture up = +Y), z = projection depth.
//
// Two ways to place one:
//   * ve::DecalProjector (scene/Decal.hpp): a component. Placed in the editor,
//     saved in scene files and prefabs, follows its object (cracks on a wall,
//     a puddle, a sign).
//   * renderer.decals().spawn(...): fire-and-forget decals without a
//     GameObject (bullet holes, footprints, blood splats): a ring buffer that
//     replaces the oldest when full, with optional lifetime + fade-out.
//
//   ve::Decal hole;
//   hole.albedoTexture = assets.loadTexture("textures/decals/bullet_hole.png");
//   hole.normalTexture = assets.loadTexture("textures/decals/bullet_hole_n.png"); // *_n: a normal map
//   hole.roughness = 0.9f; hole.roughnessOpacity = 1.0f;
//   if (auto hit = physics.raycast(eye, dir, 100.0f))
//       renderer.decals().spawn(hole, ve::DecalSystem::onSurface(hit->point, hit->normal, {0.12f, 0.12f}, 0.1f,
//                                                               randomAngle), 30.0f /* seconds */);
//
// Which surfaces receive decals: shaders that call veApplyDecals
// (standard.frag: StandardMaterial, terrain.frag: Terrain; your own shaders,
// see ve_decals.glsl). A decal applies where (decal.layers & the material's
// receive layers) != 0, e.g. keep puddles off characters with
// StandardMaterial::setDecalLayers(~kDecalLayerWorld) on the character's materials.
//
// How it works: each frame the decals in view (the nearest kMaxDecals) are
// sorted by sortOrder (then age: newer on top), written to the DecalBuffer
// (set 0, binding 14) and binned into the light cluster grid; material shaders
// loop over their cluster's decals before lighting. No extra pass, no
// G-buffer, and transparent materials can receive decals too.
// =============================================================================

#include "ve/render/GpuTypes.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace ve {

class Texture;

// Decal layer bits (any 32-bit mask works; these are the engine's conventions).
constexpr uint32_t kDecalLayerWorld = 1u << 0;     // default: terrain, buildings, props
constexpr uint32_t kDecalLayerCharacters = 1u << 1;
constexpr uint32_t kDecalLayerAll = 0xFFFFFFFFu;

enum class DecalBlend {
    Normal,   // albedo = mix(surface, decal, opacity)
    Multiply, // albedo = surface * mix(1, decal, opacity): darkening (wet patches, dirt, soot)
};

// What a decal looks like. Shared by DecalProjector (component) and DecalSystem.
struct Decal {
    // Optional textures. The normal and mask textures must load as Normal /
    // Data (TextureSettings.hpp: *_normal / *_mask names or a .texture file).
    std::shared_ptr<Texture> albedoTexture; // rgb colour, a = opacity
    std::shared_ptr<Texture> normalTexture; // tangent space (glTF / OpenGL convention: +Y = up in the image)
    std::shared_ptr<Texture> maskTexture;   // G = roughness, B = metallic multipliers (glTF style), A = opacity

    glm::vec4 color{1.0f};               // linear tint (multiplies the albedo texture), a = overall opacity
    DecalBlend blend = DecalBlend::Normal;
    float albedoOpacity = 1.0f;          // 0 = leave the surface's colour alone (e.g. a normal-only crack)
    float normalOpacity = 1.0f;          // how strongly the normal texture replaces the surface's normal
    float normalStrength = 1.0f;         // scales the normal texture's bumps
    float flatten = 0.0f;                // smooths out the surface's own bumps (1 = flat: standing water)
    float roughness = 0.5f;              // (times mask G)
    float roughnessOpacity = 0.0f;       // 0 = keep the surface's roughness
    float metallic = 0.0f;               // (times mask B)
    float metallicOpacity = 0.0f;        // 0 = keep the surface's metallic
    // Surfaces turned away from the projection direction fade out between
    // these angles (degrees between the surface normal and the box's +Z):
    // keeps the decal from smearing down the sides of things it hits.
    float angleFadeStart = 60.0f;
    float angleFadeEnd = 80.0f;
    float depthFade = 0.3f;              // fraction of the box's depth over which it fades out at the front/back
    float drawDistance = 120.0f;         // not drawn beyond this camera distance (fades out over the last 15%)
    int sortOrder = 0;                   // higher draws over lower (then newer over older)
    uint32_t layers = kDecalLayerWorld;  // receivers: see the header comment

    // The GPU form for a box with this world matrix (scale = size).
    // `opacity` multiplies color.a (distance / lifetime fades).
    void toGpu(const glm::mat4& world, float opacity, GpuDecal& out) const;
};

// Fire-and-forget decals (Renderer::decals()). Fixed capacity: when full,
// spawning replaces the oldest one.
class DecalSystem {
public:
    using Handle = uint64_t; // 0 = none; a handle of a replaced / removed decal is ignored
    static constexpr Handle kNone = 0;

    // A world matrix for a decal on a surface: centred on `point`, projecting
    // into the surface along -normal, `size` wide/high, `depth` deep (half of
    // it above the surface, so bumps aren't cut off), rotated `spin` radians
    // about the normal.
    static glm::mat4 onSurface(const glm::vec3& point, const glm::vec3& normal, const glm::vec2& size,
                               float depth = 0.25f, float spin = 0.0f);

    // `lifetime` seconds (0 = until replaced or removed), then fades out over
    // `fadeOut` seconds. The decal keeps its textures alive.
    Handle spawn(const Decal& decal, const glm::mat4& world, float lifetime = 0.0f, float fadeOut = 1.0f);
    void remove(Handle handle);
    void clear();
    void setTransform(Handle handle, const glm::mat4& world); // e.g. a decal stuck to a moving object
    // Default 512. Shrinking drops the oldest.
    void setCapacity(uint32_t capacity);
    uint32_t capacity() const { return m_capacity; }
    uint32_t count() const { return m_live; }

    // --- Engine internal (SceneGatherer) -----------------------------------------
    struct Entry {
        Decal decal;
        glm::mat4 world{1.0f};
        float spawnTime = 0.0f; // game time; < 0 until the first update() sees it
        float lifetime = 0.0f;
        float fadeOut = 1.0f;
        uint64_t serial = 0;    // spawn order (handle); 0 = free slot
    };
    // Expires finished decals; `time` = game time in seconds.
    void update(float time);
    const std::vector<Entry>& entries() const { return m_entries; }
    // Lifetime fade of an entry at `time` (1 = fully visible).
    static float lifeFade(const Entry& e, float time);

private:
    Entry* find(Handle handle);

    std::vector<Entry> m_entries;
    uint32_t m_capacity = 512;
    uint32_t m_next = 0; // ring position
    uint32_t m_live = 0;
    uint64_t m_serial = 0;
    float m_time = 0.0f;
};

} // namespace ve
