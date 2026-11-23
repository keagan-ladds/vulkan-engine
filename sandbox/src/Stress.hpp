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

// SAVANNA_PAINTTEST=1: a 4 x 2.5 m wall east of the camp using
// materials/painted_test.material (standard_painted.frag) with procedurally
// "painted" vertex colours: dirt rising from the bottom (R), worn patches (G),
// a wet stripe on the left (B). Seen from SAVANNA_POSE=33.1,3.2,10.6,30,-4.
void spawnPaintTest(ve::Scene& scene, const ve::Terrain& terrain);

} // namespace savanna
