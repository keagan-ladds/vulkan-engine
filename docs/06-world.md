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
| W4 | Data-driven vegetation: `FoliageLayer` and `GrassLayer` components (meshes by asset path with manual LOD chains, or a code-registered `MeshLibrary` entry; per-biome density; spacing, scale, colliders...). GrassPass gets an LOD chain (near mesh -> mid mesh -> cards) | planned |
| W5 | Savanna moved onto all of it: terrain + biomes + vegetation in `scenes/savanna.json`, procedural tree meshes registered in the MeshLibrary | planned |

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
- **Known gap until W4:** vegetation is generated once from the terrain at
  startup, so terrain edits don't move trees or grass yet.

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

**Vegetation.**
- Layers are components on child objects of a `Foliage` / `GrassField` object.
  They reference meshes by:
  - a glTF path. Nodes named `*_LOD0`, `*_LOD1`... (or `LOD0`...) form the LOD
    chain; otherwise the first mesh node is used.
  - or a `MeshLibrary` name, for meshes a game generates in code (Savanna's
    procedural trees).
- Density per biome: `{"Savannah": 1.0, "Swamp": 0.2}`.
- Optional impostor as the last LOD.
- Alpha-masked glTF materials become cutout materials automatically, so grass
  cards just work.
