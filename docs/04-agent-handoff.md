# Agent Handoff: how to continue this project

This file is for whoever (human or AI agent) picks up the roadmap next. Read
it fully before changing code.

## 1. Orientation (10 minutes)

1. Read [00-vision.md](00-vision.md) (goals and non-goals) and
   [01-architecture.md](01-architecture.md) (how the pieces fit).
2. Read the **Status snapshot** and **Known issues** in [02-roadmap.md](02-roadmap.md).
3. Skim [03-conventions.md](03-conventions.md); the recipes show the intended extension points.
4. Read [decisions.md](decisions.md) before changing anything architectural.
   Don't undo a decision without adding a new entry explaining why.

## 2. Build & run

Requirements (Ubuntu): `sudo apt install build-essential cmake ninja-build libvulkan-dev glslc vulkan-validationlayers libsdl2-dev libglm-dev python3-pil`.
Third-party libraries are downloaded automatically by CMake on the first configure.

```bash
cmake -S . -B build -G Ninja                 # Debug + validation layers
cmake --build build
./build/bin/savanna

cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

**Windows:** the Vulkan SDK + Visual Studio 2022 (or Build Tools) + CMake 3.26;
`cmake --preset vs2022 && cmake --build --preset vs2022-debug` (see the
README). SDL2 is built from source when no SDL2 package is found. MinGW-w64
also builds the engine (cross-compiling from Linux works: that is how the
Windows build is checked without a Windows machine; the executable links its
C++ runtime statically).

Assets are generated (they are committed, but you can regenerate them):
`python3 tools/generate_assets.py`.

## 3. The work loop

For each roadmap item:

1. **Plan:** note which modules it touches. Prefer adding a generic extension
   point in the engine and putting content in `effects/` or `sandbox/`
   (dependency rule: the engine never includes effects or sandbox code).
2. **Implement** with comments explaining the why (this is a learning codebase).
   **Document every public declaration you add or change** in headers
   (`///` above it, `///<` after a member): what it does in the caller's
   terms, units, ranges, what null / 0 / negative mean, and `@param` for any
   parameter whose name doesn't say it all (docs/03-conventions.md, "Code
   documentation"). `python3 tools/check_docs.py --changed` must report 0.
3. **Build warning-free**: `cmake --build build 2>&1 | grep -E "warning|error"` must print nothing
   (GCC and Clang, Debug and Release), and `python3 tools/check_layering.py` must pass.
4. **Verify** (§4). Validation output must stay empty.
5. **Exercise it in the sandbox**: every engine feature should be used by the
   test scene, ideally reachable by a test hook or the `AutoTest`.
6. **Update docs**: tick the roadmap box, update the architecture if anything
   structural changed, and add a decision entry for non-obvious choices.

## 4. Verification toolkit

All of these run with Vulkan validation on in Debug builds. Any
`[vulkan] Validation Error/Warning` line is a bug.

