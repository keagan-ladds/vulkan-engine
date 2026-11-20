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
