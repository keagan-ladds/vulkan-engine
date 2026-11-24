# Changelog

What changed in each tagged version of vulkan-engine, for people building a
game on it. The rules for what may change between versions are in
[docs/09-api-stability.md](docs/09-api-stability.md).

Every breaking change to a stable API gets an entry under **Breaking**, with
what to change in game code. Games should pin a version (a tag) and update on
purpose: see "Pin an engine version" in [templates/game/README.md](templates/game/README.md).

## [Unreleased]

### Breaking
- **`AssetManager::loadTexture(path)` takes only the path.** How an image
  loads (colour / data / normal map, mips, compression, sampler) is now decided
  by the image: a settings file next to it (`rock.png.texture`) or its name
  (`*_normal` -> normal map, `*_rough` / `*_ao` / `*_orm` / `*_mask`... -> data,
  else colour). Migration: drop the extra arguments; images you loaded with
  `srgb = false` whose names don't say so need a `.texture` file
  (`{"type": "Data"}`, or `"Normal"`). Material files' extra textures lose
  their `"srgb"` field for the same reason.
- **Normal maps: use `veUnpackNormal(texel)`** in custom shaders instead of
  `texel.xyz * 2 - 1`. Cooked normal maps are BC5 (X and Y only); the helper
  rebuilds Z and gives the same result for uncooked maps.

### Added
- **Asset cooking (textures).** Images are cooked into GPU-ready KTX2 files
  (BC7 colour, BC5 normal maps, BC4 greyscale data, prebuilt mips) in
  `<exe>/cooked`: automatically on a worker thread the first time they load,
  used from the next run on (no decoding or mip generation at load, 4-8x less
  GPU memory). The `vecook` tool cooks everything ahead of time (releases, CI).
  `EngineConfig::cookCache` / `cookOnDemand`; env `VE_COOK=0|read|sync`,
  `VE_COOK_DIR`. Needs BC texture support (every desktop GPU); without it the
  source images are used.
- **`standard_painted.frag`**: the standard shader plus vertex-painted dirt
  (R), wear (G) and wetness (B), with optional triplanar dirt / grunge / wear
  textures; use it from a material file (`"fragmentShader"`). A placeholder
  to copy and adapt.
- **Shared textures and material files.** Image files referenced by glTF
  models now go through the AssetManager texture cache, so models exported
  next to each other share one GPU texture per file. New `*.material` files
  (`ve::MaterialAsset`, `assets().loadMaterial`) describe a StandardMaterial
  (textures, values, optional custom shader with extra texture / parameter
  slots); a glTF material uses `materials/<its name>.material` when it exists,
  or the file its `material` custom property names. One shared Material per
  file, reloaded live; editable in the editor (Assets > Materials).
  `AssetManager::materialFolder = ""` turns name matching off.
- **Decals.** `ve::DecalProjector` (component: editor Create > Decal, scene
  files, prefabs) and `Renderer::decals()` (`ve::DecalSystem`: transient
  decals such as bullet holes, with lifetime and fade-out) project textures
  onto surfaces and change their albedo (mix or multiply), normal, roughness
  and metallic, each with its own opacity times the decal's alpha. Applied
  before lighting by `standard.frag` and `terrain.frag`; custom shaders opt in
  with `veApplyDecals` (`ve_decals.glsl`). `StandardMaterial::setDecalLayers`
  chooses which decals a material receives (`params[3].w`, previously unused).
  New descriptor binding: set 0, binding 14 (`DecalBuffer`).
