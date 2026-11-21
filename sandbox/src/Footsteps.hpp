#pragma once

// Which footstep sound a physics surface makes (audio/footstep_<sound>_<n>.wav),
// for the player and the villagers alike. Terrain has no single surface: its
// biome under the feet decides (sand / bare ground in the desert and camp,
// rock in the rocky hills, grass elsewhere).

#include <ve/world/Terrain.hpp>

#include <glm/glm.hpp>

#include <string>

namespace savanna {

inline std::string footstepSound(const ve::Scene& scene, const std::string& surface, const glm::vec3& feet)
{
    if (surface == "Wood" || surface == "Thatch") return "wood";
    if (surface == "Stone" || surface == "Metal") return "stone";
    if (surface == "Mud") return "dirt";
    if (surface == "Terrain")
        if (const ve::Terrain* t = ve::Terrain::find(scene)) {
            if (t->biomeWeight("Desert", feet.x, feet.z) > 0.5f || t->biomeWeight("Camp", feet.x, feet.z) > 0.5f) return "dirt";
            if (t->biomeWeight("Rocky", feet.x, feet.z) > 0.5f) return "stone";
        }
    return "grass";
}

} // namespace savanna
