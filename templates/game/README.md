# Game template

A minimal game built on vulkan-engine **without modifying the engine**: it
pulls the engine in with `add_subdirectory` and only uses its public API.

## Start a new game

```bash
cp -r /path/to/vulkan-engine/templates/game ~/source/my-new-game
cd ~/source/my-new-game
cmake -S . -B build -G Ninja -DVE_ENGINE_DIR=/path/to/vulkan-engine
cmake --build build
./build/bin/mygame
```

Then:

1. Rename `mygame` in `CMakeLists.txt` (target name) and `.vscode/launch.json`
   (program path), and set `config.title` in `src/main.cpp`.
2. Either edit the `VE_ENGINE_DIR` default in `CMakeLists.txt` or keep passing
   `-DVE_ENGINE_DIR=...`. The value is cached in `build/`, so you only pass it once.
3. Open the folder in VS Code and press F5.

## Pin an engine version

Build against a tagged engine version and update on purpose, so engine
changes can't break your game unannounced (see the engine's `CHANGELOG.md`
and `docs/09-api-stability.md`). The simplest way is a git submodule:

```bash
git submodule add https://github.com/keagan-ladds/vulkan-engine external/vulkan-engine
git -C external/vulkan-engine checkout v0.1.0
git commit -am "Engine v0.1.0"
```

and point `VE_ENGINE_DIR` at it (the default in `CMakeLists.txt`):
`set(VE_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/vulkan-engine" ...)`.
Clones then need `git clone --recursive` (or `git submodule update --init`).

To update: read the changelog entries since your version, then
`git -C external/vulkan-engine fetch --tags && git -C external/vulkan-engine checkout v0.2.0`,
rebuild, and fix any deprecation warnings (deprecated APIs keep working for
one minor version).

## Layout

```
src/        your C++ (any .cpp here is compiled; re-run CMake configure after adding files)
shaders/    your GLSL (.vert/.frag/.comp), compiled to build/bin/shaders/*.spv
            #include "ve.glsl" / "ve_lighting.glsl" / "vfx.glsl" work out of the box
assets/     copied to build/bin/assets on every build; load with assets().loadTexture("textures/x.png")
```

## What the example shows

| File | Feature |
|---|---|
| `src/main.cpp` | Engine setup, optional effects, opt-in screen-space GI / reflections / depth of field, sun coloured by the atmosphere, PBR `StandardMaterial` with a texture, a custom-shader material, point light with shadows, particles, physics (static ground/spheres, kinematic spinning cubes, falling dynamic boxes) |
| `src/World.*` | GPU grass (`ve::GrassPass`, with shadows; cards commented) and rule-based streamed trees/rocks (`ve::FoliageSystem`) with LOD chains, an impostor, colliders, groves and avoid rules |
| `src/CameraController.*` | A `GameObject` with behaviour (`update`, `onGui`) and a `Camera` component |
| `src/Spinner.hpp` | A reusable `Component` |
| `src/SunController.hpp` | The sun as a `GameObject`: arrow keys change its direction/height; colour from the atmosphere |
| `shaders/mygame_toon.frag` | Custom (non-PBR) lighting using the engine's lights and shadows |

## Notes

- **Shader names are global.** The engine finds shaders by file name, so don't
  reuse the engine's names (`standard.vert`, `sky.frag`, ...). Prefix yours (`mygame_`).
- **No effects?** Remove the `vfx::addDefaultEffects` call and add `NO_EFFECTS` to
  `ve_setup_game(...)` in `CMakeLists.txt`. Without the atmosphere you'll want to
  set `renderer.environment()` ambient colours and a clear colour yourself.
- **Updating the engine:** pull the engine repo and rebuild. If the update
  changes APIs, its `docs/` will say so.
- **Screen-space passes** (GI, reflections, DOF) are optional: delete their
  `addPass` lines to drop them. GI/reflections need opaque shaders to write the
  thin G-buffer (`outGBuffer = veGBuffer(N, roughness)`, see the toon shader).
- **Foliage callbacks run on a worker thread**: `FoliageWorld::height` and layer
  `density` functions must only read data that doesn't change.
- Engine documentation and recipes live in the engine repo: `docs/03-conventions.md`
  (custom materials, post-process passes, components, global shader params).
