# Getting started: a game on the prebuilt engine

This walks you from nothing to a running game that links a **prebuilt**
vulkan-engine. Your game is its own folder and its own repository: it never
compiles the engine and never edits anything inside it.

- [1. What you need](#1-what-you-need)
- [2. Get a prebuilt engine](#2-get-a-prebuilt-engine)
- [3. Make the game project](#3-make-the-game-project)
- [4. Build and run](#4-build-and-run)
- [5. Your first gameplay code](#5-your-first-gameplay-code)
- [6. Levels in the editor](#6-levels-in-the-editor)
- [7. Assets and shaders](#7-assets-and-shaders)
- [8. Ship it](#8-ship-it)
- [9. Upgrade the engine](#9-upgrade-the-engine)
- [10. Troubleshooting](#10-troubleshooting)

If you'd rather build the engine from source together with your game (easier
when you're changing the engine too), see `templates/game/README.md`: the same
template does both.

## 1. What you need

| | |
|---|---|
| **Vulkan SDK** | Its `glslc` compiles your shaders. Windows / macOS: [LunarG installer](https://vulkan.lunarg.com/sdk/home) (sets `VULKAN_SDK`; open a new terminal after). Ubuntu: `sudo apt install libvulkan-dev glslc vulkan-validationlayers` |
| **CMake 3.26+ and Ninja** | Or Visual Studio's CMake on Windows |
| **The same compiler as the package** | MSVC packages need MSVC, MinGW GCC packages need MinGW GCC, Linux GCC/Clang packages need that compiler. Static C++ libraries don't mix between compilers |
| **SDL2** (Linux only, optional) | `libsdl2-dev` if the package was built against the system SDL2; otherwise nothing |

## 2. Get a prebuilt engine

A package is a folder (call it the *engine prefix*):

```
vulkan-engine-0.1.0/
  include/                   engine + effects headers (ve/..., vfx/...), glm, nlohmann, SDL2, imgui.h
  lib/                       static libraries (ve_engine, ve_effects, Jolt, ...)
  lib/cmake/VulkanEngine/    find_package() files, ve_setup_game(), ve_compile_shaders()
  bin/vecook                 the asset cooker / packager
  share/vulkan-engine/       the engine's compiled shaders and GLSL includes (ve.glsl...)
```

**Option A: download one (Windows, MSVC).** Every Windows CI run uploads the
`vulkan-engine-msvc-release` artifact (Actions tab > a green "Windows build"
run > Artifacts). Unzip it anywhere, e.g. `C:\sdk\vulkan-engine-0.1.0`. It's a
Release build: build your game in Release (or RelWithDebInfo) too.

**Option B: build and install one** from an engine checkout, at the version you
want:

```bash
git clone https://github.com/keagan-ladds/vulkan-engine
cd vulkan-engine
git checkout v0.1.0                           # a tagged version (recommended)

cmake --preset release
cmake --build --preset release
cmake --install build-release --component VulkanEngine --prefix ~/sdk/vulkan-engine-0.1.0
```

Windows with MSVC (Developer PowerShell for VS; the `msvc` preset builds into `build-vs`):

```powershell
cmake --preset msvc
cmake --build --preset msvc-release
cmake --install build-vs --config Release --component VulkanEngine --prefix C:/sdk/vulkan-engine-0.1.0
```

Want Debug games too? Build a Debug configuration and install it into the
**same** prefix: the package then holds both, and each game build type links
the matching libraries.

```bash
cmake --preset debug && cmake --build --preset debug
cmake --install build --component VulkanEngine --prefix ~/sdk/vulkan-engine-0.1.0
# MSVC: cmake --build --preset msvc-debug
#       cmake --install build-vs --config Debug --component VulkanEngine --prefix C:/sdk/vulkan-engine-0.1.0
```

> **No install step at all:** an engine build folder is a package too
> (`-DVulkanEngine_DIR=<engine>/build`). Headers then come from that
> checkout; rebuilding the engine there is all it takes for your game to pick
> up engine changes. Handy while you're working on both.

## 3. Make the game project

### The quick way: copy the template

```bash
cp -r vulkan-engine/templates/game ~/source/my-game
cd ~/source/my-game
git init
```

Then rename `mygame` in `CMakeLists.txt` (the target) and `.vscode/launch.json`,
and set `config.title` in `src/main.cpp`. The template is a working scene
(sky, PBR, a custom shader, physics, grass, trees) to delete from as you go.

### From scratch: the minimum

Three files:

```
my-game/
  CMakeLists.txt
  src/main.cpp
  assets/            (can be empty)
```

`CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.26)
project(MyGame LANGUAGES C CXX)

find_package(VulkanEngine CONFIG REQUIRED)   # ve::ve_engine, ve::ve_effects, ve_setup_game()

file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS src/*.cpp)
file(GLOB GAME_SHADERS CONFIGURE_DEPENDS shaders/*.vert shaders/*.frag shaders/*.comp)

add_executable(mygame ${GAME_SOURCES})
target_include_directories(mygame PRIVATE src)

# Links the engine (+ effects), puts the game in build/bin, compiles shaders,
# copies the engine's shaders, copies assets/ on every build, adds vecook.
ve_setup_game(mygame
    ASSETS  ${CMAKE_CURRENT_SOURCE_DIR}/assets
    SHADERS ${GAME_SHADERS})          # add NO_EFFECTS for a plain renderer
```

`src/main.cpp`:

```cpp
#include <ve/app/CookTool.hpp>
#include <ve/ve.hpp>
#include <vfx/Atmosphere.hpp>
#include <vfx/Vfx.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    // `mygame --cook ...`: the asset cooker with this game's types registered (section 8).
    if (argc > 1 && std::string(argv[1]) == "--cook") {
        argv[1] = argv[0];
        return ve::cookToolMain(argc - 1, argv + 1);
    }
    try {
        ve::EngineConfig config;
        config.title = "My Game";
        ve::Engine engine(config);
        ve::Renderer& renderer = engine.renderer();
        ve::Scene& scene = engine.scene();

        vfx::Effects fx = vfx::addDefaultEffects(renderer);   // sky, clouds, fog, bloom, particles

        auto& sun = scene.spawn("Sun");
        auto& light = sun.addComponent<ve::DirectionalLight>();   // shines along the object's forward (-Z)
        sun.transform().lookAt({-0.4f, -1.0f, -0.3f});              // a point to face (from the origin: a direction)
        fx.atmosphere->setSun(&light);

        auto groundMat = renderer.createStandardMaterial();
        groundMat->setBaseColor({0.35f, 0.42f, 0.25f, 1.0f}).setRoughness(0.9f);
        auto& ground = scene.spawn("Ground");
        ground.addComponent<ve::MeshRenderer>(ve::Mesh::create(renderer, ve::primitives::plane(100, 100)), groundMat);

        auto& camera = scene.spawn("Camera");
        camera.addComponent<ve::Camera>();
        camera.transform().setPosition({0.0f, 2.0f, 8.0f});

        engine.run();                 // until the window closes or engine.quit()
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
```

(The template's `src/main.cpp`, `CameraController` and `SunController` show
fuller versions: a fly camera and a sun you can move.)

## 4. Build and run

Point CMake at the engine prefix with `CMAKE_PREFIX_PATH`; it's cached, so you
pass it once per build folder.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=~/sdk/vulkan-engine-0.1.0
cmake --build build
./build/bin/mygame
```

Windows (Developer PowerShell for VS, or the MinGW terminal for a MinGW package):

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/sdk/vulkan-engine-0.1.0
cmake --build build
.\build\bin\mygame.exe
```

Configure prints `Prebuilt vulkan-engine 0.1.0: <prefix>/lib/cmake/VulkanEngine`
(template) when it found the package. Everything the game needs at run time
ends up in `build/bin`: the executable, `shaders/`, `assets/` and `vecook`.

**VS Code:** with the CMake Tools extension, put the prefix in
`.vscode/settings.json` (`"cmake.configureSettings": {"CMAKE_PREFIX_PATH": "..."}`)
and press F5 (the template's `launch.json` runs `build/bin/mygame`).
**Visual Studio:** File > Open > Folder, and add `CMAKE_PREFIX_PATH` to the
CMake settings (`CMakeSettings.json` or a `CMakePresets.json` of your own).

## 5. Your first gameplay code

Behaviour lives in **components** (reusable bits) and **GameObject
subclasses** (one kind of thing). Both get `start()`, `update(dt)`,
`fixedUpdate(dt)` (50 Hz) and `onGui()`.

```cpp
// src/Spin.hpp
#pragma once
#include <ve/scene/GameObject.hpp>

class Spin : public ve::Component {
public:
    float speed = 1.0f; // radians per second
    void update(float dt) override { transform().rotate({0, 1, 0}, speed * dt); }
};
```

```cpp
go.addComponent<Spin>().speed = 3.0f;
```

To place it from the editor and save it in scene files, register it with
reflection in any `.cpp` of your game:

```cpp
// src/Spin.cpp
#include "Spin.hpp"
#include <ve/scene/Reflection.hpp>

VE_REGISTER_TYPE(Spin, "Spin")
{
    t.category("My Game");
    t.field("speed", &Spin::speed).range(-10.0f, 10.0f);
}
```

It then shows up in the editor's Add Component menu, its fields in the
inspector, and it round-trips through scene and prefab files.

Input goes through named actions, so players can rebind and gamepads work:

```cpp
input().bindAction("Jump", ve::Key::Space);
input().bindAction("Jump", ve::GamepadButton::A);
if (input().actionPressed("Jump")) { /* ... */ }
```

Next stops, all in [03-conventions.md](03-conventions.md#recipes): physics,
custom materials, prefabs, UI, save games, audio, navmesh agents, foliage,
terrain, splines, building, multiplayer-ready gameplay.

## 6. Levels in the editor

Press **F2** in your game to open the editor (F2 again returns to the game).
Build the level there: Create menu, the move gizmo (W/E/R), the inspector,
terrain sculpting, splines; then **Ctrl+S** to save it as a scene file
(e.g. `scenes/level1.json`). The editor saves into your **source** `assets/`
folder, not the copy in `build/bin`, so the file belongs in your repository.
The editor's controls are in [05-editor.md](05-editor.md).

Load it at startup instead of (or as well as) spawning objects in code:

```cpp
#include <ve/scene/SceneSerializer.hpp>

ve::SceneSerializer::load(engine.scene(), "scenes/level1.json");
```

For a big level, load it behind a loading screen over several frames
(`engine.addSceneLoad(...)`, see "Loading screens" in 03-conventions.md).

## 7. Assets and shaders

```
assets/     anything the game loads: textures, glTF models, sounds, scenes, prefabs
shaders/    your GLSL (.vert / .frag / .comp), compiled to build/bin/shaders/*.spv
```

- Load by path relative to `assets/`: `engine.assets().loadTexture("textures/rock.png")`,
  `loadModel("models/hut.glb")`.
- Models: glTF from Blender (+Y up). LODs, colliders and per-model settings
  are explained under "Import assets" in 03-conventions.md.
- Shaders: `#include "ve.glsl"` / `"ve_lighting.glsl"` work out of the box.
  **Prefix your shader file names** (`mygame_toon.frag`): shaders are looked
  up by file name and must not clash with the engine's.
- Read game files through `ve::vfs` (or the AssetManager), not
  `std::ifstream`: a shipped game keeps them in an archive.
- The first run cooks assets (compressed textures, model LODs) into a cache in
  the background; later runs load the cooked copies.

## 8. Ship it

```bash
./build/bin/mygame --cook --package dist/MyGame
```

`dist/MyGame` then holds the game, its runtime libraries, the shaders and a
`data.vepak` archive of cooked assets (no source images, `.blend` files or
settings files). Run it from there to check nothing still reads source assets.

Use `mygame --cook` rather than the package's `vecook` when your prefabs use
your own types (prefab icons are rendered with them). `--cook --check` lists
assets whose cooked copies are missing or stale. More in 03-conventions.md
("Packaging a release").

## 9. Upgrade the engine

1. Read the [CHANGELOG](../CHANGELOG.md) entries between your version and the
   new one ("Breaking" says what to change in game code).
2. Get or install the new package into a **new** prefix
   (`~/sdk/vulkan-engine-0.2.0`).
3. Re-configure with the new path: `cmake -S . -B build -DCMAKE_PREFIX_PATH=~/sdk/vulkan-engine-0.2.0`.
4. Build and fix any deprecation warnings: deprecated APIs keep working for
   one minor version ([09-api-stability.md](09-api-stability.md)).

Keeping the prefix per version means you can go back by pointing at the old
folder.

## 10. Troubleshooting

| Problem | Fix |
|---|---|
| `Could not find a package configuration file provided by "VulkanEngine"` | `CMAKE_PREFIX_PATH` must be the prefix (the folder holding `lib/` and `include/`), not `lib/cmake/...`. Delete `build/CMakeCache.txt` after fixing a wrong path |
| `...was built with MSVC, this project uses GNU` (or similar) | Build the game with the package's compiler (on Windows: the Developer PowerShell for MSVC packages, the MinGW terminal for MinGW ones) |
| Link errors about `_ITERATOR_DEBUG_LEVEL` / runtime mismatch (MSVC) | Your build type has no libraries in the package. Use Release with a Release-only package, or install a Debug build into the same prefix |
| `Could NOT find Vulkan (missing: glslc)` | Install the Vulkan SDK and open a new terminal (`VULKAN_SDK` must be set) |
| Black screen / "can't find shader mygame_x.frag" | The shader isn't in `GAME_SHADERS` (re-run configure after adding a file) or its name clashes with an engine shader |
| A type is missing in the editor or a scene file says "unknown type" | It needs `VE_REGISTER_TYPE` in a `.cpp` that's part of the game executable |
| Changes saved in the editor vanish on the next build | They were saved outside the source `assets/` folder: use the editor's save (it writes into `assets/`), and don't edit `build/bin/assets` by hand |

Stuck on something else: the engine's `docs/` (start at
[README.md](README.md)) and the header comments (`include/ve/...`) document
every public API.
