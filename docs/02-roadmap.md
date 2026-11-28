# Roadmap & Checklist

This is the **source of truth for progress**. Tick boxes as work lands
(`- [x]`). Every phase ends with acceptance criteria that must pass before
moving on. See [04-agent-handoff.md](04-agent-handoff.md) for the
verification workflow.

Legend: `[x]` done · `[ ]` todo · `[~]` partially done (see note)

## Status snapshot (2026-11-20)

Phases 0–13 are done, as are F2 (animation), most of F3, F5 tooling and the
engine side of F4 (interaction, save games, loading screens, gamepad,
profiler, navmesh + agents, prefabs, snap / placement, game UI), plus the
Savanna building kit, menus and inventory. Multiplayer (F6) has its
foundation: host-authoritative sessions, object replication and requests;
Savanna players see each other and build together.

- Zero Vulkan validation messages through the autotest (which also covers
  save / load, navigation and building) and the editor test.
- Savanna starts in ~0.9 s in Release, behind a loading screen.
- 2026-11-20 code review (see "Review follow-ups" under F5): use-after-free and
  dangling-pointer fixes (UI handlers, editor camera, component removal,
  weather, interactor, foliage focus, sandbox player refs), a network authority
  hole closed, per-frame allocations / scans removed, Clang + GCC Release
  warnings fixed, generic code moved into the engine (`buildLodChain`, `ve::env`, `ve::math`, config files) and game tuning out
  of it.

