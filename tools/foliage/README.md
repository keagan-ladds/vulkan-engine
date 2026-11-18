# Foliage generator

Procedural trees and plants for the engine, written out as model files
(`sandbox/assets/models/foliage/*.gltf`) that the `FoliageType` layers load
like any authored model: LOD levels, collision hulls, an impostor baked by
the engine, and a stump model for the chopped state.

| Species | File | Biome | Notes |
|---|---|---|---|
| Umbrella thorn acacia | `acacia.gltf` (+ `acacia_stump.gltf`) | savannah, camp | short trunk, 2-4 leaning leaders, flat layered crown |
| Fever tree | `fever_tree.gltf` (+ stump) | swamp | tall yellow trunk, ascending limbs, airy crown |
| Baobab | `baobab.gltf` (+ stump) | savannah | massive trunk, stubby root-like limbs, sparse leaves |
| Savanna bush | `bush.gltf` | savannah | multi-stemmed, dense, no collision |
| Jungle tree | `jungle_tree.gltf` (+ stump) | jungle | tall trunk, buttress roots, broad canopy |
| Palm | `palm.gltf` (+ stump) | jungle, swamp | curving ringed trunk, arching fronds |
| Fern | `fern.gltf` | jungle undergrowth | rosette of arching fronds |

## Files

- `textures.py` - leaf-card atlases (2x2 sprays per species, alpha with the
  colour bled past the edges) and tileable bark (albedo + normal), drawn
  procedurally with numpy / Pillow. Output: `sandbox/assets/textures/foliage/`.
- `treegen.py` - the generator (pure Python + numpy): skeletons, bark meshes,
  leaf cards, LODs, colliders, stumps, and a glTF writer. Runs anywhere.
- `blender_foliage.py` - a Blender add-on (and headless batch mode) on the
  same generator: builds editable objects with proper materials, exports glTF.
- `preview_blender.py` - renders a quick Cycles preview of a generated file.

## Workflows

Regenerate everything the game uses (no Blender needed):

    python3 tools/foliage/textures.py sandbox/assets/textures/foliage
    python3 tools/foliage/treegen.py sandbox/assets/models/foliage            # all species
    python3 tools/foliage/treegen.py sandbox/assets/models/foliage acacia     # one

In Blender (to look at, tweak or hand-edit trees):

1. Edit > Preferences > Add-ons > Install... > `tools/foliage/blender_foliage.py`
   (keep it next to `treegen.py`), enable "vulkan-engine Foliage".
2. 3D view sidebar (N) > Foliage: pick a species, seeds (one variant per
   seed, e.g. `1, 2, 3`), Generate. You get `<Variant>_LOD0/1/2` objects
   (LOD1/2 hidden) and a wireframe `<Variant>_COL` per variant, with bark and
   leaf materials (alpha clip, double-sided leaves, bark normal map, vertex
   colours for ambient occlusion).
3. Edit freely: sculpt the trunk, move / delete cards, delete a variant.
   Keep the names (`_LOD<n>`, `_COL`); Blender's `.001` suffixes are fine.
4. Export for the engine: writes `<species>.gltf` into the models folder,
   pointing at the shared textures (not copies).

Headless, e.g. in a build script:

    blender -b -P tools/foliage/blender_foliage.py -- --species acacia --seeds 1,2,3,4 --out sandbox/assets/models/foliage --blend acacia.blend
    blender -b -P tools/foliage/preview_blender.py -- sandbox/assets/models/foliage/acacia.gltf preview.png 0 16

(Ubuntu's Blender package needs `python3-numpy`; the official builds bundle it.)

## How a tree is generated

1. **Skeleton.** An explicit trunk (and per species: leaders, limbs, a
   bottle profile, buttresses), then the crown filled by *space
   colonisation*: attraction points are sampled in the species' crown
   envelope (an umbrella lens for acacias, an ellipsoid shell for fever trees,
   a dome for jungle trees); every step each point pulls the nearest branch
   node, which grows towards the average pull, and points that are reached
   are used up. Branches fill the shape without crossing.
2. **Radii** from the pipe model (a parent's cross-section is about the sum
   of its children's), rescaled so the trunk has the species' size while the
   twigs stay thin; chains smoothed, long thin branches sag a little.
3. **Bark**: each branch is a chain from its fork to its tip with rings of
   vertices along it (parallel-transported frames: no twisting), sides by
   radius, UVs at a constant texel density, a closed tip; trunk flare and
   buttress fins come from per-angle radius functions.
4. **Leaves**: alpha-tested cards (sprays from the atlas, attachment point at
   the bottom centre) along the last metre or so of every twig, oriented per
   species (nearly horizontal layers for acacias). Normals bend outwards
   from the crown centre for soft, volumetric shading; vertex colours darken
   cards deep in the crown (ambient occlusion).
5. **LODs**: thin branches dropped and fewer sides; fewer cards (the outer
   ones), scaled up to keep the coverage. The engine adds an impostor.
6. **Collision**: the trunk's convex hull (`_COL`). **Stump**: the trunk cut
   at 0.6 m with a pale cut face (the `Stump` foliage state).

## Tuning / adding a species

Every species is one function in `treegen.py` decorated with `@species(name,
bark=..., leaves=...)`: change its numbers (heights, radii, crown shape,
attraction point counts, card size / reach) or copy one. New leaf or bark
looks are entries in `textures.ATLASES` / `textures.BARKS`. Then point a
`FoliageType`'s `mesh` at the file (and `states: "Stump = <file>_stump.gltf"`).

Budgets (LOD0 / LOD1 / LOD2 triangles): acacias ~11k / 4k / 1.1k, jungle
trees ~13k / 3.5k / 1k, palms ~0.9k / 0.5k / 0.2k. The big trees are wide, so
their FoliageTypes switch LODs earlier (`lodThresholds` 0.28 / 0.1 / 0.035
screen fraction instead of the default 0.10 / 0.035 / 0.012): that halved the
camp view's frame time on the software renderer (4.8 s -> 2.3 s; the old
procedural trees: 1.1 s). Alpha-tested cards in the shadow maps are now the
largest cost (fewer, bigger cards in the shadow LODs would be next).

Not yet: wind sway (no foliage wind in the engine's standard shader), seasonal
variants (bare trees), grass / reeds as models.