| Check | Command |
|---|---|
| Header documentation | `python3 tools/check_docs.py --changed` (headers this branch touched; exit 1 if anything is undocumented), `python3 tools/check_docs.py` (whole codebase), `cmake --build build --target docs` (HTML reference in `build/docs/html`). Needs `doxygen` |
| Smoke test (pickup, weather, exits by itself) | `cd build/bin && SAVANNA_AUTOTEST=1 ./savanna` → `[autotest]` lines, exit code 0 |
| Multiplayer (host + client processes) | `tools/nettest.sh build-release/bin /tmp/net` → `[nettest host]` / `[nettest client]` lines, exit 0; screenshots `net-host.png`, `net-client.png` |
| Title screen | `VE_HEADLESS=1 SAVANNA_MENU=1 VE_CAPTURE=title.png VE_CAPTURE_FRAME=60 VE_BENCH=4,62 ./savanna` |
| Specific view | `SAVANNA_TIME=21.3 SAVANNA_WEATHER=3 SAVANNA_POSE=36,4,22,60,-8 SAVANNA_DEBUG=1 ./savanna` |
| Performance | `SAVANNA_DEBUG=1` → F1 overlay shows FPS, total GPU ms and **per-pass GPU ms** |
| Release package | `build/bin/vecook --package <dir>` (`--loose` for folders); run the packaged game from `<dir>` to check nothing reads sources |
| Cooked assets | `build/bin/vecook --check` (exit 1 if anything is stale), `build/bin/vecook` cooks; `VE_COOK=0` / `read` / `sync` pick source images, cooked only, or cook-before-load |
| Engine still usable by games | `cp -r templates/game /tmp/g && cmake -S /tmp/g -B /tmp/g/build -G Ninja -DVE_ENGINE_DIR=$PWD && cmake --build /tmp/g/build && /tmp/g/build/bin/mygame` |
| Windows builds (CI) | `.github/workflows/windows.yml` runs on every push to `main` / `claude/**` and on pull requests: MinGW-w64 GCC (+ the game template) and MSVC 2022 must build with zero first-party warnings. Read the run's job logs on GitHub (Actions tab) |
| Clean shutdown | send SIGTERM (`timeout 10 ./savanna`): SDL turns it into a normal quit, so the whole shutdown path runs |
| Benchmark | `VE_HEADLESS=1 VE_BENCH=120,30 SAVANNA_TIME=17.6 SAVANNA_WEATHER=0 ./savanna` (Release) → `[bench]` lines: main-thread CPU ms, frame ms, physics step, total and per-pass GPU ms, then exit |
| Screenshot | `VE_HEADLESS=1 VE_CAPTURE=shot.png VE_CAPTURE_FRAME=60 ./savanna` (any test hook works alongside) |
| Consecutive frames (flicker / jitter checks) | add `VE_CAPTURE_COUNT=8`: saves `shot_0.png` … `shot_7.png`; with `VE_FIXED_DT=0.016667` each frame advances 1/60 s however slow the software renderer is |

**Machines without a display or GPU** (CI, cloud containers): install
`xvfb mesa-vulkan-drivers` and prefix commands with `xvfb-run -a`. Lavapipe
renders Vulkan 1.4 on the CPU: correct images and validation, but frames take
~0.5–1 s, and its worker threads share the cores, so judge CPU changes by the
benchmark's *main thread* time and GPU changes by per-pass time. Compare two
builds by alternating runs (A, B, A, B): absolute numbers drift with load.

Screenshots without a display server tool: capture the window with
`xwd -name "Savanna - vulkan-engine test scene" -silent -display :0 > shot.xwd`
and convert it with a few lines of Pillow (XWD header: 100 bytes big-endian
uint32s, then colormap entries of 12 bytes each, then BGRX pixels).

Weather values: 0 Clear, 1 Hazy, 2 Overcast, 3 Rain. Pose yaw: 0 looks
north (-Z), 90 looks west (-X).

## 5. Gotchas (learned the hard way)

- **Static renderers need `refresh()` after code changes their fields**
  (mesh, materials, visible, castShadows, enabled): they're GPU instances
  and nothing re-reads them per frame. Transform, `setActive` and destroy are
  tracked automatically.
- **Remember other objects with `ve::ObjectRef<T>` / `ve::ComponentRef<T>`**
  (`ve/scene/ObjectRef.hpp`), never a `GameObject*` / `Component*` member.
  The editor can delete or replace any object; a stored raw pointer then
  dangles, and even `isDestroyed()` reads freed memory. Resolve with
  `ref.get(scene())` where you use it (O(1)).
- **Includes follow the module layering**: `python3 tools/check_layering.py`
  must pass. Need something from a higher module? Add an extension point
  (a registration or callback the composition root wires up), don't include it.

- **`GpuTypes.hpp` ↔ `ve.glsl` must match byte for byte.** If you add a field,
  add it in both places, keep 16-byte alignment, and update `fillGlobals()`.
- **Push-constant structs** in C++ must match the GLSL `push_constant` block
  (std430 layout: `glm::vec2` is 8-byte aligned in GLSL, but only 4 in C++ by
  default; order fields so vec4s come first).
- **Bindless slots and frames in flight:** never free or reuse a texture slot
  immediately. `Bindless::removeTexture` already defers.
- **Every pass that samples the shadow maps must declare them**
  (`Access::Sampled` on `dirShadowMap()`/`pointShadowMap()`), or the graph won't
  transition them out of the depth-attachment layout.
- **Depth used for testing and sampling at once** → declare both
  `Access::DepthRead` and `Access::Sampled` (both map to `READ_ONLY_OPTIMAL`).
- **Shutdown order:** the scene and assets release GPU resources through deferred
  deletion. `Renderer::shutdown` clears the bindless fallback, then flushes
  deletions *before* destroying the bindless set. Keep that order.
