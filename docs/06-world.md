# World as data: terrain, biomes, foliage, grass, weather (Phase W)

Goal: everything that makes up a level can be edited per scene in the editor:
terrain shape, biomes, what grows where, weather and time of day. A new scene
starts empty; you add a Terrain, stamps, biomes and vegetation from the
Create menu.

## Stages

| Stage | Contents | Status |
|---|---|---|
| W1 | Weather, DayNightCycle, Water as registered scene objects (effects module) | ✅ |
| W2 | `ve::Terrain`: procedural heightfield (seed, noise layers, border mountains), chunk meshes, heightfield collider, `TerrainStamp` children (flatten / raise / lower, movable with the gizmo), rebuild when edited | ✅ |
| W3 | Biomes as data on the Terrain: ground colours, where they occur (height / slope / noise patches), biome stamps; per-point biome weights blended at borders | ✅ (rules + stamp painting; vegetation still reads them from code) |
| W4 | Data-driven vegetation: `Foliage` + `FoliageType` layers, `GrassLayer`, `FoliageExclusion` (meshes from glTF with manual LOD chains, or a code-registered `MeshLibrary` entry; per-biome density; spacing, scale, colliders...). GrassPass LOD chain (near mesh -> mid mesh -> cards). Regenerates when the terrain changes | ✅ |
| W5 | Savanna moved onto all of it: terrain + biomes + vegetation in `scenes/savanna.json`, procedural tree meshes registered in the MeshLibrary | ✅ |

## Notes from W1-W3
- **Savanna's world is now data.** `scenes/savanna.json` holds the Terrain
  with four biomes (Savannah, Swamp, Camp, Rocky) and four stamps (Swamp basin
  with noisy pools, Kopje, Small kopje, Camp clearing), plus Water, Weather and
  DayNightCycle. The old `sandbox/src/Terrain.*` is gone.
- **Terrain queries are snapshot based.** `Terrain::data()` returns an
  immutable `TerrainData`; worker threads keep their copy alive. Game code
  should look the terrain up (`Terrain::find(scene)`) rather than holding a
  reference: the editor can replace scene objects (Open scene).
- **Play/Stop restores in place:** existing objects get their saved state
  re-applied, deleted ones are re-created and new ones removed.

## Notes from W4-W5
- **Vegetation is scene data.** Savanna's `scenes/savanna.json` now holds a
  `Foliage` object with six `FoliageType` children (Baobabs, Acacias, Fever
  trees, Boulders, Rocks, Bushes), `GrassLayer`s for grass and reeds, and
  `FoliageExclusion`s on the camp clearing and the huts. Only the procedural
  meshes stay in code (`registerPropMeshes()` -> `ve::MeshLibrary`).
  Placement matches the old code rules closely (9466 vs 9459 instances, same
  colliders).
- **Everything follows the terrain.** Foliage and grass rebuild (throttled)
  when the terrain's `version()` changes, when their settings change, or when
  an exclusion moves. The Terrain itself now rebuilds only when its fields,
  position or stamps actually change (Play/Stop no longer rebuilds it).
- **Headless runs for tests.** `VE_HEADLESS=1` renders offscreen with a hidden
  window; `VE_CAPTURE=out.png [VE_CAPTURE_FRAME=N]` saves a frame.
  `SAVANNA_SCENE=path.json` loads another scene file.

## Authoring LOD chains in Blender
Export glTF (`.glb` / `.gltf`, +Y up) with one object per level:

| Object name | Becomes |
|---|---|
| `Grass_LOD0`, `Grass_LOD1`, `Grass_LOD2` | the levels of variant "Grass", nearest first |
| `LOD0`, `LOD1`... | the same, for a file with one variant |
| an empty named `Tree_LOD0` with child meshes | one level; the children are merged |
| `Tree_COL` / `COL` / `UCX_Tree` | collision (its convex hull) for that variant / all variants; not drawn |
| `Rock_A`, `Rock_B` (no LOD names) | one variant each, a single level |

- A level's position is ignored (its rotation and scale are kept), so you can
  lay the levels out side by side in Blender. Blender's `.001` suffixes are
  ignored.
- Separate files work too: `mesh = "grass_lod0.glb; grass_lod1.glb"`.
- **Grass** (`GrassLayer::mesh`): the root at the origin. Each level is scaled
  so LOD0 is 1 unit tall, then the layer's `minHeight..maxHeight` sets the real
  height. The base colour texture's alpha is cut out at `alphaCutoff`, so a card
  level just works. Base colour factors and vertex colours tint the mesh, and
  the vertex height (0..1) is the wind sway weight. Switch distances are
  `lodDistances` (m), dithered over `lodBlend`.
- **Foliage** (`FoliageType::mesh`): real size. `lodMode` is screen size or
  distance, with `lodThresholds`. `impostor` adds a baked octahedral impostor as
  the last level, and `autoLods` simplifies one-level meshes. `collision`:
  Auto (the `_COL` nodes), a primitive sized by `colliderSize`, or the convex
  hull of LOD0.
- The log says what was found, e.g. `FoliageType 'Plants': 1 variant(s) from
  plant.glb, 3 level(s) + impostor, 1 authored collider(s)`.

## Design notes

**Terrain.**
- `ve::Terrain` is a GameObject class (registered).
- Height = base noise (fbm) + secondary hills + border mountains + stamps,
  applied in order:
  - Flatten: blend towards a height.
  - Raise: dome or ridged hill.
  - Lower: basin down to a height, with noisy pools.
- Each stamp has a radius and a falloff, and can force a biome.
- Stamps are `TerrainStamp` components on any GameObject, usually children of
  the terrain, so you place them with the gizmo.
- The terrain watches its parameters and its stamps' transforms, and rebuilds
  (throttled) when they change. Dependent systems (foliage, grass) see the
  terrain's `version()` change and regenerate.

**Biomes.**
- A biome has a name, a ground colour (+ variation colour), and rules: height
  range, max slope, noise frequency/coverage (patches), priority.
- Weight for a point = the strongest matching rule, softened at the edges, or a
  stamp forcing it. Biome 0 is the default (fills the rest).
- Steep slopes show a "rock" colour regardless of biome; near the water line
  the ground goes to mud.
- `Terrain::biomeWeights(x, z)` gives the per-biome weights to anyone
  (foliage, grass, gameplay).

**Vegetation** (`ve/world/Vegetation.hpp`, `LodModel.hpp`).
- Foliage layers are `FoliageType` components on child objects of a `Foliage`
  object (the object name is the layer name, used by `avoid = "Acacias:2.5"`).
  Each `GrassLayer` object is one GPU grass field. They reference meshes by:
  - a glTF path. Nodes named `*_LOD0`, `*_LOD1`... (or `LOD0`...) form the LOD
    chain; otherwise the first mesh node is used.
  - or a `MeshLibrary` name, for meshes a game generates in code (Savanna's
    procedural trees).
- Density per biome: `biomes = "Savannah:1, Swamp:0.2"`. Negative values
  push a layer out of a biome, and `*` is a constant (weights add up to 1), so
  `"*:1, Rocky:-1.2, Swamp:-0.8"` means everywhere except rocks and swamp.
  `biomeThreshold > 0` turns the density into all-or-nothing.
- Optional impostor as the last LOD.
- Alpha-masked glTF materials become cutout materials automatically, so grass
  cards just work.
- `FoliageExclusion` (radius, falloff, foliage / grass) on any object keeps
  vegetation out: the camp, inside the huts.