- **Prebuilt-engine package.** An engine build folder now exports a CMake
  package: a game can `find_package(VulkanEngine)` with `VulkanEngine_DIR`
  pointing at it and link the compiled engine instead of building it
  (`cmake/Package.cmake`; `ve_setup_game()` copies the engine's shaders).
- **Biome files.** A terrain biome can be its own `.biome` asset file
  (`{"asset": "biomes/x.biome"}` in the terrain's list) and lists the foliage
  (`.foliage`) and grass (`.grass`) files that grow in it. The new
  `BiomeVegetation` object grows them. See docs/06-world.md.
- **`ve::DataAssets`**: cached JSON asset files, reloaded when they change on
  disk, editable live and saved on demand.
- **Editor:** a Biomes list in the terrain's inspector, an asset window for
  `.biome` / `.foliage` / `.grass` files (live edits, Save / Revert) and an
  Assets menu. JSON fields in the inspector are no longer cut at 2 KB.
- Reflection can describe plain settings structs (`TypeInfo::isData`), e.g.
  `TerrainBiome`, `TerrainMaterial`.

### Fixed
- Vulkan validation error "SPIR-V Capability DemoteToHelperInvocation":
  the device now enables `shaderDemoteToHelperInvocation` (a Vulkan 1.3 core
  feature the shaders already used).
- Under the VS Code debugger on Windows with an NVIDIA GPU, compiling a new
  pipeline (e.g. giving a grass layer a model) took minutes and the game looked
  hung: gdb caught every JIT registration of the driver's shader compiler
  (16,000 for one grass shader), stopping all threads each time. The launch
  configurations (engine and game template) now run gdb with
  `set auto-solib-add off`; one new pipeline went from 40 s to under a second.

### Changed
- `Editor::drawField` takes the owning object as a pointer (null = asset file,
  no undo). Editor-internal; no game code calls it.
- The terrain's `biomes` field is hidden in the generic inspector (the Biomes
  section replaces the raw JSON box). Scene files are unchanged.

## [0.1.0] - 2026-11-22

The first tagged version: the baseline games build against.

### What's in it
- **Core:** the main loop with a fixed physics step, input actions and axes
  (keyboard, mouse, gamepads, rebinding), CPU profiler zones, one engine job
  pool, loading screens, environment switches (`ve::env`), benchmark and
  headless capture modes.
- **Scene:** GameObjects with components (Unity-style hybrid), lifecycle hooks
  that cost nothing unless overridden, typed handles for remembered objects
  (`ObjectRef`, `ComponentRef`), reflection for the editor and scene files,
  prefabs, save games, scene files.
- **Rendering (Vulkan 1.3):** bindless materials, a render graph, cascaded and
  point-light shadows with face caching, clustered lights, temporal AA, GPU
  instancing with LODs, impostors and occlusion culling, static renderers as
  GPU instances, GPU skinning, asynchronous buffer uploads, shader
  hot-reload, a pipeline cache.
- **Effects (optional module):** sky and volumetric clouds, volumetric fog,
  bloom, particles, fire, weather, day/night, water, colour grading,
  screen-space GI and reflections, depth of field, photo mode.
- **World:** streamed procedural terrain with biomes and stamps, foliage
  layers with promotion to interactive objects, GPU grass, LOD chain building.
- **Gameplay:** interaction ("press E"), building placement with snap points,
  placement boxes and fittings, marker nodes in imported models
  (`SNAP_*`, `BOX_*`, `EXCL_*`), inventory, proxy sets (thousands of simple
  things as data, promoted near the player).
- **Physics (Jolt):** rigid bodies, character controller, joints, collision
  layers and surfaces, raycasts and shape casts.
- **Animation:** skinned glTF, crossfades, layers, events, two-bone, foot,
  look-at and aim IK.
- **Navigation:** tiled navmesh building and agents (Recast / Detour).
- **Audio:** 3D sources, buses, zones with muffling, music moods, ambience.
- **UI:** retained-mode game UI (`ve::ui`), ImGui debug overlay.
- **Networking:** host-authoritative co-op foundation (sessions, object and
  transform replication, requests to the host).
- **Editor:** hierarchy, inspector, gizmos, undo, terrain tools, prefabs,
  Play / Stop.
- **Platforms:** Linux (GCC, Clang) and Windows (MinGW-w64 GCC, Visual
  Studio 2022), checked by CI on Windows.
- **Tooling:** `tools/check_layering.py` enforces the module layering; the
  sandbox's autotest, editor test and two-process multiplayer test.

### Known limitations
See [docs/09-api-stability.md](docs/09-api-stability.md) for the areas still
expected to change (multiplayer gameplay through the host, world streaming)
and the roadmap's known issues.
