# Architecture Decision Log

Short records of the choices that shape the codebase. Add a new entry (don't
edit old ones) when a decision changes.

---

### D1. Vulkan 1.3 with dynamic rendering + synchronization2
**Chosen:** require Vulkan 1.3 (`dynamicRendering`, `synchronization2`).
**Alternatives:** Vulkan 1.0 render passes/framebuffers.
**Why:** removes `VkRenderPass`/`VkFramebuffer` boilerplate and gives clearer
barrier APIs. Every desktop GPU from roughly 2016 on, and Mesa, supports it.

### D2. One global pipeline layout + bindless textures
**Chosen:** set 0 = per-frame globals (UBO + light/material SSBOs + shadow maps),
set 1 = a 4096-entry bindless texture array; 128-byte push constants.
**Alternatives:** per-material descriptor sets; shader reflection to build layouts.
**Why:** every shader can access everything ("all shaders see lights, matrices"
was a requirement), materials become an index plus raw parameters, and passes
pass texture indices in push constants. That means no descriptor management in
game code and descriptors are bound once per pass.
**Cost:** needs descriptor indexing (Vulkan 1.2 core). Parameters are positional,
not named.

### D3. Fixed vertex format (64 bytes)
**Chosen:** a single `ve::Vertex` (position, normal, uv, tangent, colour).
**Why:** any mesh works with any material/shader, which keeps pipeline creation
trivial. The vertex colour channel suits low-poly procedural art.
**Cost:** memory for unused attributes. Skinning will need an extra vertex
stream rather than a different format.

### D4. "Render graph lite"
**Chosen:** an ordered pass list (stage + order), per-frame declared access,
automatic whole-image barriers, and screen-relative textures.
**Alternatives:** a full render graph with pass culling, aliasing and async compute.
**Why:** it gets most of the benefit (no manual barriers, arbitrary user passes)
at a fraction of the complexity. Passes can still transition textures mid-pass.

### D5. Reversed-Z for the main camera
**Why:** much better depth precision for large outdoor scenes (camera far plane
4 km). Shadow maps keep standard Z because their ranges are small. Both
conventions are documented in 03-conventions.md.

