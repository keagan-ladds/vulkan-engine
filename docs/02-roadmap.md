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

Next up: the **Future phases** at the bottom, starting with F1 (physics) and
F2 (animation), which a Valheim-like game needs next. Also see
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
8. Physics is just "stand on the heightfield", so the player walks through
   trees and huts.
9. `Scene::forEach<T>()` only finds components registered under exactly `T`
   (a `class MyLight : public PointLight` added as `MyLight` is not seen by the
   renderer). Add as `addComponent<PointLight>` or extend the registry.

---

## Future phases (not started; ordered by usefulness for a Valheim-like game)

### F1 — Physics & character
- [ ] Integrate Jolt Physics: rigid bodies, static mesh/heightfield colliders, raycasts
- [ ] `Rigidbody`, `Collider` components; physics in `fixedUpdate` with interpolation
- [ ] Character controller (Jolt CharacterVirtual), replace terrain-height sampling in `Player`

### F2 — Animation
- [ ] Skinned meshes from glTF (joints/weights in a second vertex stream so `Vertex` stays fixed)
- [ ] Animation clips, sampler, blending, `Animator` component
- [ ] GPU skinning (compute pre-pass writing skinned vertices, or vertex shader)

### F3 — World & performance
- [ ] Engine-level terrain module (streaming chunks, LOD, splat maps) — promote `sandbox/Terrain`
- [ ] GPU instancing for foliage/rocks (`InstancedMeshRenderer`), indirect draws
- [ ] Clustered/tiled light culling for many point lights
- [ ] Mesh LODs and distance culling; shadow caster LOD
- [ ] Temporal AA (+ temporal reprojection for fog/clouds)
- [ ] Alpha-tested foliage shadows (cutout shadow shader)
- [ ] Multithreaded scene gathering / command recording

### F4 — Building & gameplay systems
- [ ] Grid/snap building system (pieces, sockets, structural stability)
- [ ] Inventory/crafting data model (data files)
- [ ] Interaction system (raycast "use" with prompts) — generalise `Sunstone`
- [ ] AI (state machines / behaviour trees), navmesh (Recast/Detour)
- [ ] Combat (hitboxes, damage, stamina), boss encounters

### F5 — Tooling
- [ ] Shader hot-reload (watch `shaders/`, recompile via glslc, `Shader::invalidate()`)
- [ ] Scene/prefab serialization (JSON)
- [ ] Retained-mode game UI (inventory grids, health bars) beyond ImGui
- [ ] Gamepad input + rebinding UI
- [ ] Save/load
- [ ] CPU profiler zones (GPU per-pass timing already exists)
