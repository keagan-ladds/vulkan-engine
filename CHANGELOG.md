# Changelog

What changed in each tagged version of vulkan-engine, for people building a
game on it. The rules for what may change between versions are in
[docs/09-api-stability.md](docs/09-api-stability.md).

Every breaking change to a stable API gets an entry under **Breaking**, with
what to change in game code. Games should pin a version (a tag) and update on
purpose: see "Pin an engine version" in [templates/game/README.md](templates/game/README.md).

## [Unreleased]

### Breaking
- **Network protocol changed** (player ids in the hello, snapshots, replies):
  hosts and clients must run the same engine version. `Transport::Stats`
  byte counts are now real totals per connection (ENet's reset every second).
- **`ve::Inventory` is gone from the engine.** Inventories are game rules
  (slots, stacking, what an item is), and the engine shouldn't decide them.
  The same class lives on in the sandbox as `savanna::ItemStacks`
  (`sandbox/src/ItemStacks.*`). Migration: copy those two files into your
  game (or write your own), and drop `#include <ve/gameplay/Inventory.hpp>`.
- **Material extra slots end at params 10 / textures 14.** The standard
  layout now uses params[11] and textures[15] for translucency, so a material
  file's `"params"` / `"textures"` entries for those slots are ignored (with
  a warning) and `MaterialAsset::apply` no longer clears them. Migration: move
  a custom shader's inputs from slot 11 / 15 to a free one (4-10 / 5-14).
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

### Changed
- **World edits rebuild after the drag, not during it.** Dragging a stamp,
  mask, road, exclusion or a field in the editor no longer rebuilds terrain,
  foliage and grass every few frames: they rebuild once it's released (plus
  `Engine::editSettleSeconds`, 0.35 s). New `Engine::setInteractiveEdit()` /
  `worldEditsHeld()`. Moving a `TerrainStamp` now rebuilds only the land
  around it (it used to rebuild the whole terrain and all foliage), and
  terrain collider tiles are re-sampled on the job pool instead of the main
  thread (a 100-250 ms hitch per edit before).
