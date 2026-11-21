#pragma once

// Stress test hooks (benchmarks), SAVANNA_STRESS=<what>:<count>.
//
//   SAVANNA_STRESS=pieces:3000   3000 building pieces (mixed kinds and
//                                materials, static) in a grid south of the
//                                camp, starting right by the campfire.
//
// A "[stress]" log line every 30 frames reports the draw calls, so A/B runs
// (e.g. RendererSettings::staticBatching, SAVANNA_STATIC=0) can be compared
// next to the [bench] numbers.

#include <string_view>

namespace ve {
class Scene;
class Terrain;
} // namespace ve

namespace savanna {

void spawnStress(ve::Scene& scene, const ve::Terrain& terrain, std::string_view spec);

} // namespace savanna
