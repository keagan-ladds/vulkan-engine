#pragma once

// World dressing for the example scene: GPU grass and streamed foliage (trees
// and rocks scattered by rules, with LODs, impostors and colliders). Both work
// on any terrain: here the "terrain" is the flat ground plane (height 0).

namespace ve {
class Renderer;
class Scene;
class GameObject;
class FoliageSystem;
} // namespace ve

// Grass around the camera everywhere except the play area (ve::GrassPass).
void addGrass(ve::Renderer& renderer);

// Trees + rocks via ve::FoliageSystem. Colliders follow `colliderFocus`.
ve::FoliageSystem& addFoliage(ve::Scene& scene, ve::Renderer& renderer, ve::GameObject& colliderFocus);