- **Mesh pipelines compile in the background.** A shader's pipelines are
  built on the job pool from when it's created (`RendererSettings::
  asyncPipelines`, env `VE_ASYNC_PIPELINES=0` to turn off): no frame hitch on
  a shader's first draw, but `Shader::pipeline()` can return null and objects
  using it are skipped until it's ready. Custom passes binding
  `shader->pipeline(kind)` themselves must skip a null pipeline. Loading
  screens wait for pending compiles ("Compiling shaders").
- **Start-up and loading stay responsive.** The window appears with the first
  frame instead of sitting black while the engine starts. The loading screen
  (or the editor's progress panel) is drawn before any load step runs. Objects
  spawned while loading start within a ~50 ms budget per frame
  (`Scene::startPending(budgetMs)`), Foliage builds one layer's meshes per
  frame, and the clouds' noise is cached in `cooked/generated/`. Savanna
  (Debug, software GPU): the longest loading frame went from 4 s to under 1 s.
  Loading itself can take a little longer: the navmesh is now built with the
  vegetation already placed, rather than rebuilt after loading.

### Added
- **Multiplayer mechanisms (N2 / N3, engine side).** All generic: the game
  keeps every rule (docs/decisions.md D38, conventions "Multiplayer-ready
  gameplay").
  - Stable player ids (`Network::playerId`, `PeerInfo::playerId`,
    `findPlayer`), per-player save files (`save::writePlayer` /
    `readPlayer` / `players`), player objects (`setPlayerObject`,
    `withinReach`, `viewpoint`), `kick`, `onPeersChanged`.
  - Requests with answers: `toServer(name, payload, reply)` and `reject()`
    in the host's handler (predict, undo if refused); `on(name, fn, From)`
    says who may send a message; a per-client request budget
    (`Config::maxRequestsPerSecond`).
  - Replication: dynamic bodies follow kinematically where they aren't
    owned; `NetworkAnimator` (ve/net/NetworkAnimator.hpp); any `Saveable`'s
    state with `NetworkObject::replicateSaveState` (JSON patches;
    `Saveable::saveRevision()`; Foliage and Terrain implement it).
  - Interest management (`relevanceRadius`), per-client snapshots
    delta-encoded against acknowledged poses, `Network::bandwidth()`,
    F1 > Network, lag compensation (`poseAt`, `viewTime`, `rewind`), a
    bad-network simulator (`VE_NET_SIM`, `Transport::Simulation`), dedicated
    server mode (`EngineConfig::server`, `VE_SERVER=1`, `Engine::isServer()`).
  - `Animator::looping` / `clipSpeed` / `layerWeightTarget`,
    `UiLayer::addOverlaySection`, `FoliageSystem::overrideRevision`.
- **Material translucency** (leaves, paper, cloth): `"translucency"`,
  `"translucencyColor"`, `"translucencyTexture"` in material files,
  `StandardMaterial::setTranslucency` / `setTranslucencyTexture`. Light
  reaching the back comes through (like Blender's Translucent BSDF); the front
  reflects less by the same share. In standard.frag and standard_painted.frag;
  custom shaders: `veStandardTranslucency` + `veTranslucentLighting`
  (ve_lighting.glsl). Savanna: `SAVANNA_LEAFTEST=1`.
- **Blender: "VE Standard" node group** (tools/blender/ve_painted_node_group.py,
  now the "VE materials" add-on): the standard material with the material
  file's fields as inputs, exported like VE Painted (by hand or with the glTF
  export). Panel: Mode / Cutoff per material, **Convert to VE Standard** for
  imported Principled (+ Translucent) materials; an alpha wired from a
  separate image is merged into `<albedo>_rgba.png`. VE Painted gains the
  translucency inputs. Tested by `tools/blender/test_ve_standard.py`.
- **Installable engine package**: `cmake --install <build> --component
  VulkanEngine --prefix <dir>` makes a relocatable package (headers, static
  libraries, vecook, compiled shaders + GLSL includes, a
  `find_package(VulkanEngine)` config with `ve_setup_game()`). The game
  template uses a package when `CMAKE_PREFIX_PATH` points at one, else builds
  the engine from `VE_ENGINE_DIR`. Engine and bundled libraries get a `d`
  suffix in Debug builds. Windows CI builds the template against MSVC and
  MinGW packages and uploads the MSVC Release package.
- **Prefab icons** (`ui::prefabIcon`, `ui/PrefabIcon.hpp`): menu pictures of
  any prefab, rendered from its meshes with their materials and textures in a
  private scene. A hand-made `<prefab>.png` wins, else the cooked icon
  (`<cache>/<prefab>.veicon`, assets/IconCook.hpp: `vecook` renders them for
  prefabs with an `"icon"` section, which also sets the view), else rendered at
  load and cached. `ui::renderIconPixels` / `iconParts` draw meshes with
  materials. Packaging ships icon prefabs and their cooked icons
  (`Cooker::shipSource`). The sandbox build menu uses them (and gains a hut
  made from the textured rondavel model).
- **`<game> --cook`**: vecook's code is now `ve::cookToolMain(argc, argv)`
  (`ve/app/CookTool.hpp`); a game's `main` can pass `--cook [options]` to it
  (the template's does), so cooking and packaging see the game's types.
  Cookers can report files they can't cook here (`Cooker::unsupportedHere`):
  vecook lists them as "left to the game". `Engine::registerTypes()` for
  tools that read scene / prefab data without an Engine.
- **Finding things near a point**: `Scene::findNearest<T>(p, maxDistance,
  filter, horizontal)`, `Scene::forEachWithin<T>(p, radius, fn)`,
  `Scene::findNearestWithTag`, `Scene::forEachActive<T>` (T: a component type
  or a GameObject class). The sandbox uses them for power: generators, lamp
  posts that need one nearby to be built, fuel barrels that fill the nearest.
- **Splines** (`ve::Spline`, `scene/Spline.hpp`): smooth curves through
  control points (centripetal Catmull-Rom) with a width factor per point;
  `path()` gives a world-space `SplinePath` (length, `at(distance)`,
  grid-accelerated `closest()`, side of the path). Editor: Create > Spline,
  point editing in the viewport (Tab; drag, Ctrl+click to add / insert,
  Delete), snap to ground, undo.
- **Fences, powerlines and props along splines** (`ve::SplineInstancer`,
  `world/SplineInstancer.hpp`): a mesh repeated along a spline (spacing or one
  per control point, dropped onto the terrain, random yaw / scale / jitter,
  LOD chain + impostor, optional box colliders), with sagging cables or
  straight rails between items. `FoliageExclusion::spline` clears a corridor of
  trees and / or grass along a spline. Editor: Create > Spline > Fence /
  Powerline.
- **Roads and paths** (`ve::TerrainSpline`, `world/TerrainSpline.hpp`): flatten
  the terrain across their width with embankments (following the smoothed
  ground or the spline's height), paint a biome, and optionally lay a surface
  mesh with a material. Editing one re-shapes only the land around it.
  `TerrainData`'s constructor takes the road splines (new trailing
  parameter). Editor: Create > Spline > Road / Path; the selected spline shows
  its road width and vegetation corridors.
- **Sub-biomes** (`TerrainSubBiome`, Terrain `subBiomes`; laid by the
  `TerrainMask` component along a spline or in a circle): variations inside
  whatever biome is there: foliage density (all layers or per layer), grass
  density and height. Foliage / grass biome rules accept sub-biome names.
  `TerrainData` gains `subBiomeWeight`, `foliageFactor`, `grassFactors`;
  `TerrainData`'s constructor takes `TerrainOverlays` (roads + sub-biome
  masks) as its last parameter. `Foliage::layerNames()`.
- **Blender: VE Painted materials export with the model.** Installed as an
  add-on, `tools/blender/ve_painted_node_group.py` writes the `.material` file
  of every VE Painted material a glTF export includes (into the `assets`
  folder the .gltf is saved under), and the VE Engine panel gains **Export all
  VE materials**.
- **Blender: exact preview of the painted overlays.** The VE Painted example
  material samples grunge / dirt / wear world-space triplanar, as the engine
  does (Box projection was wrong on rotated or joined objects); **Convert
  overlays to triplanar** rewires existing materials. Exporting from a .blend
  whose VE Painted group is older than the add-on no longer fails (missing
  inputs export their defaults, with a warning).
- **Blender: normal map fixes.** A normal-map input at strength 0 no longer
  bends the preview whatever colour its socket holds; Create / update resets
  unconnected normal inputs to flat. The export warns when a normal map's
  name (or `.texture` file) won't make the engine load it as a normal map, and
  leaves out a baked normal at strength 0.
- **Spot light cookies and chromatic aberration** (`SpotLight::cookie`,
  `cookieTexture`, `chromaticAberration`): a mask projected across the beam
  (an old, badly focused torch: rings, a bulb hole, smudges) and red / blue
  lens fringes at the cone's and the cookie's edges. They also show in
  volumetric fog. New shader helper `veSpotColor(L, l)` (vec3); `veSpotFactor`
  stays a float (now including the cookie's pattern). Editor fields on the
  SpotLight inspector. Savanna: the player's torch uses
  `textures/torch_cookie_mask.png` (`SAVANNA_TORCH_COOKIE=0`: plain cone).
- **Cooked models, LODs and impostors.** glTF models cook to `.vemodel` in
  the cook cache (no glTF parsing at load; the sandbox villager loads in
  0.7 ms). An optional `<model>.model` file asks for generated LOD levels
  (`autoLods`), a baked octahedral impostor and material remaps
  (`"materials": {"Bark": "materials/bark.material"}`); `vecook` cooks those
  on a headless renderer (a game run cooks them on first load otherwise), and
  `FoliageType` uses the cooked levels and impostor instead of baking at load.
  Packaging ships the cooked model without its `.bin` / `.model` files, and
  now includes `cooked/generated/`. New API: `ve::ModelData`, `parseGltf`,
  `buildModel`, `ModelSettings` (`assets/ModelData.hpp`), `assets/ModelCook.hpp`,
  `Model::cookedLods`, `impostorFromCooked`, `log::infoOnce`, `core/BinaryIO.hpp`.
- **Spot lights** (`ve::SpotLight`): torches, headlights, street lamps. A cone
  (`innerAngle` / `outerAngle`, degrees) along the object's forward, with
  range, intensity and optional shadows (only the cube faces the cone
  reaches); beams show in volumetric fog. Editor: Create > Light > Spot light,
  with a cone gizmo. Custom shaders with their own point-light loop should
  multiply by `veSpotFactor(L, l)` (the game template's toon shader does).
  The sandbox player has a torch (T / pad d-pad down).
- **Scene files load in the background.** `Engine::addSceneLoad(name, path,
  then)` reads and parses the file on the job pool, then creates its objects
  a time slice per frame; `then(roots)` runs before any of them start (adjust
  them, spawn what goes with them). Built on `SceneSerializer::loadAsync`
  (`SceneLoad`), `Engine::addLoadStepOverFrames` (a load step spanning frames;
  later steps wait) and `Scene::holdStarts()`. The sandbox loads its level
  this way.
- **API reference and documented headers.** Every public header declaration
  now has a doc comment (`///`, shown by IntelliSense / clangd);
  `cmake --build build --target docs` builds an HTML reference with Doxygen
  (when installed). `python3 tools/check_docs.py --changed` lists
  undocumented declarations in the headers a branch touches.
