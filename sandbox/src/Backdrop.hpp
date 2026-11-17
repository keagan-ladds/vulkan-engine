#pragma once

// Mountain backdrop (art test): a ring of distant mountains around the
// playable terrain, out to ~14 km. Never walked on, so it's just a mesh:
// no physics, no shadow casting, one draw per sector (sectors outside the
// view are culled). Shape: ridged noise turned into mesas with sheer walls,
// plus a few hand-placed hero massifs; vertex colours give sandstone strata,
// rock on cliffs and scrub on gentle slopes. backdrop.frag lights it like any
// surface, then hazes it towards the sky's own colour with distance (the
// volumetric fog only reaches ~1 km), which gives the layered depth.

#include <ve/scene/GameObject.hpp>

#include <glm/glm.hpp>

#include <memory>
#include <vector>

namespace ve {
class Material;
}

namespace savanna {

class MountainBackdrop : public ve::GameObject {
public:
    MountainBackdrop();
    void start() override;
    void lateUpdate(float dt) override;

    struct Massif {
        glm::vec2 center; // world xz
        float radius;     // m
        float height;     // m
    };

    float innerRadius = 2000.0f;  // m from the world centre (the terrain's border hills are just inside)
    float outerRadius = 14000.0f;
    float hazeDensity = 5.0e-5f;  // per metre at ground level
    float hazeFalloff = 1.0f / 1500.0f;
    std::vector<Massif> massifs;

private:
    float height(glm::vec2 p) const;
    std::shared_ptr<ve::Material> m_material;
};

} // namespace savanna
