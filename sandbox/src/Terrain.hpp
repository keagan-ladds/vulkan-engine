#pragma once

// =============================================================================
// Terrain - procedural heightfield world for the test scene.
//
// Generated from noise: rolling savannah, a swamp basin with pools, granite
// "kopje" outcrops and border mountains. It's split into chunks (separate
// meshes) so the renderer can frustum-cull them, and rendered with a custom
// shader (terrain.vert/frag) to demonstrate per-material shaders.
//
// Game code can query height()/normal()/biome masks, e.g. to place trees or
// keep the player on the ground.
// =============================================================================

#include <ve/scene/GameObject.hpp>

#include <glm/glm.hpp>

#include <memory>
#include <vector>

namespace ve {
class Material;
}

namespace savanna {

class Terrain : public ve::GameObject {
public:
    static constexpr float kHalfSize = 320.0f;  // world spans [-320, 320] on X and Z
    static constexpr float kCellSize = 2.0f;    // meters between height samples
    static constexpr int kChunkCells = 32;      // cells per chunk side (64 m)
    static constexpr float kWaterLevel = 0.0f;

    // Points of interest.
    static constexpr glm::vec2 kCamp{20.0f, 10.0f};
    static constexpr glm::vec2 kSwamp{-130.0f, 110.0f};
    static constexpr glm::vec2 kKopje{120.0f, -80.0f};

    Terrain();          // computes the heightfield (usable immediately)
    void start() override; // creates the chunk meshes/objects

    // Exact height of the rendered surface (bilinear between samples).
    float height(float x, float z) const;
    glm::vec3 normal(float x, float z) const;
    float slope(float x, float z) const { return 1.0f - normal(x, z).y; }
    float swampMask(float x, float z) const;
    float rockMask(float x, float z) const;
    bool inBounds(float x, float z, float margin = 0.0f) const;

    // Raw procedural function (unsampled).
    static float heightFunction(float x, float z);

private:
    void buildMeshes();
    glm::vec4 colorAt(float x, float z, float h, const glm::vec3& n) const;

    int m_res = 0; // samples per side
    std::vector<float> m_heights;
    std::shared_ptr<ve::Material> m_material;
};

} // namespace savanna