### D6. Unity-style GameObject + Component hybrid
**Chosen:** `GameObject` is subclassable (user requirement: "characters derive
from a base class") *and* holds components (renderers, lights, audio, emitters).
**Why:** behaviour sits naturally on the object while engine features compose.
A full ECS was rejected for now (harder to learn, unnecessary at this scale).

### D7. Effects live outside the engine (`ve_effects`)
**Why:** the requirement was that atmosphere, fog, clouds, weather and particles
"should not be hard-coded into the engine". Building them only on the public API
proves the extension points work and keeps the core small. Global-param slots
0–3 are reserved by convention.

### D8. Forward rendering (not deferred)
**Why:** simpler, handles transparency and custom shaders uniformly, and works
well with a few dozen lights. Clustered light culling is planned (F3) if light
counts grow.

### D9. Third-party via pinned FetchContent tarballs
VMA, ImGui, cgltf, stb and miniaudio are fetched at configure time and compiled
into `ve_thirdparty` with warnings off. Vulkan, SDL2 and glm come from the
system. **Why:** no vendored code in the repo, reproducible versions, small
dependency surface.

### D10. Half-resolution volumetrics with ordered dither
**Context:** full-resolution fog cost 25 ms on Iris Xe.
**Chosen:** half-resolution raymarch, per-pixel light prefiltering, a 3D noise
texture instead of ALU noise, and 4×4 Bayer jitter with a matching 4-tap box
upsample. Result: 3.8 ms.
**Next:** temporal reprojection (F3) for higher quality at the same cost.

### D11. Procedural test content + generated assets
The sandbox's world is generated in code, and its files (textures, glTF, WAV) are
produced by `tools/generate_assets.py`. **Why:** no licensing questions, small
repo, and the importers are still exercised on real files.

### D12. Games consume the engine via `add_subdirectory` (no install/package step)
**Chosen:** a game's CMakeLists adds the engine checkout as a subdirectory and
calls `ve_setup_game()`. Outputs land in the game's build folder.
**Alternatives:** installed CMake package (`find_package(vulkan-engine)`), or a git submodule/FetchContent.
**Why:** simplest while the engine changes quickly. You edit the engine and the
game rebuilds it, with no install step. FetchContent from a git URL works the
same way once the engine has a remote; switching to it is a one-line change in
the template.

### D13. Jolt Physics, hidden behind an engine API
**Chosen:** Jolt 5.2.0 (fetched, built with its own CMake), linked privately.
The engine exposes its own `Collider`/`Rigidbody`/`CharacterController`/`PhysicsWorld`.
**Alternatives:** PhysX (heavy SDK), Bullet (older and slower), or exposing Jolt types directly.
**Why:** Jolt is modern, fast, MIT-licensed, and has a proper virtual character
controller (stairs, slopes, pushing), which a Valheim-like game needs. Hiding it
keeps games simple and makes the backend swappable. Its SIMD flags propagate to
`ve_engine`, so it's pinned to SSE4.2 to keep binaries portable.
**Unity-like split:** physics steps after `fixedUpdate`, and Transforms are
interpolated between steps.

### D14. Skip hooks nobody overrides (2026-11 performance pass)
**Chosen:** the base `GameObject`/`Component` versions of `update`,
`fixedUpdate`, `lateUpdate` and `onGui` set a "not overridden" bit when they
run; the Scene stops calling them and skips objects with no hooks at all.
**Alternatives:** per-hook object lists maintained on spawn (needs compile-time
override detection, which reflection factories and subclass chains defeat);
calling every hook every frame (thousands of empty virtual calls plus a
hierarchy walk each).
**Cost:** an override must not call the base's empty hook. Documented in
GameObject.hpp and the handoff gotchas.

### D15. The component registry matches subclasses
**Chosen:** components stay bucketed by exact type; `forEach<T>` caches the
list of buckets whose type is `T` or derives from it (a `dynamic_cast` on one
element) and rebuilds it only when a new component type appears.
**Why:** a `class FlickerLight : public PointLight` was invisible to the
renderer (old known issue 5). Iteration stays a cached lookup per frame.

### D16. Cache point-light shadow faces
**Chosen:** hash every cube face's inputs and skip it when unchanged
(`RendererSettings::cachePointShadows`).
**Alternatives:** explicit "static light" flags (error-prone: something moves
into range and the shadow is wrong); updating faces round-robin (visible lag).
**Why:** point lights in this kind of game (torches, campfires) are static and
mostly surrounded by static geometry. The hash covers everything the depth
cube depends on, so the result is always identical to re-rendering.

### D17. Benchmark mode and software-GPU testing
**Chosen:** `VE_BENCH=<frames>[,<warmup>]` logs averaged main-thread CPU time
(thread clock), frame time, physics step and per-pass GPU timestamps, then
quits; combined with `VE_HEADLESS` and `VE_CAPTURE` it gives reproducible
numbers and screenshots in CI-like environments, including GPU-less machines
with Mesa's lavapipe. Compare builds A/B in alternating runs on the same
machine: absolute numbers drift with machine load.

### D18. Terrain streaming: generator snapshots, sample tiles, CDLOD-style quadtree
**Chosen:** the terrain is a pure function of its parameters and stamps.
`TerrainData` snapshots those inputs and caches 64-cell sample tiles on
demand; the visible ground is a quadtree of fixed-size nodes with geomorphing
and skirts, built on a worker thread; physics tiles follow the camera,
characters and dynamic bodies.
**Alternatives:** geometry clipmaps (need a toroidally updated heightmap on the
GPU and don't suit per-vertex biome colours); streaming baked heightmaps from
disk (needs an offline bake step; the editor edits the generator live).
**Why:** one code path for every world size, no load-time bake, edits stay
live (old meshes keep showing until rebuilt), and every query (foliage,
grass, audio, gameplay) sees exactly the generated samples. Coarse nodes
evaluate the generator directly, so far terrain never forces full-resolution
tiles into memory.


### D19. Multiplayer: host-authoritative co-op, started early
**Chosen:** one player hosts (a listen server); the host owns the world and
clients own only what they control (their character, via
`NetworkTransform`). Everything else is a request to the host (`toServer`),
which applies it and replicates the result (`spawn` / `despawn` / state).
Offline play is a host with nobody connected, so the same code runs in all
modes. Late joiners get a save-game capture as the world snapshot. Transport:
ENet (reliable + unreliable channels over UDP).
**Alternatives:** lockstep (needs a deterministic simulation: physics and
floating point make that impractical); rollback / prediction everywhere (far
more complex, mostly needed for competitive action); client-authoritative
world (simple, but conflicting edits and trivial cheating); a library like
GameNetworkingSockets or yojimbo (heavier, more dependencies; the transport
is hidden behind `ve::net::Transport`, so it can be swapped later).
**Why:** it matches a Valheim-like co-op survival game (few players, friends,
building and gathering where a little latency on actions is fine), it needs
no determinism, and the save system already describes "the world" so joining
mid-game comes almost for free. Started before the gameplay is finished
because single-player-only systems (direct world changes) are what make
networking expensive to add later.
**Cost:** clients feel the round trip on actions until prediction is added
(N3); the host's machine and connection carry the session.

### D20. Removed components leave the object at once, freed at end of frame (2026-11 review)
**Chosen:** `removeComponent` unregisters the component, runs its
`onDestroy()` (if it started) and moves its `unique_ptr` out of the object's
list into a scene-owned list freed in `flushDestroyed`. Hook loops iterate a
snapshot of the component pointers.
**Alternatives:** erase immediately (a component removing itself inside its
own hook freed the code that was running: use-after-free); flag in place and
erase later (every loop over `components()` in engine and game code would
have to remember to skip flagged ones).
**Why:** no dangling `this` inside hooks, and `components()` stays the plain
list callers expect. **Cost:** a removed component lives until the end of the
frame (`isRemoved()` tells a remembered pointer apart).

### D21. Platform-independent default seeds; persistent pipeline cache (2026-11 review)
- A `FoliageType` without a seed derives it with FNV-1a over its name (was
  `std::hash<std::string>`, which differs between standard libraries): the
  same scene places the same trees with every compiler, which saves
  (`FoliageInstanceId` overrides) and co-op between platforms (D19) rely on.
  **Cost:** default placement changed once; saves made earlier with chopped
  or picked instances in seedless layers refer to the old layout.
- `Device` keeps a `VkPipelineCache` on disk (`pipeline_cache.bin` next to the
  executable). Header fields are checked before use; a mismatch or a damaged
  file just starts an empty cache.

### D22. Remembered objects and components are typed handles (2026-11)
**Chosen:** `ObjectRef<T>` (just the id; `get(scene)` = `Scene::findById` plus
a checked cast) and `ComponentRef<T>` (owner id, address and a never-reused
`Component::serial()`, checked against the owner's live components before the
address is trusted). Neither converts back to a pointer by itself.
**Alternatives:** raw pointers + `isDestroyed()` (reads freed memory); bare
`uint64_t` ids with hand-written `findById` + casts (worked, but nothing
enforced it, and checking components by address alone breaks when an address
is reused); a generation-indexed handle table (more machinery than needed).
**Why:** the editor can delete any object at any time, and most bugs the
2026-11 review found were remembered raw pointers. The type now makes the
safe pattern the easy one. **Cost:** a hash lookup (plus a short component
scan) per use; `get()` without a scene only sees the main scene.

### D23. Module layering is enforced, with extension points instead of upward includes (2026-11)
**Chosen:** `tools/check_layering.py` encodes which engine module may include
which, plus a short list of known exceptions with reasons, and fails on
anything else. Lower modules expose extension points that the composition
root (`Engine`) wires up: each module's `registerXTypes(TypeRegistry&)`,
`SkinningSystem::addGatherer` (anim feeds skinning without render including
anim), render's `DebugLineBuffer` (debug pushes lines into it),
`Engine::atShutdown` (world's mesh cache), `Engine::editorPaused`. The model
collider cache moved from `Model` (assets) to `PhysicsWorld::meshCollider`.
**Why:** the module-level rule (engine ← effects ← game) was clean, but inside
the engine includes had drifted sideways and upwards, which makes splitting
the engine or a server build harder. A script keeps it honest; the exception
list should shrink over time.

### D24. One engine job pool (2026-11)
**Chosen:** `ve::jobs` (`ve/core/Jobs.hpp`): hardware threads − 1 workers
(max 15), Normal and Background lanes, cancellable handles, `parallelFor`,
`DrainGroup` for systems that keep their own priority queue (foliage keeps
nearest-first ordering). World systems submit jobs instead of owning threads.
Jobs own their inputs and results (`shared_ptr`), so owners cancel and drop
them without blocking; only jobs that read their owner (foliage) cancel and
wait, bounded by one item. Jolt keeps its own pool, sized by
`jobs::physicsThreadCount()`; it only runs while the main thread waits on a
step.
**Alternatives:** per-system threads (oversubscribed the CPU: foliage alone
started cores − 1 threads per system); a work-stealing scheduler with fibers
(more than this engine needs).

### D25. Asynchronous buffer uploads; dirty-only material uploads; audio filter on the audio thread (2026-11)
- `Device::uploadBuffers` stages into a per-frame ring and queues the copy;
  `flushUploads` records queued copies at the start of the next frame (after a
  barrier against earlier reads and writes, before a transfer → read barrier),
  and `immediateSubmit` flushes them first. Staging blocks are reused under the
  deferred-deletion rule (F + `kFramesInFlight`); deferred destructions still
  pending when copies are flushed move to the flushing frame.
  `uploadBuffersNow` blocks. **Why:** every streamed terrain node, skinned
  mesh and prop used to stall the main thread on the GPU. **Cost:** data is on
  the GPU from the next frame's command buffer on; a pass that creates a
  mesh and draws it in the same frame must use `uploadBuffersNow`.
- Materials copy only dirty slots into each frame's material buffer
  (`MaterialDirtySet`), instead of the whole table every frame.
- Low-pass cutoffs are handed to the audio thread through an atomic and
  applied in a custom miniaudio node's process callback (the main thread used
  to re-initialise a filter the audio thread was running).

### D26. A composition root above the modules; big classes split by concern (2026-11)
**Chosen:** `Engine`'s implementation, which creates every module, wires
their extension points and runs the loop, lives in `ve/app/Engine.cpp`, the
top layer. `ve/core/Engine.hpp` stays where every module can include it as
the service locator, but only forward-declares the subsystems
(`RendererSettings` moved to its own plain-data header for `EngineConfig`).
**Alternatives:** moving the whole `Engine` (header included) to `app/`: every
module that calls `Engine::get()` would then include upwards; replacing the
locator with injected dependencies (a large API change for games).
**Also:** classes that had grown past what one person can hold in mind are
split by concern without API changes: `Renderer` owns `SceneGatherer`,
`ShadowSetup`, `LightClusterer` and `FrameCapture`; the `Editor` is one class
across `Editor*.cpp` files per panel (the panels share most state); physics is
one file per component plus `JoltInternal.hpp`, which keeps D13's rule (Jolt
only in `ve/physics/*.cpp`, now checked by `tools/check_layering.py`).

### D27. Proxy sets: data far away, objects up close; saves store the difference (2026-11)
**Chosen:** `ve::ProxySet` holds stored things as entries with stable ids
(sequential for level content, random for runtime spawns), draws them
instanced, and promotes those near a focus into GameObjects carrying a
`ProxyLink`; demotion captures the object's transform and Saveable state back
into the entry. Save games hold removed ids plus changed or added entries,
against the level's entries as they were at `start()`.
**Alternatives:** one GameObject per item (thousands of objects, per-object
saves); saving the whole list (large saves for untouched levels); dirty flags
(miss changes made through promoted objects).
**Why:** the same pattern foliage already proved, made generic, and the
building block for streaming placed objects (docs/08-world-streaming.md).
**Cost:** a copy of the level's entries in memory; an entry whose object has
Saveable state counts as changed once it has been promoted. Not replicated
over the network yet (N2).

### D28. Static renderers become GPU instances (2026-11)
**Chosen:** `MeshRenderer::isStatic` puts a renderer into an InstanceGroup per
(mesh, materials, castShadows) owned by the Renderer's `StaticBatcher`. The
Scene reports changes (transform dirtying only for objects with batched
renderers, `setActive`, destroy, reparenting, removal, `refresh()`); there is
no per-frame scan. Point-light cube faces get the batched entries in range as
draw items, so the face cache (D16) still hashes what it draws; editor
picking draws them from the batcher.
**Alternatives:** per-frame dirty checks over all statics (the cost being
removed); merging static meshes (breaks per-object picking, removal and
colliders); automatic detection of unmoving renderers (needs the per-frame
scan again).
**Why:** player buildings are thousands of pieces that never move: 3,000
pieces cost ~16 ms of main thread and ~10k draws, now ~10 ms and ~1k, with
the same image. **Cost:** changing a static renderer's fields from code
needs `refresh()`; batched statics have no motion vectors and lose
front-to-back ordering against the per-item opaque list (the opaque pass was
~25 % slower on lavapipe in the stress test); `staticBatching = false`
restores the old path.

### D29. Windows: Vulkan SDK + MSVC, SDL2 from source when missing, UTF-8 process code page (2026-11)
**Chosen:** on Windows the only prerequisites are the LunarG Vulkan SDK and a
compiler (Visual Studio 2022 / Build Tools; MinGW-w64 also works). If no SDL2
CMake package is found, `cmake/Dependencies.cmake` builds SDL2 from source as a
static library, so there's no DLL to ship. MSVC builds use `/utf-8` (sources
are UTF-8), `/bigobj`, `/permissive-`, the DLL C runtime everywhere (Jolt
would default to the static one, which fails to link), and `NOMINMAX` /
`WIN32_LEAN_AND_MEAN`. Every game executable gets a manifest whose
`activeCodePage` is UTF-8 (Windows 10 1903+): `char*` file APIs in stb,
cgltf, miniaudio and `std::filesystem` then take UTF-8 paths as on Linux.
**Alternatives:** requiring vcpkg (another tool to install); converting every
path to wide strings at each C API (40-odd call sites and the third-party
loaders); requiring SDL2 development packages (manual setup, DLL copying).
**Cost:** Windows builds compile SDL2 once (about a minute); Windows versions
before 10 1903 would treat non-ASCII paths in the old code page.

