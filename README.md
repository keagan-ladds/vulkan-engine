# vulkan-engine

A small, documented C++20 / Vulkan 1.3 game engine for atmospheric 3D
survival/crafting games, plus **Savanna**, a test scene in an African
savannah/swamp setting with a Valheim-like look.

```
engine/    ve_engine   core: loop, input, audio, assets, scene (GameObject/Component),
                       renderer (render graph, materials, CSM + point shadows), ImGui UI,
                       physics (Jolt: rigid bodies, character controller, raycasts)
effects/   ve_effects  optional add-ons: sky + volumetric clouds, volumetric fog,
                       bloom, particles, weather
sandbox/   savanna     the test game
templates/game/        starter for a new game in its own folder (no engine edits needed)
docs/      plans, architecture, roadmap/checklist, conventions, agent handoff
```

## Making your own game

Copy `templates/game` anywhere and build it against this checkout. See
[templates/game/README.md](templates/game/README.md):

```bash
cp -r templates/game ~/source/my-new-game && cd ~/source/my-new-game
cmake -S . -B build -G Ninja -DVE_ENGINE_DIR=/path/to/vulkan-engine
cmake --build build && ./build/bin/mygame
```

## Build & run

```bash
sudo apt install build-essential cmake ninja-build libvulkan-dev glslc \
                 vulkan-validationlayers libsdl2-dev libglm-dev
cmake -S . -B build -G Ninja      # Debug (Vulkan validation on)
cmake --build build
./build/bin/savanna
```

Textures are cooked into GPU-ready files (`build/bin/cooked/`) the first
time they load; `./build/bin/vecook` cooks everything at once (see
[docs/03-conventions.md](docs/03-conventions.md#import-assets)).

Smoke tests without a window on screen (offscreen rendering):
`VE_HEADLESS=1 SAVANNA_AUTOTEST=1 ./build/bin/savanna` (or `SAVANNA_EDITORTEST=1`).
Headless still needs a display server for SDL's Vulkan window: on a machine
without one, prefix `xvfb-run -a`. No GPU? Mesa's `mesa-vulkan-drivers`
(lavapipe) renders on the CPU. Performance: `VE_BENCH=120,30` averages CPU and
per-pass GPU times over 120 frames after 30 warm-up frames, logs them and quits
(see [docs/04-agent-handoff.md](docs/04-agent-handoff.md#4-verification-toolkit)).

### Windows

Install:
- the **[Vulkan SDK](https://vulkan.lunarg.com/sdk/home#windows)** (headers, loader, `glslc`, validation layers; the installer sets `VULKAN_SDK`);
- **Visual Studio 2022** (the "Desktop development with C++" workload) or its Build Tools;
- **CMake 3.26+** (Visual Studio ships one) and Git;
- optionally Python 3 with Pillow, only to regenerate the sandbox assets.

SDL2 needs nothing: if CMake doesn't find an SDL2 package (vcpkg, a development
package on `CMAKE_PREFIX_PATH`), it builds SDL2 from source and links it
statically. The other libraries are fetched as on Linux.

```powershell
cmake --preset vs2022                          # a Visual Studio solution in build-vs/
cmake --build --preset vs2022-debug            # or vs2022-release
.\build-vs\bin\savanna.exe
```

Or open the folder in Visual Studio 2022 and pick the `debug` / `release` preset
(Ninja). Test hooks work the same way, for example
`$env:SAVANNA_AUTOTEST=1; .\build-vs\bin\savanna.exe` in PowerShell.

**VS Code with MSVC:** pick a "(MSVC Debug)" / "(MSVC Release)" configuration
in Run and Debug (needs the C/C++ extension). It builds with the `vs2022`
preset into `build-vs\` and runs under Visual Studio's debugger: symbols go in
a separate `.pdb`, so the Debug `.exe` stays small and the debugger starts
quickly. The other F5 configurations use GCC and gdb (MinGW-w64 section below).

#### MinGW-w64 / GCC (w64devkit) instead of Visual Studio

A MinGW-w64 GCC toolchain works too, for example
[w64devkit](https://github.com/skeeto/w64devkit), which ships GCC, CMake and
Ninja in one folder. Put its `bin` on `PATH` and install the Vulkan SDK as above
(nothing else is needed). Then use the Ninja presets and name the compilers, so
CMake doesn't pick up Visual Studio's if that is also installed:

```powershell
cmake --preset debug -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++   # or --preset release
cmake --build --preset debug                                            # or release
.\build\bin\savanna.exe                                                 # build-release\ for release
```

- The executable links the GCC runtime statically, so it runs without MinGW DLLs
  next to it; it only needs `vulkan-1.dll`, which the GPU driver installs.
- CMake 4 (which w64devkit ships) is fine: the build raises old fetched
  libraries' `cmake_minimum_required` for you (`CMAKE_POLICY_VERSION_MINIMUM`).
- A build folder can't switch generators: if `build/` was configured with
  Visual Studio before, delete it first.
- Open a new terminal after installing the Vulkan SDK so `VULKAN_SDK` is set.
- **VS Code:** the F5 configurations work as on Linux (same names, CMake +
  Ninja + GCC, debugged with w64devkit's `gdb.exe`; needs the C/C++ extension).
  Start VS Code from a terminal where `gcc`, `cmake`, `ninja`, `gdb` and
  `VULKAN_SDK` are available, or put w64devkit's `bin` on the user `PATH`.

Games built with `ve_setup_game` get a manifest that makes UTF-8 the process
code page (Windows 10 1903 or later), so paths with any characters work as on
Linux. A shared SDL2 DLL, if one was found, is copied next to the executable.

Or in VS Code press **F5** and pick a configuration: Sandbox (the game) or Editor (starts in the in-engine editor; F2 toggles it anywhere), each in Debug or Release, plus the Debug Autotest and Editor test smoke tests.

| Key | Action |
|---|---|
| WASD, mouse | move, look |
| Shift / Space | sprint / jump |
| F | toggle fly mode (Space/Ctrl = up/down) |
| E or left click | pick up sunstone |
| Q or right click | throw a rock (physics) |
| V | shoot: leaves a bullet-hole decal where you aim |
| T / G (hold) | fast-forward / rewind time of day |
| R | next weather (clear → hazy → overcast → rain) |
| Tab | release/capture mouse |
| F1 | engine overlay: stats, per-pass GPU timings, live settings |
| H | hide help · Esc quit |

## Docs

Start with [docs/README.md](docs/README.md). The roadmap
([docs/02-roadmap.md](docs/02-roadmap.md)) is the checklist of what's done and
what's next. [docs/04-agent-handoff.md](docs/04-agent-handoff.md) explains how
to continue the work.