- **Hooks for loading and compile UI.** `Engine::loadEvents()` (Began, step
  started / finished, task finished, Ended) and `Renderer::compileEvents()` /
  `compileStatus()` (pipelines and hot-reloaded shaders: started / finished /
  failed, pending counts), built on a new `ve::Event` / `ve::Subscription`
  (`core/Event.hpp`). The sandbox HUD shows a "Compiling shaders" note with
  them. `Renderer::waitForPipelines()` for screenshots and tests.
- **Release packaging and archives.** `vecook --package <dir>` writes a
  shippable folder: the game, its runtime libraries and `data.vepak` (zstd;
  assets with images / sounds as their cooked copies only, cooked files,
  shaders; no `.texture` / `.audio` / `.blend` / hidden files or
  `assets/.vepackignore` matches). `--loose` writes folders instead. The
  engine mounts `<exe>/*.vepak` at start (`EngineConfig::mountArchives`); all
  engine file reads go through the new `ve::vfs` (disk first, then archives),
  so game code reading its own files should use `vfs::read` / `vfs::open` /
  `vfs::exists` too. New: `AssetManager::loadPixels` (CPU pixels, also from a
  cooked copy), `ve::Cooker` / `registerCooker` (cooker registry used by
  vecook). `AssetManager::exists` now also counts an asset whose cooked copy
  is all that exists.
