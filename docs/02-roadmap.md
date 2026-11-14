# Roadmap & Checklist

This is the **source of truth for progress**. Tick boxes as work lands
(`- [x]`). Every phase ends with acceptance criteria that must pass before
moving on. See [04-agent-handoff.md](04-agent-handoff.md) for the
verification workflow.

Legend: `[x]` done · `[ ]` todo · `[~]` partially done (see note)

## Status snapshot (2026-11-14)

Phases 0–10 are implemented and verified on Linux with an Intel Iris Xe
(Mesa 25, Vulkan 1.4):

- Zero compiler warnings on a clean Debug build. Release also builds and runs.
- Zero Vulkan validation messages, covering startup, every test-hook view,
  window resizing, the autotest and shutdown.
- The savanna scene runs vsync-locked at 60 FPS at 1600×900 in a Debug build
  with validation on, with about 11 ms of GPU time. Per-pass timings are in the
  F1 overlay.
- `SAVANNA_AUTOTEST=1` passes in both Debug and Release.

Physics (Phase 11, formerly future phase F1) is done too: Jolt rigid bodies, a
character controller and raycasts, all used by the savanna scene and the game
template. One physics step takes about 0.3–0.7 ms for roughly 410 bodies.

Next up: **Phase 12 (dense worlds)**, then F2 (animation) and building/gameplay systems. Also see
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
- [ ] Cascade blending at split boundaries (seams can be visible on large flat areas)

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
- [ ] **Collision & trigger events**: Jolt `ContactListener` → queued and
  dispatched on the main thread as `Component::onCollisionEnter/Stay/Exit(other, contact)`.
  Sensor bodies (`Rigidbody::isTrigger`) → `onTriggerEnter/Exit`. Needed for
  pickups, damage zones, water volumes and doors.
- [ ] **Shape casts & overlap queries**: `sphereCast`, `boxCast` and
  `overlapSphere/Box` (with layer masks). Needed for melee hit detection, AI
  senses and building placement validity.
- [ ] **Collider debug drawing**: implement Jolt's `DebugRenderer` on top of a
  new engine line/wireframe renderer (also useful for AI paths, nav, bounds).
  F1 toggle.
- [ ] **User-configurable collision layers/masks**: currently fixed at
  static / moving / character. Games need e.g. projectiles, triggers,
  enemies, debris and "ignore player".
- [ ] **Joints / constraints**: hinge (doors, gates), fixed (breakable
  building pieces), distance/rope, plus a constraint component API.
- [ ] **Vehicles**: the concept art has trucks. Wrap Jolt's
  `VehicleConstraint` (wheeled vehicle, suspension, engine/transmission) as a component.
- [ ] **Water/buoyancy**: Jolt `ApplyBuoyancyImpulse` for floating debris,
  plus swim/wade volumes for the character.
- [ ] **Ragdolls** (needs F2 animation skeletons): Jolt `Ragdoll` from a skeleton.
- [ ] **Collider streaming for large worlds**: only create bodies for
  trees/rocks near the player (ties into the foliage system). Thousands of
  static bodies across a huge map are a waste.
- [ ] **Character improvements**: character-vs-character collision
  (`CharacterVsCharacterCollisionSimple`), climbing onto dynamic crates,
  crouch (shape swap), being pushed by moving kinematic platforms, and fall
  damage hooks (landing velocity).
- [ ] **Interpolation of kinematic bodies** (they currently follow their
  Transform, which is already smooth; only matters if driven by physics velocity).
- [ ] **Physics materials**: per-shape friction/restitution + surface type
  (footstep sounds, impact particles).
- [ ] **Determinism / save-state** (later): body state serialisation for save games.
- [ ] Known issue: the heightfield collider triangulates differently from the
  render mesh (a few cm on steep slopes), and Jolt quantises heights to 8 bits per block.

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
- [x] Savanna: SSGI + SSR on (`SAVANNA_SSFX=0` disables), DOF off (`SAVANNA_DOF=1` enables); all toggle in F1
- **Result:** SSGI about 1.3 ms, SSR about 0.3 ms, DOF about 1.2 ms (1600×900, Iris Xe); validation clean
- Not done: temporal accumulation (would allow fewer rays and less noise), SSR roughness blur (glossy reflections are sharp), SSR fallback to the sky probe on miss, an albedo target for more accurate GI, bokeh highlights in DOF

