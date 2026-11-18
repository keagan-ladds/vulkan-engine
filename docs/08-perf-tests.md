# Performance tests

Tests that need a real GPU. The software renderer used for automated checks
(lavapipe) can't show these effects: it has no registers or wave occupancy,
and no hardware early depth test.

## Standard shader: one shader for every feature?

`standard.frag` is one shader for every `StandardMaterial`. Optional features
are runtime branches on material data: textures present or not, alpha cutoff,
translucency. A draw never takes the branches it doesn't use, but three costs
could remain. Each question below has a proposed fix, and the test decides
whether the fix is worth making.

| # | Question | Measured by | If yes |
|---|---|---|---|
| Q1 | What does translucency (backlit leaves) cost when it is on? | Same build, `SAVANNA_BACKLIT=1` vs `0`, in leafy views | Keep the setting. Consider a cheaper variant (no second shadow lookup) for low quality. |
| Q2 | Do materials that don't use translucency still pay for its code? The compiler sizes registers for the worst path, which lowers occupancy for every draw. | Translucency off at runtime vs **compiled out** (`VE_PERF_NO_TRANSLUCENCY`) | Make translucency a specialization constant / separate pipeline. |
| Q3 | Does the alpha-cutout `discard` in the shared shader slow opaque materials? A shader that can `discard` may lose the early depth test, and the engine has no depth pre-pass. | Opaque-only scene (no Foliage object), `discard` present vs **compiled out** (`VE_PERF_NO_DISCARD`) | Give opaque materials a shader without `discard`; cutout keeps its own. |

The defines live in `standard.frag` (`#ifndef VE_PERF_...`) and exist only for
this test. With one set, the output is identical for materials that don't use
the feature. In Q3 that holds because the scene has no cutout materials.

### What the script does

`tools/perf/shader_variants.sh <Release bin dir> <output dir>`:

1. Copies the bin dir twice and recompiles `standard.frag` in each copy with
   one define. It uses the build's own glslc command (`shaders/standard.frag.spv.cmd`),
   so there are no extra builds and the C++ is identical.
2. Writes `savanna-noleaves.json`: the Savanna scene minus its Foliage object
   (huts, villagers, rocks, terrain, grass).
3. Runs Savanna's benchmark mode in each case. It runs headless, at a fixed
   resolution, with a fixed time step, clear weather and a fixed camera:

   | view | camera | what it stresses |
   |---|---|---|
   | `camp-sun` | camp at 17:00, facing the low sun | acacia leaves, backlit |
   | `jungle` | jungle at 11:00 | dense broadleaf canopy |
   | `vista` | high wide view at 10:00 | many trees at LOD / impostors |
   | `camp-noleaves` | the camp, no Foliage object | opaque StandardMaterials only |

   For each view it runs the following (`nodiscard` only in `camp-noleaves`):
   - `base-on` (translucency on);
   - `base-off`;
   - `notrans`;
   - `nodiscard`.

   The variants are interleaved inside each repeat, so thermal or clock drift
   hits them evenly.
4. Writes `summary.md`, using medians over the repeats:
   - a table of total GPU time, the Opaque pass and the Shadows pass;
   - each question's comparison, with a verdict.

   A difference only counts when it exceeds both 2 % and twice the
   repeat-to-repeat spread.

The numbers are the engine's GPU timestamps per render pass (`VE_BENCH`). They
don't depend on vsync or the window, but they do depend on GPU clocks.

### Running it

Linux or Windows (Git Bash). Needs CMake + Ninja, the Vulkan SDK (glslc), and
Python 3 for the summary.

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
tools/perf/shader_variants.sh build-release/bin perf-results
```

About 10-15 minutes on a desktop GPU: 4 views × 3-4 variants × 3 repeats, a
few seconds of loading each. Before running:

- plug laptops in and use the high-performance power profile;
- close games, browsers with video and other GPU users;
- on laptops with two GPUs, make sure the discrete one is used. The log prints
  `GPU: <name>` and `summary.md` repeats it.

Options (environment variables):

| variable | default | |
|---|---|---|
| `PERF_RESOLUTION` | `2560x1440` | GPU-bound on purpose; use your monitor's resolution |
| `PERF_FRAMES` / `PERF_WARMUP` | `300` / `150` | measured / skipped frames per run |
| `PERF_REPEATS` | `3` | use 5 if the spread in the table is above ~3 % |
| `PERF_SCENES` | all four | e.g. `"camp-noleaves"` to rerun Q3 only |

`VE_RESOLUTION=WxH` works for any engine program, not only this test.

To run a case by hand, e.g. to look at it in RenderDoc or Nsight:

```bash
cd build-release/bin
VE_RESOLUTION=2560x1440 SAVANNA_TIME=17 SAVANNA_POSE=20,3,-5,90,10 SAVANNA_WEATHER=0 SAVANNA_BACKLIT=1 ./savanna
```

### Optional: register counts

Q2 is about register pressure, which you can also read directly. Capture a
`base-off` frame and compare `standard.frag`'s register count against the
`notrans` copy (`perf-results/bin-notrans/shaders/standard.frag.spv`):

- **AMD:** Radeon GPU Analyzer
  (`rga -s vk-spv-offline --frag standard.frag.spv`, VGPR count), or on Linux
  `RADV_DEBUG=shaderstats`.
- **NVIDIA:** Nsight Graphics, Shader Profiler, "Registers".
- **Intel:** Graphics Frame Analyzer.

A lower count for `notrans` is worth knowing even if the timings are within
noise; the gap only grows as features are added.

### Reporting back

Send `perf-results/summary.md`, plus the `logs/` folder if anything looks
odd. What happens next:

| result | next step |
|---|---|
| Q3 yes | Split opaque and cutout into two shaders (Material modes already choose separate pipelines). |
| Q2 yes | Turn translucency into a specialization constant; the renderer setting then switches pipelines. |
| Q1 large (above ~10 % of the Opaque pass in leafy views) | A cheaper path: reuse the front shadow lookup and drop the forward-scatter lobe at low quality. |
| all no | Keep the single shader; this test stays as the check to rerun when more features are added. |

A software-renderer dry run (`PERF_FRAMES=2 PERF_REPEATS=2
PERF_RESOLUTION=640x360` under lavapipe) only checks that the pipeline works.
Its numbers are noise.
