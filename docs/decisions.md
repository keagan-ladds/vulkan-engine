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