### 12d — Foliage system
- [ ] World cells streamed around the camera (worker-thread generation, GPU upload budget per frame)
- [ ] Biome rule sets (density textures/noise, slope/height/water constraints, spacing, scale/tint variation) for savannah, jungle and swamp
- [ ] Collider streaming (static bodies only near the player)
- [ ] GPU grass: compute-generated blades around the camera (no storage), wind, distance fade, player interaction later
- [ ] Hi-Z occlusion culling (optional; mostly for jungle)

### 13 — Terrain streaming & LOD (prerequisite for multi-km worlds)
- [ ] Engine terrain module: chunked heightfield streaming, LOD (geomorphing or clipmaps), splat materials
- [ ] Physics heightfield tiles streamed with the terrain

---

## Known issues / tech debt

Ordered roughly by how much they'll bite while building a game.

1. **Alpha-tested materials cast solid shadows.** The default shadow shader
   ignores alpha. Add a cutout shadow fragment shader (`ShaderDesc` already
   allows a custom one).
2. **No temporal accumulation for clouds/fog.** They use per-pixel jitter plus
   a blur and look slightly soft. TAA or temporal reprojection would allow fewer
   steps and sharper results.
3. **No cascade blending**, so cascade transitions can show a seam.
4. **One draw call per submesh.** Fine for about 1k objects. Grass is
   pre-merged per 32 m cell. Large forests need GPU instancing or indirect
   draws (F3).
5. **Material params are positional** (slot indices, no reflection). Each
   shader documents its layout in a comment. A SPIR-V reflection layer could
   add named parameters.
6. **Shader hot-reload missing.** Changing a shader means restarting the app.
7. Sky-view LUT assumes a viewer near y = 0 and a flat ground; very high
   cameras get an approximate sky.
8. Physics: the heightfield collider's triangle diagonals can differ from the
   render mesh, so feet may float or sink up to a few cm on steep slopes.
   Jolt also quantises heights to 8 bits per 2×2 block. Characters can't
   climb up onto dynamic crates reliably (a CharacterVirtual limitation unless
   you tune `mMaxStrength` and the supporting volume).
9. `Scene::forEach<T>()` only finds components registered under exactly `T`
   (a `class MyLight : public PointLight` added as `MyLight` is not seen by the
   renderer). Add as `addComponent<PointLight>` or extend the registry.

---

## Future phases (not started; ordered by usefulness for a Valheim-like game)

### F2 — Animation
- [ ] Skinned meshes from glTF (joints/weights in a second vertex stream so `Vertex` stays fixed)
- [ ] Animation clips, sampler, blending, `Animator` component
- [ ] GPU skinning (compute pre-pass writing skinned vertices, or vertex shader)

### F3 — World & performance
- [ ] Clustered/tiled light culling for many point lights
- [ ] Temporal AA (+ temporal reprojection for fog/clouds)
- [ ] Multithreaded scene gathering / command recording

### F4 — Building & gameplay systems
- [ ] Grid/snap building system (pieces, sockets, structural stability)
- [ ] Inventory/crafting data model (data files)
- [ ] Interaction system (raycast "use" with prompts) — generalise `Sunstone` (raycasts now available)
- [ ] AI (state machines / behaviour trees), navmesh (Recast/Detour)
- [ ] Combat (hitboxes, damage, stamina), boss encounters

### F5 — Tooling
- [ ] Shader hot-reload (watch `shaders/`, recompile via glslc, `Shader::invalidate()`)
- [ ] Scene/prefab serialization (JSON)
- [ ] Retained-mode game UI (inventory grids, health bars) beyond ImGui
- [ ] Gamepad input + rebinding UI
- [ ] Save/load
- [ ] CPU profiler zones (GPU per-pass timing already exists)