- **Fixed vertex format:** shaders that ignore attributes are fine. The
  validation layer's `Shader-OutputNotConsumed` perf warning is filtered on purpose.
- **Mesh pipelines have 2 colour attachments** (SceneColor + SceneNormal). Any
  pass that draws scene materials must bind both (`TransparentPass` does, and
  leaves the normal target unwritten). Otherwise pipeline/rendering attachment
  counts mismatch. Full-screen passes are unaffected.
- **FoliageSystem and windowed-grass callbacks run as jobs on the engine pool,
  several at once.** `FoliageWorld::height`, `normal`, every
  `FoliageLayer::density` and `GrassDesc` height / mask callbacks must only
  read immutable data and own what they capture (by value or `shared_ptr`):
  a cancelled job may still be finishing after its owner is gone. Jobs
  (`ve/core/Jobs.hpp`) never touch the scene, renderer, device or audio.
  Add layers and exclusions before the first `update()`/`preload()`.
- **Don't call a base class's empty hook** (`ve::GameObject::update(dt)`,
  `ve::Component::onGui()`) from an override. The base versions mark the hook
  as "not overridden" and the Scene stops calling it for that object.
- **glm + GCC "most vexing parse":** `glm::mat3 m(glm::vec3(x[0]) / s, ...)` can
  parse as a declaration. Use brace initialisation.
- **ImGui**: `ImGui::Render()` is called inside `UiPass::execute`. If a frame
  is skipped (minimized), `UiLayer::endFrameIfNotRendered()` ends it.
- **CMake used from a parent project:** imported targets (`Vulkan::glslc`,
  ...) are only visible in the directory that created them. Anything called from
  a game's CMakeLists (`ve_compile_shaders`, `ve_setup_game`) must use cache
  variables (`Vulkan_GLSLC_EXECUTABLE`, `VE_OUTPUT_DIR`, ...) instead. Re-run the
  template check above after touching `cmake/`.
- **Jolt stays inside `ve/physics/*.cpp`** (shared through the internal
  `JoltInternal.hpp`; the layering check enforces it). Don't include Jolt headers
  anywhere else: its SIMD flags/defines are only set up for `ve_engine`, and
  keeping them private means games never depend on Jolt.
- **Physics order per fixed step:** `Scene::fixedUpdate` (characters move,
  forces applied) → `PhysicsWorld::step` → after all steps `interpolate()`. A
  `CharacterController::move()` called from `update()` would run at the wrong rate.
- **Testing aim:** a ray (or thrown rock) through the exact centre of the crate
  pyramid passes through the 2 cm gap between crates. That cost a debugging
  session; aim off-centre in tests.
- **Read files through `ve::vfs`** (`core/FileSystem.hpp`), never `std::ifstream` / `std::filesystem::exists` on game data: packaged games keep everything in `data.vepak`.
- **Normal maps: `veUnpackNormal(texel)`**, never `texel.xyz * 2 - 1`: cooked
  normal maps are BC5 and their B channel reads 0.
- **Textures load the way their file says** (`TextureSettings`: name or
  `.texture` file), never as the caller asks: a new data texture with a
  colour-sounding name needs a `.texture` file or a better name.
- **Vertex shaders must use `veModelMatrix()`**, never `draw.model`, or
  instanced draws render every instance at the origin. The helper only
  exists in `.vert` files (`VE_VERTEX_STAGE`, set by CMake).
- **Instancing buffers are shared across frames in flight**; ordering
  comes from the memory barriers in `InstanceSystem::recordCulling` (previous
  frame's indirect/vertex reads → this frame's transfer/compute writes).
  Keep those barriers if you change the pass order.
- **Effects need their order:** `VolumetricFogPass` needs `AtmospherePass`
  (SkyViewLUT and noise). `vfx::addDefaultEffects` adds them in the right order.

## 6. Where to start next

1. **F2 Animation**: skinned glTF characters for enemies and bosses.
2. **Physics follow-ups** (Phase 11 open items): collision/trigger callbacks
   (contact listener → `Component::onCollisionEnter`), collider debug drawing,
   shape casts for melee/building placement.
3. **Cutout shadows** (Known issue 1): quick win; needed for foliage.
4. **Temporal AA + reprojection** to clean up clouds/fog and allow fewer steps.

When an item is done, update the **Status snapshot** date and numbers in the roadmap.