- **Audio cooking.** WAV and FLAC sounds are cooked into Ogg Vorbis like
  textures (on demand and by `vecook`; ~10-15x smaller, lengths sample-exact
  so loops stay seamless), tuned per sound with an optional `<sound>.audio`
  file (quality, mono, sampleRate, compress). Ogg Vorbis files now play
  (miniaudio's stb_vorbis is compiled in). `AssetManager::textureStats()` is
  now `cookStats()` (textures and sounds).
- **Asset cooking (textures).** Images are cooked into GPU-ready KTX2 files
  (BC7 colour, BC5 normal maps, BC4 greyscale data, prebuilt mips) in
  `<exe>/cooked`: automatically on a worker thread the first time they load,
  used from the next run on (no decoding or mip generation at load, 4-8x less
  GPU memory). The `vecook` tool cooks everything ahead of time (releases, CI).
  `EngineConfig::cookCache` / `cookOnDemand`; env `VE_COOK=0|read|sync`,
  `VE_COOK_DIR`. Needs BC texture support (every desktop GPU); without it the
  source images are used.
- **Blender node group for the painted shader**
  (`tools/blender/ve_painted_node_group.py`): "VE Painted" reproduces
  `standard_painted.frag` in Blender so painted assets look the same while you
  paint; `tools/blender/test_ve_painted.py` checks the two agree. Its
  "Export VE material" button (Material tab > VE Engine; also installable as
  an add-on) writes the engine's `.material` file from the node.
- **Baked normal for `standard_painted.frag`**: texture slot 8 (params slot
  9: strength, UV scale) takes a per-asset normal map on the mesh's own UVs,
  whiteout-blended over the material's tiled normal map: a generic tiled
  surface plus damage baked per asset. The Blender node group has matching
  "Baked Normal" inputs and the material export writes them.
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
- Enum fields in data files (material `"mode"`, ...) read their names
  (`"Cutout"`, any case) as well as numbers; a name used to be ignored
  silently (the material stayed Opaque).
- **"Instance buffer full" after many rebuilds.** Freed instance ranges were
  never merged with their neighbours, so repeated foliage / grass rebuilds
  fragmented the buffer until no run was big enough. They're merged now, and
  the error says how much is free and the largest free run.
- **Blue, prop-shaped "shadows" on walls next to props** (and a blue rim along
  the bottom edge of objects). The ambient ground-bounce check (`veAmbient`)
  read the sun shadow at one point 2 m out along the normal, so a crate's sun
  shadow in that spot removed the warm bounce and printed the crate's outline
  in sky blue on the wall, and downward faces sampled underground. It now
  averages four lookups 1.5 m above the surface, around a point 2.5 m out
  horizontally: props under ~1.5 m can't affect it, and the shade of terrain,
  huts and trees still removes the bounce, as a soft gradient. Savanna:
  `SAVANNA_BOUNCETEST=1` (wall + cubes).
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