Next up: F6 N2 (gameplay through the host: interactions, pickups,
villagers, doors) alongside game-side F4 (animal AI, crafting, building
follow-ups, combat). New gameplay should be written host-authoritative from
the start (see conventions, "Multiplayer-ready gameplay"). Also see
[Known issues](#known-issues--tech-debt).

---

## Phase 0 — Project & build

- [x] CMake project with three targets: `ve_engine` (static lib), `ve_effects` (static lib), `savanna` (exe)
- [x] Pinned third-party fetch (VMA 3.1.0, ImGui 1.91.9b, cgltf 1.14, stb, miniaudio 0.11.21) via FetchContent
- [x] `ve_compile_shaders()` CMake function: GLSL → SPIR-V with `#include` + depfiles, outputs to `bin/shaders`
- [x] Assets copied to `bin/assets` on build
- [x] VS Code: tasks (Debug/Release), launch configs (gdb), IntelliSense via compile_commands
- [x] `.gitignore`
- [x] Engine consumable from an external game: `VE_BUILD_SANDBOX` option, `ve_setup_game()`, `templates/game` (verified by building a copy outside the repo)

**Accept:** fresh `cmake -B build -G Ninja && cmake --build build` succeeds with no warnings in our code. ✅

## Phase 1 — Core

- [x] `Log` (levels, `std::format`, coloured output)
- [x] `Window` (SDL2, resize/minimize, Vulkan surface + extensions)
- [x] `Input` (keys/mouse state, pressed/released edges, mouse delta/wheel, relative mouse mode, named action bindings, UI capture)
- [x] `Time` (dt, unscaled dt, time, frame count, fixed dt, time scale, fixedAlpha)
- [x] `Engine` main loop with fixed-timestep accumulator (§3 of architecture)

**Accept:** window opens, Esc quits, resize/minimize don't crash. ✅

## Phase 2 — GPU layer

- [x] `Device`: 1.3 + descriptor indexing + optional features (anisotropy, cube arrays, depth clamp, wireframe), VMA
- [x] Deferred deletion queue, immediate-submit helper, debug object names
- [x] `Swapchain` with recreation + vsync toggle
- [x] `Buffer` / `Image` helpers (VMA), image views per layer, mip generation via blits
- [x] Sampler set (linear/nearest × repeat/clamp, shadow compare)
- [x] `Bindless` descriptor set (textures + storage images) with slot allocation/deferred free
- [x] `PipelineBuilder` (graphics) + compute pipeline helper; single global pipeline layout

**Accept:** clear-colour frame presents with zero validation messages. ✅

## Phase 3 — Renderer core

- [x] `GpuTypes.hpp` ↔ `ve.glsl` shared interface (FrameGlobals, lights, materials, push constants)
- [x] `Mesh` (fixed `Vertex`, submeshes, AABB, staging upload) + `MeshData` helpers (append, flat shading, normals, tangents); primitives (cube, plane, sphere, icosphere, cylinder/cone, quad)
- [x] `Shader` (desc + lazy per-pass pipeline cache) and `Material` (+ `StandardMaterial`, opaque/cutout/transparent)
- [x] `RenderGraph` with stages, declared access, automatic barriers, screen-relative textures, resize, mid-pass transitions
- [x] Built-in passes: `ShadowPass`, `OpaquePass`, `TransparentPass`, `CompositePass` (ACES/Reinhard, exposure, contrast, saturation, vignette)
- [x] `FullscreenPass` helper + `Renderer::createFullscreenPipeline`
- [x] Frustum culling, opaque (queue → pipeline → front-to-back) / transparent (back-to-front) sorting
- [x] Reversed-Z main camera
- [~] Standard PBR shader (GGX, hemispherical ambient + split-sum approx, vertex colour, normal/MR/emissive/occlusion maps). *The normal-map path is implemented but no test asset uses it yet. Add a normal-mapped glTF to the sandbox.*
- [x] GPU timestamps per pass (profiler in F1 overlay)

**Accept:** lit meshes with different materials and custom shaders render; resizing works; validation clean. ✅

## Phase 4 — Shadows

- [x] Directional CSM: configurable light count (`maxShadowedDirectionalLights`) and cascade count (1–4), practical splits, sphere fit + texel snapping
- [x] Point light cube shadows: configurable count (`maxShadowedPointLights`), closest-to-camera assignment, distance-as-depth
- [x] 3×3 hardware PCF (directional), 4-tap (point), normal-offset + slope bias, cascade selection, distance fade
- [x] Per-cascade / per-face (+ light range) caster culling
- [x] Debug UI: cascade visualisation toggle, bias sliders, live count/cascade changes
- [x] Cascade blending: the last 15% of each cascade blends into the next (each cascade's box is widened to cover the band)

**Accept:** two shadowed directional lights (sun + moon) and ≥2 shadowed point lights (campfire + torches) render correct shadows, validation clean. ✅

## Phase 5 — Scene system

- [x] `Transform` hierarchy (local TRS, cached world matrix, dirty propagation, lookAt, directions, euler helpers)
- [x] `GameObject` (lifecycle hooks, components, active flag, children, tags, deferred destroy)
- [x] `Component` base + typed scene registry (`scene.forEach<T>()`)
- [x] `Scene` (spawn, find, findAllWithTag, start queue, fixed/update/late/gui dispatch, destroy flush)
- [x] Components: `Camera`, `MeshRenderer`, `DirectionalLight`, `PointLight`, `AudioSource` (+ `vfx::ParticleEmitter`)

**Accept:** a GameObject subclass moves itself in `update`, a child follows its parent, destroy mid-frame is safe. ✅ (Player/Head, Sunstone pickup in autotest)

## Phase 6 — Assets

- [x] `AssetManager` with search paths and caching
- [x] Texture import (stb), sRGB/linear, mipmaps, bindless registration; `Texture::create2D/create3D/solid`
- [~] glTF import (cgltf): meshes/primitives → submeshes, PBR materials, node hierarchy, nearest/linear samplers. *External-URI textures are tested (rondavel). The `.glb`/embedded and data-URI paths are implemented but untested.*
- [x] `scene.instantiate(model)`

**Accept:** sandbox loads PNGs and a glTF model and renders them correctly. ✅

## Phase 7 — Audio

- [x] `AudioSystem` (miniaudio engine, listener follows main camera, master volume)
- [x] `AudioClip`, `AudioSource` component (loop, volume, pitch, 3D min/max distance, play/stop, playOnStart), `playOneShot`

**Accept:** looping 2D ambience + 3D positional loop. ✅ The code path runs without errors (wind, fire, rain loops, pickup one-shot). *Whether it sounds right still needs a human listen.*

## Phase 8 — UI

- [x] ImGui (SDL2 + Vulkan dynamic rendering backends), `UiPass`
- [x] `onGui()` hook on GameObjects/Components
- [x] Engine debug overlay (F1): FPS/GPU time, stats, renderer settings, pass list with toggles + timings + per-pass `onGui()`, scene hierarchy
- [x] Input respects ImGui capture

**Accept:** HUD text visible; overlay toggles passes live. ✅

## Phase 9 — Effects module

- [x] `AtmospherePass`: sky-view LUT (Rayleigh/Mie/ozone), sun disc, stars, moon, half-res volumetric clouds (CPU-generated 3D Perlin-Worley noise), ambient from the sky, overcast
- [x] `VolumetricFogPass`: half-res height fog, 3D noise, sun shafts via cascades, point-light glow, Bayer dither + matching upsample
- [x] `BloomPass` (13-tap downsample with Karis average, tent upsample)
- [x] `ParticleEmitter` + `ParticlePass` (alpha + additive, velocity stretch, soft particles, wind, built-in sprites)
- [x] `Weather` (presets, smooth blending, wetness, wind gusts, camera-following rain, rain audio)
- [x] Wind/weather/sun global param convention in `vfx.glsl`

**Accept:** golden-hour scene with visible god rays and clouds; weather transitions smoothly. ✅

## Phase 10 — Savanna test scene

- [x] `tools/generate_assets.py` (detail/mud/thatch textures, rondavel glTF, wind/fire/rain/pickup audio)
- [x] Terrain: 321² heightfield in 100 chunks, biome colours (savannah / red soil / swamp / rock), custom terrain shader with wetness
- [x] Water plane with custom transparent shader (depth-based absorption + foam, sky-LUT reflections, rain ripples)
- [x] Vegetation: procedural acacia, baobab and fever trees, bushes, rocks and boulders; wind-swayed grass and reeds (custom shader)
- [x] Day/night cycle with sun + moon, atmosphere-coloured sunlight, exposure adaptation
- [x] Campfire (flicker, point shadows, fire/smoke/spark particles, 3D audio) and torches
- [x] Player controller (walk/sprint/jump with fixedUpdate gravity + interpolation, fly mode, slope limit, wading)
- [x] Collectibles + HUD + pickup sound
- [x] Weather cycling (R)
- [x] Test hooks + `AutoTest`

**Accept:** interactive frame rates on an Iris Xe-class integrated GPU at 1280×720, validation clean. ✅ (60 FPS vsync at 1600×900)

## Phase 11 — Physics (Jolt)

- [x] Jolt Physics 5.2.0 via FetchContent (own CMake, SSE4.2, private to `ve_engine`)
- [x] `PhysicsWorld`: fixed-step update after `fixedUpdate`, gravity, layers (static / moving / character), broad-phase optimisation after bulk adds, raycasts with body/character filtering
- [x] `Collider`: box, sphere, capsule, cylinder, convex hull, triangle mesh, heightfield, offset; shared handles; world scale applied
- [x] `Rigidbody` component: static / kinematic / dynamic, mass/friction/restitution/damping, continuous collision, forces, impulses, velocities, teleport; interpolated Transforms
- [x] `CharacterController` component (Jolt CharacterVirtual): slopes, steps, floor sticking, pushing bodies, inner body, jump
- [x] `scene.instantiate(model, parent, staticColliders)` mesh colliders from glTF
- [x] Savanna: terrain heightfield, tree trunks, rock hulls, huts (mesh), torches; player uses the controller; throw rocks (Q/RMB); crate pyramid; HUD look-at label via raycast; autotest physics checks
- [x] Game template: static/kinematic/dynamic example

**Still to do for physics** (roughly in priority order for the game):
- [x] **Collision & trigger events**: Jolt contacts are queued and dispatched
  on the main thread as `onCollisionEnter/Stay/Exit(const Collision&)` (other
  object, point, normal, impact speed) to components and GameObjects on both
  sides. `Rigidbody::isTrigger` sensors send `onTriggerEnter/Exit`, including
  for characters. Autotest: trigger box + crate impact counts.
- [x] **Shape casts & overlap queries**: `sphereCast`, `boxCast`, `shapeCast`,
  `overlapSphere/Box`, `overlap`, with a `QueryFilter` (statics, dynamics,
  characters, triggers, ignore). Per-game layer masks come with the
  configurable collision layers below.
- [x] **Collider debug drawing**: body wireframes (from the shapes' triangles,
  cached per shape) as `ve::debug` lines, F1 > Debug draw.
- [x] **User-configurable collision layers/masks** (F7).
- [x] **Joints / constraints** (F7): hinge, fixed, slider, distance
  components; limits, motors, springs, breaking.
- [ ] **Vehicles**: the concept art has trucks. Wrap Jolt's
  `VehicleConstraint` (wheeled vehicle, suspension, engine/transmission) as a component.
- [ ] **Water/buoyancy**: Jolt `ApplyBuoyancyImpulse` for floating debris,
  plus swim/wade volumes for the character.
- [ ] **Ragdolls** (needs F2 animation skeletons): Jolt `Ragdoll` from a skeleton.
- [x] **Collider streaming for large worlds**: terrain physics tiles and
  foliage colliders exist only near the player (Phase 12d / 13).
- [ ] **Character improvements**: character-vs-character collision
  (`CharacterVsCharacterCollisionSimple`), climbing onto dynamic crates,
  crouch (shape swap), being pushed by moving kinematic platforms, and fall
  damage hooks (landing velocity).
- [ ] **Interpolation of kinematic bodies** (they currently follow their
  Transform, which is already smooth; only matters if driven by physics velocity).
- [x] **Physics materials**: per-body friction / restitution + surface names
  (F7); per-triangle materials (one mesh, several surfaces) later.
- [ ] **Determinism / save-state** (later): body state serialisation for save games.
- [~] Known issue: Jolt quantises heightfield heights to 8 bits per block (a
  few cm on steep slopes). The triangulation now matches the render mesh.

**Accept:** player walks on terrain and can't walk through trees, rocks or huts;
thrown rocks collide and come to rest; crates topple when hit; raycasts report
the right object; validation clean; template builds outside the repo. ✅

## Phase 12 — Dense worlds: instancing, LOD, impostors, foliage

Goal: expansive Valheim × Far Cry 2 vistas (see the concept art): forest and
savannah to the horizon, dense grass, jungle, at 60 FPS on Iris Xe-class
hardware (target roughly 50k+ visible tree instances/impostors and a few
million grass blades), scaling up on discrete GPUs.

### 12a — GPU-driven instancing ✅
- [x] `InstanceGroup` API: up to 4 LODs × materials, per-instance transform + custom vec4 (tint), add/set/remove with stable handles
- [x] Global instance buffer (device-local, sub-allocated per group), dirty-range uploads via per-frame staging
- [x] Compute culling (`instance_cull.comp`): distance, frustum, min caster size per cascade, per view (camera + every directional shadow cascade), LOD pick by distance, compacted visible lists, indirect draws
- [x] Instanced path in shaders: `veModelMatrix()` / `veInstanceCustom()` (stage macros from CMake)
- [x] Instanced draws in OpaquePass + ShadowPass (directional), GPU visible-count readback for stats
- [x] Cutout (alpha-tested) shadow shaders for directional + point shadows
- [x] Physics: `PhysicsWorld::createStaticBody` (no GameObject), `RaycastHit::userValue`
- [x] Savanna: all trees, rocks and bushes are instance groups (~10.3k props vs ~800 before), reed screens test cutout shadows
- **Result:** GameObjects 976 → 331, CPU draw calls 312 → 109 (plus about 21 groups × views indirect draws), culling
  0.12–0.16 ms of GPU time, 3.3k static bodies at 0.2–0.5 ms per physics step. GPU frame time is 19.5 ms
  (Iris Xe, 1600×900) with 10× the props, versus 11 ms before. The cost is now full-detail vertex work
  (opaque about 6 ms, shadows about 6.4 ms); **12b LOD and 12c impostors are what bring this down.**
  Not measured on a discrete GPU yet.
- Open: visible-list memory scales with views × group capacity (per-view compaction later); point-light shadows skip instances.

### 12b — LOD ✅
- [x] meshoptimizer 0.22 dependency: `MeshData::simplified(SimplifyOptions)` (weld → simplify/sloppy → compact, `faceted` keeps low-poly flat shading) and `MeshData::optimize()` (vertex cache + fetch)
- [x] Screen-size LOD selection in the cull shader (`InstanceLod::screenSize`, projected radius / distance), distance mode kept (`maxDistance`), global `lodBias`
- [x] Dithered LOD crossfade: instances near a threshold are drawn in both LODs with complementary 4×4 ordered-dither masks (`veInstanceLodFade()` + `veLodDither()`), configurable band
- [x] Shadow LOD bias (`shadowLodBias`, default 1: shadows use one LOD coarser, no crossfade)
- [x] Per-LOD visible counts in the F1 overlay; LOD bias / crossfade sliders
- [x] Savanna: every prop variant gets a LOD chain (trees 3 levels, e.g. acacia 454/153/16 triangles; bushes/rocks 2)
- **Result:** GPU 19.5 → about 12 ms (Debug) / 13 ms (Release) on Iris Xe at 1600×900 with the same ~10.3k props;
  shadows 6.4 → 3.2 ms, opaque 6.1 → 3.2 ms; vsync-locked 60 FPS. Back to the pre-instancing frame time
  with 10× the props.
- Notes: simplification treats a mesh as one submesh (fine for single-material props; multi-material glTFs
  need per-part simplification). Custom material shaders must forward `veInstanceLodFade()` and call
  `veLodDither()` to crossfade; otherwise they pop. Depth-only shadows never crossfade.

### 12c — Impostors ✅
- [x] Octahedral impostor baking (`Impostor::bake`): N×N hemi-octahedral views (default 12×12 × 128 px) rendered on the GPU into an albedo atlas (sRGB) + object-space normal atlas, mip chain (stops at 8 px frames), at load time (a few ms per mesh; StandardMaterial layout)
- [x] Impostor rendering as the last LOD: camera-facing quad, 3-frame barycentric blend with per-frame ray/plane intersection (parallax-correct), alpha-weighted blending, normal-mapped engine lighting (receives shadows, day/night), per-instance tint, LOD crossfade
- [x] Impostor shadows: quad faces the light (from the shadow view-projection rows), nearest frame alpha test
- [x] Savanna: acacia/baobab/fever trees use mesh → simplified mesh → impostor; `SAVANNA_LODBIAS` test hook
- **Result:** impostors are visually equivalent to the meshes at distance (colour, faceting, shadows); aerial view with every prop visible at 60 FPS (GPU about 14 ms, Iris Xe)
- Not done: disk cache for baked atlases (baking is fast enough for now), depth/pixel-depth-offset for impostor intersections, full-sphere (looking from below) variant

### 12e — Screen-space effects ✅ (requested alongside 12c)
- [x] Thin G-buffer: `SceneNormal` (world normal + roughness) as a second Forward target; `veGBuffer()`; engine, sandbox and template shaders write it; `independentBlend` required
- [x] `vfx::ScreenSpaceGIPass`: half-res 1-bounce diffuse GI + AO, Bayer-rotated rays + 4×4 box upsample
- [x] `vfx::ScreenSpaceReflectionsPass`: half-res view-space march + binary refine, Fresnel/roughness/edge fades
- [x] `vfx::DepthOfFieldPass`: thin-lens CoC, auto focus, half-res golden-angle gather with background clamping
- [x] Savanna: SSGI + SSR on (`SAVANNA_SSFX=0` disables), DOF off (`SAVANNA_DOF=1` enables; on/off switches read through `ve::env::flag`, so `0` / `false` / `off` mean off); all toggle in F1
- **Result:** SSGI about 1.3 ms, SSR about 0.3 ms, DOF about 1.2 ms (1600×900, Iris Xe); validation clean
- [x] Follow-up: temporal accumulation for SSGI + SSR (`vfx::TemporalAccumulator`), exact 4×4 depth-aware SSGI upsample (fixed dot patterns around small bright lights)
- Not done: SSR roughness blur (glossy reflections are sharp), SSR fallback to the sky probe on miss, an albedo target for more accurate GI, bokeh highlights in DOF

### 12d — Foliage system ✅
- [x] `ve::FoliageSystem`: (cell, layer) streaming around the camera, nearest-first generation on a worker thread, per-frame instance budget, hysteresis unloading, stale-result tickets, `preload()`
- [x] Rules: deterministic jittered grid per layer, probability, value-noise clustering, height/slope range, `density(FoliageSample)` callback (biomes), exclusion circles, `avoid` other layers, scale/tint/align-to-normal/sink; `query()` for gameplay
- [x] Collider streaming: static bodies only near `colliderFocus` (budgeted, with hysteresis)
- [x] `ve::GrassPass`: GPU grass from height/mask textures, CPU tile frustum culling, R2-sequence clumps with flicker-free distance thinning, wind from a global param, custom fragment shaders
- [x] Grass extras (after review): random yaw + tilt per clump, alpha-clipped grass cards (`cardMesh`/`cardTexture`), grass shadows in the near cascades via the new `ShadowCaster` hook (about 0.5 ms)
- [x] Savanna: all props are foliage layers (savannah/swamp/kopje rules); grass and reeds are GrassPasses; autotest teleports across the map and checks streaming
- [x] Scene data (Phase W4/W5, docs/06-world.md): `Foliage` + `FoliageType`, `GrassLayer` (glTF LOD chains: mesh -> mesh -> cards), `FoliageExclusion`, `MeshLibrary`; per-biome densities; regenerates on terrain edits
- **Result:** preload about 110 ms for about 9.4k props; worker about 0.06 ms per (cell, layer); static bodies 3.3k → about 450 (near the player only); grass + reeds about 0.3–0.7 ms GPU; same visuals and draw counts as the old one-shot scatter
- [x] Interactive foliage: stable `FoliageInstanceId`s, per-instance states (alternative meshes + colliders, or removed) that are re-applied whenever a cell streams in, `overrides()` / `setOverrides()` for save games, and promotion of instances near the player to GameObjects (`promoteDistance`, `promote`) with a `FoliageLink`. Savanna: chop acacias into stumps (`ChoppableTree`), pick berry bushes that regrow (`BerryBush`, `Regrowth`); autotest chops, walks 1.3 km away and back, picks and waits for regrowth
- [x] Hi-Z occlusion culling of instances (camera view, last frame's depth pyramid): camera instance draws −55 % in the jungle, −59 % at the camp
- [x] Grass interaction: `GrassBender` components push blades aside (the player has one)
- Not done: a multi-threaded worker pool; saving overrides to disk (waits for the save system)

### 13 — Terrain streaming & LOD (prerequisite for multi-km worlds) ✅
- [x] Chunked heightfield streaming: `TerrainData` is an immutable generator
  snapshot plus a thread-safe LRU cache of 64-cell sample tiles (heights +
  biome weights) generated on demand (`tileCacheMB`); no whole-world grid
- [x] LOD: quadtree of 33×33-vertex nodes (`TerrainLod`), geomorphing towards the
  parent grid in `terrain.vert` / `terrain_shadow.vert`, skirts, worker-thread
  mesh building with an upload budget, LRU node cache, old meshes kept until
  edits are rebuilt
- [x] Physics heightfield tiles streamed around the camera, characters and
  dynamic bodies (`colliderDistance`); render mesh, queries and grass use
  Jolt's quad diagonal, so the visible and physical ground match
- [x] Grass bakes a window around the camera on big worlds (`GrassDesc::window`)
- [x] Foliage streaming stays cheap with tens of thousands of cells (update
  only re-walks cells when the viewer moves or something changed)
- [x] Savanna is 4 km wide: savannah around the camp, a desert with dunes and
  quiver trees to the east, a jungle with hills, jungle trees, palms and
  undergrowth to the west; the autotest walks into both
- [x] Splat materials: optional per-biome `material` (albedo, normal,
  roughness, AO, size, tint) and a triplanar `rockMaterial`; three layers per
  vertex with height-based blending; distance anti-tiling (rotated 3.7×
  second copy + macro noise); untextured biomes keep their vertex colours.
  Savanna: soil, sand ripples, leaf litter, rock
- [x] Editor sculpt / paint brushes (`TerrainEdits`, saved with the scene);
  brush rebuilds are incremental (tiles, LOD nodes, colliders, foliage cells
  and grass textures outside the brushed area are kept)
- **Result** (lavapipe, Release): 4 km world, 2049² samples, ~62k foliage
  instances preloaded in ~1.2 s; main thread ~1.8 ms per frame, as on the
  640 m map (it was 21 ms before foliage stopped re-walking every cell)

---

### Audio follow-up ✅
- [x] Mixer buses (SFX/Ambient/Weather/Music/UI) with volume + low-pass; `AudioZone` (box/sphere/cylinder) muffles buses while the listener is inside; per-source occlusion via physics rays
- [x] Savanna: rain on Weather, wind on Ambient, a zone in each hut, occluded campfire; autotest checks both
- Not done: reverb/echo zones, HRTF (front/back cues), Doppler

### Music & ambience ✅ (docs/07-audio.md)
- [x] A1 `MusicPlayer` (`audio().music()`): moods (playlists, shuffle, gaps), crossfades, mood stack with priorities (`pushMood` / `popMood`), resume, stingers; `MusicMood` / `MusicZone` scene components
- [x] A2 Audio parameters (`timeOfDay`, `night`, `rain`, `wind`, `storm` from the effects module; game ones like `danger`); `AmbientLayer` (loop + random 3D one-shots, gated by hours, terrain biome at the listener, parameters)
- [x] A3 Savanna: Explore / Camp / Combat moods (M = stand-in fight), camp music zone; crickets, frogs, day insects, birds, hyenas (synthesized placeholders, `tools/generate_ambience.py`); autotest logs moods and layer levels
- Later: intensity stems driven by a parameter, moving Weather's own loops onto layers

---

### Photo mode ✅
- [x] `vfx::PhotoMode` (F12): pause + frozen time, free camera, FOV / exposure / time of day / depth of field panel
- [x] `Renderer::beginPhotoCapture`: offscreen at any resolution, sub-pixel jitter, N frames averaged in linear colour, PNG; max-quality effect preset for the shot
- Later: EXR/HDR output, a "hide the player" toggle, tiled rendering beyond the GPU's max image size

---

### Performance & simplification pass ✅ (2026-11-16)
Reviewed by three scout agents (renderer/GPU layer, scene/world/physics,
effects/shaders); every change was checked with the autotest, the editor test,
zero validation messages, screenshot diffs and A/B benchmarks (`VE_BENCH`).
No feature or quality setting was removed; shader changes give the same image
(fp rounding), apart from the bug fixes noted.
- [x] Shaders, exact: 3×3 PCF as 4 bilinear taps; grass reuses the sun shadow
  from `veEvaluateLighting`; cofactor normal matrix; fog tracks its cascade
  along the ray (no per-step search/mat4); clouds return before texture fetches
  where density is provably zero; SSGI/SSR project the ray once; grass vertex
  shader stops at hidden clumps; arithmetic `veBayer4()`; water glint shadow
  only where the glint is non-zero
- [x] Shader bug fixes: `fog_apply` is now an exact 4×4 box (the Bayer dither
  didn't cancel, and it assumed half resolution); impostors sample with
  explicit gradients (implicit ones were undefined in divergent flow)
- [x] Point-light shadow cache (`cachePointShadows`, D16): a static camp
  re-renders ~1.5 of 24 cube faces per frame
- [x] Instancing: one cull thread per instance (was per instance × view),
  sorted per-pass draw lists, visible list sized to need (Savanna: 3.6 MB
  instead of 36 MB), cull views for every possible cascade (runtime-added
  shadowed lights lost instanced shadows)
- [x] Scene: hooks nobody overrides are skipped (D14); `forEach<T>` matches
  subclasses (D15, old known issue 5); id → object map (`findById` O(1),
  `findFirst<T>()`); destroy flush survives `onDestroy()` spawning or
  destroying (could leave freed components registered)
- [x] Dangling pointers fixed: `DayNightCycle` and the sandbox HUD cached
  Weather/DayNight pointers the editor could delete; they now use ids
- [x] Per-frame CPU: terrain edit-state JSON 4×/s instead of every frame,
  `Terrain::find` without a scene scan, ambience parsing cached, resting
  bodies don't rewrite transforms, transforms don't re-walk dirty subtrees,
  grass material set once per frame, hot-reload stats each file once per poll,
  primitives of the same shape share a mesh
- [x] Consolidation: `ve::Behaviour` declares the hooks `GameObject` and
  `Component` share; `FoliageRules` holds the fields `FoliageType` and
  `FoliageLayer` both declared; `ctx.fullscreen(...)` and `graph.sampled(id)`
  replace repeated pass boilerplate
- [x] Simplification: `ve/core/Strings.hpp` + `log::warnOnce` (5 copies),
  `vk::imageBarrier`/`memoryBarrier`/`recordMipChain` (3 copies),
  `vfx::ScreenSpaceTracePass` (SSGI + SSR shared their whole pass), one
  descriptor-write helper, `sandbox/Noise2D.hpp` removed (duplicate of
  `ve/world/Noise.hpp`)
- [x] Tooling: `VE_BENCH`, headless testing on GPU-less machines (lavapipe +
  Xvfb), see 04-agent-handoff.md §4
- **Result** (Release, 1600×900, Mesa lavapipe on 4 CPU cores, 3 alternating
  runs each; lavapipe weighs GPU work very differently from real GPUs, so
  re-measure the GPU side on hardware):

  | | camp at dusk | vista |
  |---|---|---|
  | Main thread (engine CPU) | 2.00 → 1.77 ms (−11 %) | 1.53 → 1.51 ms |
  | GPU total | −5.5 % | −4.2 % |
  | Shadows | −7.6 % | −6.3 % |
  | Volumetric fog | −7.0 % | −10.7 % |
  | SSGI | −5.1 % | −4.2 % |
  | Instance culling | −17.5 % | −3.8 % |
  | Atmosphere | +11.5 % | +9.7 % |

  On lavapipe the cloud early-outs made Atmosphere slower; whether to keep
  them depends on real-GPU numbers.
- Not done (measured or judged not worth it yet): depth prepass for foliage
  overdraw (bigger change; measure on real GPUs first), caching distant
  shadow cascades (would lag a frame), subgroup-aggregated cull atomics, a
  1×1 sky-ambient texture for fog, folding bloom's final blend into the
  composite (the engine would have to know about an effect).

---

## Known issues / tech debt

Ordered roughly by how much they'll bite while building a game.

1. **Motion vectors cover moving objects, not vertex animation.** Wind-blown
   foliage and grass rely on TAA's neighbourhood clipping and get slightly
   softer. Clouds and fog still blur their own dither instead of
   reprojecting it.
2. **Material params are positional** (slot indices, no reflection). Each
   shader documents its layout in a comment. A SPIR-V reflection layer could
   add named parameters.
3. Sky-view LUT assumes a viewer near y = 0 and a flat ground; very high
   cameras get an approximate sky.
4. Physics: Jolt quantises heightfield heights to 8 bits per 2×2 block, so
   feet may float or sink a few cm on steep slopes (the triangle diagonals
   match the render mesh since Phase 13). Characters can't
   climb up onto dynamic crates reliably (a CharacterVirtual limitation unless
   you tune `mMaxStrength` and the supporting volume).

---

## Future phases (not started; ordered by usefulness for a Valheim-like game)

### F2 — Animation ✅
- [x] Skinned meshes from glTF (joints / weights kept per `SkinnedMeshSource`, so `Vertex` stays fixed)
- [x] Animation clips (T/R/S channels, step / linear / cubic spline), crossfade blending, `Animator` component; joints are GameObjects
- [x] GPU skinning: `skin.comp` pre-pass into a shared output buffer drawn by the regular pipelines (`Mesh::createShared`)
- [x] Savanna: two `Villager`s (generated rigged model, Idle + Walk) wander around the camp; autotest checks clips, joints and skinned bounds
- Later: motion vectors from skinning, root motion (layers, events, two-bone / foot / aim IK: F7)

### F3 — World & performance
- [x] Clustered point lights: 16×9×24 clusters binned on the CPU each frame
  (lights are already culled there), `veLightList()` for custom shaders, full
  loop as a fallback
- [x] Temporal AA (`RendererSettings::temporalAA`): Halton jitter, camera
  reprojection from depth, Catmull-Rom history, YCoCg variance clipping
- [ ] Temporal reprojection for fog/clouds (fewer steps)
- [ ] Multithreaded scene gathering / command recording
- [x] Procedural fire (effects: `vfx::Fire` + `FirePass`; noise flames, coal bed, flicker for lights). Savanna: campfire and torches use it
- [x] Heat haze above fires (`Fire::haze`: copy of the screen around the fires, noise offsets, depth-aware)

### F4 — Building & gameplay systems
Engine provides mechanisms, the game (Savanna here) the rules and content.
- [x] Interaction (engine): `ve::Interactable` + `ve::Interactor` (eye ray, then view cone with line of sight; focus / prompt / use); Savanna's trees, berry bushes and sunstones use it, its HUD shows the prompt; the autotest chops and collects through it
- [x] Navmesh (engine): `NavMeshSurface` (tiled Recast build from terrain + static colliders + foliage colliders on worker threads, loading task, rebuilds around chopped trees, optional streaming around the camera), queries (paths, nearest / random points, raycast), `NavAgent` (Detour crowd: path following + avoidance). Savanna: villagers idle / wander / follow the player when asked (game-side state machine)
- [ ] Animal AI (herds grazing, fleeing the player) on the same agents, in the game
- [x] Inventory (game; `ItemStacks`, no longer in the engine): item types with baked icons, 24-slot stacks (add / remove / move / merge), saved with the game; inventory screen (Tab / I, drag & drop, details)
- [ ] Crafting (game; recipes as data files)
- [x] Grid/snap building (engine: prefabs, `SnapPoint` + `findSnap`, grid helpers, `PlacementBox` + `fits`, `PlacementPreview` ghost, save games keep spawned prefab instances; editor snaps on drag. Savanna: 12-kind kit (foundation, floor, mud / plaster walls, door, window, post, beam, metal / thatch roofs, stairs, railing, fence) as prefabs, build mode B, navmesh rebuilds; autotest builds, saves, reloads)
- [x] Foliage exclusions on building pieces: `FoliageExclusion` boxes (turn with the object), applied incrementally while playing (foliage re-places nearby cells keeping states, grass re-bakes in the background); Savanna's foundation and stairs prefabs carry one
- [x] Marker nodes in imported models: `SNAP_<type>_<accepts>[_align]`, `BOX`, `EXCL[_circle]` empties (or meshes, for their bounds) become `SnapPoint` / `PlacementBox` / `FoliageExclusion` on `ModelInstance` nodes; glTF extras override fields; more kinds via `registerModelMarker` (editor test checks it)
- [x] Doors / windows as fittings: snap into wall openings and turn to match (`SnapPoint::alignRotation`), host-aware fit test, removed with their wall; Savanna: plank door (hinged leaf, Use opens, state saved) and shuttered window
- [ ] Building follow-ups (game): costs / crafting, structural stability (support graph), damage / repair, more pieces (gables, verandas, multi-storey), material variants on the same piece (reference's "same structure, different materials")
- [ ] Combat (game: hitboxes, damage, stamina), boss encounters

### F6 — Multiplayer (co-op, host-authoritative)
Started early on purpose: retrofitting networking means rewriting every
system that changes the world, so new gameplay is written against the
authority API from now on (offline = a host with nobody connected). Model:
one player hosts (listen server, Valheim-style); clients own their character
and ask the host for everything else (decision D19).

**N1 — Foundation ✅**
- [x] Transport (engine, `ve/net/Transport`): ENet 1.3.18, reliable + unreliable channels, timeouts that survive loading screens
- [x] Byte streams (`ve/net/Stream`): values, varints, strings, CBOR JSON, 7-byte rotations; bounds-checked reads
- [x] `Network` (engine.net()): host / join / leave, handshake (game id + protocol version, refusals), peer list with round trips, `isServer()` / `isClient()`; poll at frame start, flush at frame end; messages arriving while loading wait for the load
- [x] Requests: named messages (`on`, `toServer`, `toClients`); on the host / offline `toServer` runs locally, so one code path serves all three modes
- [x] Objects: `NetworkObject` (identity = GameObject id, owner), `NetworkTransform` (owner sends 20 Hz unreliable, others interpolate 100 ms behind, teleports snap), `NetReplicated` (custom state, sent reliably when marked dirty), `spawn` / `despawn` of prefabs on every machine, ownership changes, the owner leaving despawns (or hands back) its objects
- [x] Late join: the world snapshot is a save-game capture (moved / removed level objects, spawned pieces, every Saveable incl. foliage, terrain edits, time of day), then the spawns saves don't cover, owners and state
- [x] Savanna: `SAVANNA_NET=host` / `join:<address>`, player avatars (villager model, walk / idle from speed, replicated name), building through the host (`build.place` / `build.remove`, navmesh rebuilt everywhere), host clock and weather sent to clients
- [x] Test: `tools/nettest.sh` runs a host and a client process headless over localhost: snapshot (piece placed before joining), live spawns, interpolated avatar movement, a client build request, the client leaving (its avatar despawned)

**N2 — Gameplay through the host** (engine side ✅; game side in progress)

Engine mechanisms (generic: no gameplay rules in the engine, decision D38):
- [x] Stable player ids (`Network::playerId`, remembered per user), `PeerInfo::playerId`, `findPlayer`; a second connection with the same id is refused
- [x] Per-player save data next to a slot (`save::writePlayer` / `readPlayer` / `players`)
- [x] Player objects per peer (`setPlayerObject`, replicated) and `withinReach` / `viewpoint` for validating requests
- [x] Requests with answers (`toServer(name, payload, reply)`, `reject(reason)`): predict, undo if refused
- [x] Who may send what (`on(name, fn, From::Clients / Host)`), a per-client request budget (`Config::maxRequestsPerSecond`), `kick`
- [x] `onPeersChanged` on every machine (player lists)
- [x] Dynamic bodies follow the owner kinematically on other machines (thrown things simulated by their owner, usually the host)
- [x] `NetworkAnimator`: Animator playback (clips per layer, looping, speeds, layer weights) from the owner
- [x] Save state replication: any `Saveable` on a `NetworkObject` with `replicateSaveState` (Foliage's chopped trees, Terrain edits, game objects) as JSON patches when it changes (`saveRevision()`)

Savanna (game rules on top):
- [x] Foliage changes made on the host reach everyone (the Foliage object replicates its save state)
- [ ] Chopping / picking requested by clients (`tree.chop` through the host, with prediction), sunstones, doors (`DoorHinge` as NetReplicated), throwing rocks (host-simulated physics)
- [ ] Villagers / animals simulated on the host (NetworkTransform + NetworkAnimator)
- [ ] Inventories per player (the host keeps each player's by player id; `save::writePlayer`)
- [ ] Save games in a hosted world: the host saves, players keep their characters
- [x] Menu: Host game / Join game (name, address, port fields), connection status in the panel
- [ ] Connection errors / players joining in the HUD, player list (Tab), joining from a played title world (reload first)
- [ ] The host validates requests (piece fits, reach, materials) with `withinReach` / `reject`

**N3 — Quality**
- [x] Interest management: `relevanceRadius` per object (or `Config::relevanceRadius`) around each client's camera / player object; catch-up on re-entry. Not yet: priorities, hiding objects' existence
- [x] Snapshots per client delta-encoded against the last acknowledged pose (mm, 32-bit rotations, unchanged poses not sent); bandwidth in F1 > Network
- [x] Lag compensation primitives: pose history, `viewTime(peer)`, `rewind(peer, fn)`. Not yet: client-side prediction of movement other than requests
- [x] Dedicated server mode (`EngineConfig::server`, VE_SERVER=1: headless, no audio / editor, fixed tick rate). Not yet: running without a Vulkan device, NAT punch-through / relay, Steam / platform sessions
- [x] Network conditions simulator (`VE_NET_SIM`, `NETTEST_SIM` for tools/nettest.sh)

### F7 — Engine follow-ups (debugging, physics, animation, UI)
- [x] Debug drawing (`ve/debug`): lines / shapes / labels from anywhere, depth-tested over the final image with faint hidden parts, durations, channels in F1 (physics colliders, terrain, characters, navmesh, nav agents; games add their own). Savanna: "Villagers" channel, `SAVANNA_DRAW`
- [x] Collision layers: 32 named layers + collision matrix, per-body layer (Rigidbody, CharacterController, static bodies), query masks. Savanna: thrown rocks are "Debris", the Use ray looks past them; the autotest checks a box falling through a platform it doesn't collide with, and masked rays
- [x] Physics materials: per-body `surface` names in hits, contacts and the character's ground (`groundSurface`); foliage types and terrain set theirs. Savanna: footsteps by surface (grass / dirt / wood / stone, generated sounds), building pieces by material
- [x] Animation layers (joint masks, override / additive, fading weights), clip events, two-bone IK and foot IK. Savanna: villagers wave on an arm layer, step sounds from Walk events, feet on slopes; the model has foot / hand joints and a Wave clip
- [x] UI text input (`ui::TextInput`: OS text, caret, editing keys with repeat, filters, submit) and scrolling lists (`ui::ScrollView`: clip, wheel, bar, drag, focus follow). Savanna: Save as (name + list), Load and Controls lists scroll, Multiplayer panel (name, address, port, Host / Join / Leave)
- [x] Joints: `FixedJoint`, `HingeJoint`, `SliderJoint`, `DistanceJoint` (limits, soft limits, velocity / position motors, friction, break force / torque + `onBreak`, broken state saved, safe body lifetime, debug drawing). Savanna: doors are dynamic leaves on motor-driven hinges (pushable), a rope lantern by the fire; the autotest's joint yard checks a limited pendulum, a 180 deg/s motor, a slider to 1.5 m, a 4-link rope and a weld breaking under a dropped block
- [x] Look-at and aim IK (`AimIK`, `LookAtIK`): weighted chains, angle limits, blending. Savanna: villagers look at a nearby player and point at the nearest sunstone after greeting; the autotest checks both aims converge
- [ ] Later: root motion, motion vectors from skinning; text selection / clipboard, multi-line text, on-screen keyboard for pads; ragdolls, buoyancy (Phase 11 list)

### Decals ✅ (2026-11)
- [x] Clustered forward decals: oriented boxes projecting albedo (mix / multiply), normal (+ flatten), roughness and metallic, each with its own opacity x alpha; angle / depth / distance / lifetime fades; sort order; receive layers. `DecalProjector` component (editor: Create > Decal, box gizmo, inspector; scene files) and `Renderer::decals()` for transient decals. Applied in `standard.frag` and `terrain.frag` before lighting (SSR sees wet puddles). Savanna: puddles in the camp, a cracked hut wall, V / pad left shoulder shoots bullet holes; the autotest checks projectors, lifetime expiry and a shot
- [ ] Later: decals on grass / impostors, an atlas for many small decal textures, emissive decals (embers), GPU binning if thousands are in view

### Shared assets ✅ (2026-11)
- [x] glTF image files go through the AssetManager texture cache: models exported side by side share one GPU texture per file
- [x] Material files (`*.material`, `MaterialAsset`): StandardMaterial values + textures, custom shader + extra slots; used by glTF materials by name (`materials/<name>.material`) or by a `material` custom property; one shared Material per file; live reload + editor (Assets > Materials). Savanna: the huts' thatch comes from `materials/thatch.material`; the autotest checks texture sharing, the material file and a live edit
- [x] `standard_painted.frag`: vertex-painted dirt / wear / wetness over the standard material (placeholder to adapt). Savanna: `SAVANNA_PAINTTEST=1` test wall
- [ ] Later: texture compression (BC7 / BC5, KTX2), per-instance material overrides

### Asset cooking (2026-11)
- [x] Phase 1: cook cache (`<exe>/cooked`), validity by cooker version + settings hash + source size / time / content hash, background on-demand cooking on the job pool, `vecook` tool (`--check`, `--force`, `--clean`)
- [x] Phase 2: textures -> KTX2 with prebuilt mips, BC7 (colour), BC5 (normal maps), BC4 (greyscale data); per-image `TextureSettings` (`.texture` files / name convention) instead of caller flags
- [x] Phase 3: audio -> Ogg Vorbis (libogg + libvorbis encoder, miniaudio/stb_vorbis playback; `<sound>.audio` settings; sandbox 92 MB of WAV -> 7 MB, lengths sample-exact)
- [x] Phase 4: glTF -> `.vemodel` (meshes with normals / tangents, materials, nodes, skins, animations, embedded images; stamp covers `.bin` buffers and material files). `<model>.model` settings: generated LODs (`autoLods`), a baked octahedral impostor (BC7) and material remaps, cooked offline by `vecook` on a headless renderer (or on first load in the game); foliage uses the cooked levels and impostor instead of baking at load. Savanna: `models/test_tree.gltf`; the autotest checks the cooked LODs and that its impostor needs no bake
- [x] Phase 5: `ve::vfs` (every engine file read: disk first, then mounted `.vepak` archives overlaid on the exe folder), `.vepak` archives (per-entry zstd, index at the end), cooker registry (`ve::Cooker`: vecook and packaging treat every kind alike), `vecook --package <dir>` (cooked copies instead of sources, no settings files / .blend / `.vepackignore` matches; shaders, the game and its runtime libraries; `--loose`, `--level`). Packaged savanna: 13.8 MB -> 11.2 MB archive, autotest passes without any source asset
- [ ] Later: alpha-coverage-preserving mips for cutouts, editor panel for .texture files, swapping a freshly cooked texture in while running

### F5 — Tooling
- [x] Shader hot-reload: edited GLSL (or any include) is recompiled with the build's own glslc command and its pipelines rebuilt; errors keep the old shader (`ShaderHotReload`, `PipelineHandle`); compiles run in the background, the frame never waits for glslc
- [x] Scene serialization (JSON scene files, editor save / load)
- [x] Prefabs (E3): `.prefab` files, `prefab::instantiate`, `PrefabInstance` link, editor Save as prefab / Create > Prefab (no overrides / propagation yet)
- [x] Loading screens (engine): `Engine::beginLoading(loadingScene)` + load steps (main thread, one per frame) + load tasks (progress of work over frames); the game's loading scene (UI) is drawn while the world streams in editor-paused mode. Foliage fills its cells on a worker pool (cores − 1) instead of the blocking preload. Savanna: sunset title, progress bar, tips. Loading waits for the area around the viewer (foliage `readyDistance`; terrain LOD on a worker pool, unbudgeted uploads): 2.4 s -> 0.9 s in Release. In the editor: no loading scene, a progress panel over the world as it fills in (also `openScene`)
- [x] Save/load: `ve::save` (slots, capture / apply: moved and removed level objects, `ve::Saveable` hooks; Foliage overrides, Terrain edits and time of day built in). Savanna: F5 / F9 (reload behind the loading screen, then apply), player + regrowth Saveable, counters as game data; the autotest saves, changes things, loads and checks
- [x] Gamepad input + rebinding (engine): SDL game controllers, axes with radial dead zones, triggers as actions, `startRebind`, bindings JSON, virtual pad for tests. Savanna: pad layout for the player, F10 controls window saved to `config/controls.json`
- [x] CPU profiler zones (engine): `VE_PROFILE` scopes, nested, F1 "CPU zones" table + bench output
- [x] Retained-mode game UI (engine, `ve/ui/Gui.hpp`): canvases of anchored elements (panels, labels, images, buttons, progress bars, sliders, stacks, grids), mouse + keyboard / pad focus navigation, drag & drop, tooltips, modal capture, TTF fonts, mesh icon baking. Savanna: title screen over a live sunset (Continue / New / Load / Settings / Controls / Quit), pause menu, settings saved to `config/settings.json`, HUD (health / stamina, hotbar, clock), inventory, build menu with piece cards; the autotest opens and drives each

### Review follow-ups (2026-11-20 code review)
Done in the review: the items listed in the status snapshot. Found but left
for later (bigger changes, or behaviour worth deciding deliberately):
- [x] Split `Renderer` into `SceneGatherer`, `ShadowSetup`, `LightClusterer`, `FrameCapture`; `Editor` and `Physics.cpp` into one file per part
- [x] Layering (D23, D26): composition root in `app/Engine.cpp`, `core/Engine.hpp` a forward-declaring service locator; per-module type registration; skinning gatherers; render-side debug line buffer; mesh collider cache in physics; `tools/check_layering.py` enforces the table and Jolt's privacy
- [ ] Shrink the layering exceptions listed in `tools/check_layering.py` (scene → assets / anim / physics in a few files, physics / nav ↔ debug, audio → world, world → gameplay / ui)
- [x] Async uploads: a per-frame staging ring flushed at the start of the frame (`Device::flushUploads`); no GPU wait per streamed mesh
- [x] Upload only changed material slots (`MaterialDirtySet`)
- [x] Static renderers as GPU instances (`StaticBatcher`, D28) and proxy sets for many simple things (`ve::ProxySet`, D27)
- [ ] Draw static batches in front-to-back order with the opaque list (the opaque pass lost ~25 % on lavapipe in the stress test)
- [ ] World streaming for placed objects: proposal in [08-world-streaming.md](08-world-streaming.md)
- [ ] Motion vectors for skinned meshes
- [x] Audio low-pass cutoffs go to the audio thread (atomic target, applied in a custom node's process callback)
- [x] One shared engine job pool (`ve/core/Jobs.hpp`): foliage, terrain LOD, grass bakes, navmesh tiles and shader compiles; no destructor waits for a cancelled bake
- [ ] Fog: the sky ambient is the same for every pixel; compute it once on the CPU
- [ ] Network join: `welcome()` sends several small messages per networked object
- [x] Remembered objects are typed handles (`ObjectRef` / `ComponentRef`) across engine, effects and Savanna (incl. Campfire, Torch, Sunstone children)
- [ ] `FoliageType` (scene-built foliage) bakes impostors with the neutral edge colour; expose `ImpostorSettings::backgroundAlbedo` as a field if fringes show
