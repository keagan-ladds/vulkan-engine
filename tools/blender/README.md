# Blender tools

## `savanna_mountains.py`: procedural savanna mountains with LODs

Generates large backdrop landforms like the sandstone massif in the Savanna
concept art: sheer banded cliffs, cliff tiers with benches, a cragged summit
and a scree apron with fallen boulders. Each mountain is exported as one
glTF/GLB with a LOD chain, and optionally a collision proxy.

| Preset | What | Footprint, summit | LOD triangles | Time |
|---|---|---|---|---|
| `mesa`  | Hero massif: two tiers of fluted sandstone cliffs, cragged top, scree apron with boulders | 1.1 x 0.9 km, ~390 m | 160k / 50k / 16k / 5k | ~25 s |
| `butte` | A single smaller stack | 440 x 380 m, ~170 m | 80k / 25k / 8k / 2.5k | ~10 s |
| `ridge` | A long, jagged range for the far distance | 3.8 x 1.2 km, ~500 m | 160k / 50k / 16k / 5k | ~40 s |
| `kopje` | A granite boulder pile | 160 x 130 m, ~35 m | 40k / 12k / 4k / 1.2k | ~7 s |

(Footprints include the apron; times are generation only, on 4 CPU cores.)

Every seed gives a different mountain.

### Running it

From the command line (tested with Blender 5.3; written to also run on 3.6+ but not tested there; nothing else to install):

```bash
blender --background --python tools/blender/savanna_mountains.py -- \
    --preset mesa --seed 7 --out sandbox/assets/models/mountain_mesa.glb --preview mesa.png

# every preset, 3 seeds each
blender -b -P tools/blender/savanna_mountains.py -- --preset all --count 3 --out-dir out/

# tweak any parameter (see --list for all of them, per preset)
blender -b -P tools/blender/savanna_mountains.py -- --preset mesa --seed 3 \
    --set height=380 --set tiers=3 --set strata_spacing=11 --lods 200000,60000,20000,6000 --out tall.glb
```

The script also runs with the `bpy` pip module (`pip install bpy`, which
needs the Python version that bpy release was built for):
`python tools/blender/savanna_mountains.py --preset mesa --out mesa.glb`.

Inside Blender, open the file in the **Text Editor**, edit `UI_CONFIG` at the
top and press **Run Script**. The levels appear side by side in a new
collection; export them yourself, or set `"out"`.

Options (after `--`):

| Option | Meaning |
|---|---|
| `--preset NAME` | `mesa`, `butte`, `ridge`, `kopje` or `all` |
| `--seed N`, `--count N` | first seed; how many variants per preset |
| `--lods a,b,c,...` | triangle budget per level (any number of levels) |
| `--set key=value` | override a parameter (tuples as `a,b`) |
| `--collision` | also build `<Name>_COL`, a coarse proxy (`collision_tris`) |
| `--out FILE` / `--out-dir DIR` | `.glb` or `.gltf` (+ `.bin`) |
| `--split` | also write one file per level (`<file>_lod0.glb`...) |
| `--blend FILE` | save the scene |
| `--preview FILE.png` | Cycles (CPU) renders: the levels side by side, plus `FILE_hero.png` |
| `--list` | print every preset's parameters |

### What's in the file

- Objects `Mesa_07_LOD0`, `Mesa_07_LOD1`... (`<Preset>_<seed>_LOD<n>`), all
  at the origin, Blender +Z up exported as glTF +Y up, metres. The base sits
  at z = 0 and the edges run `sink` metres below it, so the mountain beds into
  uneven terrain without gaps.
- Smooth normals. LOD1+ carry LOD0's normals (a data transfer), so lighting
  hardly changes when levels swap.
- `COLOR_0` vertex colours: banded rock, dark streaks on the cliffs, darker
  crevices, soil at the foot, dry scrub and bushes on flat ground. The
  material uses them as base colour (roughness 0.92).
- `TEXCOORD_0`: box projection in world metres (`uv_scale` metres per repeat),
  for tiling rock / soil textures. For a triplanar shader, ignore it.
- Optional `<Name>_COL`: a decimated copy for collision, without colours.

### How it's built

1. **Heightfield** (numpy): a squared-off ellipse outline, bent by noise into
   bays, promontories and buttresses, with a profile across it: domed and
   cragged summit, rounded rim, `tiers` near-vertical cliff bands with
   undulating benches between them, a concave scree apron, then ground
   settling below zero.
2. **Boulders**: noisy icospheres on the apron (or piled up, for kopjes).
3. **Voxel remesh** unions everything into one surface with even triangles,
   so the vertical cliffs get as much detail as the flat ground (a heightfield
   on its own would stretch them into slivers). The voxel size is chosen from
   the visible surface area and the LOD0 budget (`oversample`).
4. **3D detail** along the normals: strata whose ledges stick out (`strata_*`),
   vertical erosion ribs (`flute_*`) and rock noise (`detail_*`), on cliffs
   more than on flat ground.
5. **Vertex colours** from the same data (bands, steepness, cavities).
6. **LODs**: each level is decimated (collapse) from the dense mesh.

The noise is the script's own (numpy Perlin), so a seed gives the same
mountain on any Blender version.

### Making new kinds of mountain

Add an entry to `PRESETS` (only the keys that differ from `BASE`), or pass
`--set`. Useful knobs:

- Shape: `radius`, `elongation`, `height`, `squareness`, `lobe_amp`, `gully_amp`
- Cliffs: `tiers`, `tier_gap`, `cliff_width` (0.05 = sheer, 0.5 = slopes), `rim_round`
- Top: `dome`, `crown_amp`, `crown_freq`
- Apron: `talus_height`, `talus_width`, `boulders`, `boulder_size`
- Rock: `strata_spacing`, `strata_depth`, `flute_amp`, `detail_amp`
- Colour: `palette` (`sandstone`, `granite`; add more in `PALETTES`), `vegetation`
