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
| `src/main.cpp` | Engine setup, optional effects, sun coloured by the atmosphere, PBR `StandardMaterial` with a texture, a custom-shader material, point light with shadows, particles |
| `src/CameraController.*` | A `GameObject` with behaviour (`update`, `onGui`) and a `Camera` component |
| `src/Spinner.hpp` | A reusable `Component` |
| `shaders/mygame_toon.frag` | Custom (non-PBR) lighting using the engine's lights and shadows |

## Notes

- **Shader names are global.** The engine finds shaders by file name, so don't
  reuse the engine's names (`standard.vert`, `sky.frag`, ...). Prefix yours (`mygame_`).
- **No effects?** Remove the `vfx::addDefaultEffects` call and add `NO_EFFECTS` to
  `ve_setup_game(...)` in `CMakeLists.txt`. Without the atmosphere you'll want to
  set `renderer.environment()` ambient colours and a clear colour yourself.
- **Updating the engine:** pull the engine repo and rebuild. If the update
  changes APIs, its `docs/` will say so.
- Engine documentation and recipes live in the engine repo: `docs/03-conventions.md`
  (custom materials, post-process passes, components, global shader params).