### D30. Decals are applied in the material shaders (clustered forward decals) (2026-11)
**Chosen:** decals are oriented boxes gathered on the CPU each frame (nearest
1024 in view), sorted, written to a `DecalBuffer` (set 0, binding 14) and
binned into the light cluster grid. Material shaders call `veApplyDecals` on
their `VeSurface` before lighting, so a decal changes albedo, normal,
roughness and metallic with per-channel opacity and everything downstream
(lights, shadows, ambient, the thin G-buffer for SSR) sees it. Decal textures
are bindless and sampled with gradients from the position derivatives, so the
per-pixel decal loop may diverge. Normal maps are oriented on the decal's
frame and built on the geometric normal; `flatten` blends the surface's own
normal towards it (standing water).
**Alternatives:** deferred decals (a box drawn after opaque that blends into
a G-buffer: this renderer is forward and has no albedo/roughness targets, so
it could only darken the lit colour or the normal); mesh decals (geometry
clipped to the surface: per-decal CPU work and no channel control); decal
atlases (one texture per decal is simpler with bindless).
**Why:** correct PBR changes to every channel (wet puddles reflect, bullet
holes are rough and dented) for the price of a cluster lookup per pixel, with
no extra pass and no G-buffer. **Cost:** only shaders that call
`veApplyDecals` receive decals (standard and terrain; grass and impostors
don't); each overlapping decal costs up to three texture samples in those
shaders; more than 1024 decals in view keeps the nearest.

### D31. Shared textures by path; material files replace glTF materials by name (2026-11)
**Chosen:** glTF image files are loaded through `AssetManager::loadTexture`
(keyed by canonical path, colour/data and sampler), so separate models share
textures. `*.material` JSON files (`MaterialAsset`, reflection-described,
loaded through `DataAssets`) define a StandardMaterial; a glTF material
uses one when its `material` extras property names it or when
`materials/<name>.material` exists. Each file is one shared `Material`,
updated in place on reload.
**Alternatives:** per-model textures (the old behaviour: duplicated GPU
memory); swapping materials in game code after instantiating (each model
still loads its own textures first, and static renderers need refreshing);
a material library inside each glTF (Blender would have to repeat it per
file); matching only by explicit property (every Blender material would
need it set by hand).
**Why:** a texture used by many buildings is loaded once, and a surface type
is tuned in one file. Name matching needs no Blender work beyond consistent
material names; the property covers the exceptions. **Cost:** a file
silently overrides every glTF material of that name (logged when loaded);
the file's values replace the glTF's entirely (no merging); switching a file
used by static renderers to or from Transparent needs a level reload.

