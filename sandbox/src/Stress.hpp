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

// SAVANNA_BOUNCETEST=1: a 4 x 2 m grey wall at (36, 4) with a 1 m cube 2.6 m
// in front of each face. The face turned away from the sun must be evenly lit
// by the ambient: no cube-shaped blue patch (ground-bounce visibility,
// ve_lighting.glsl). Seen from SAVANNA_POSE=36,2.2,10,0,-12 (south face) or
// SAVANNA_POSE=36,2.2,-2,180,-12 (north face).
void spawnBounceTest(ve::Scene& scene, const ve::Terrain& terrain);

// SAVANNA_LEAFTEST=1: two leaf-green cards at (40, 4) facing +Z (south): the
// left (west) plain, the right materials/leaf_test.material (translucency
// 0.5), and a warm point light 1 m south of them. At night from behind
// (SAVANNA_TIME=22 SAVANNA_POSE=40,1.5,0.3,180,-5) only the translucent card
// glows; from the front (SAVANNA_POSE=40,1.5,7.5,0,-5) its front is a little
// darker (the light it lets through isn't also reflected).
void spawnLeafTest(ve::Scene& scene, const ve::Terrain& terrain);

} // namespace savanna
