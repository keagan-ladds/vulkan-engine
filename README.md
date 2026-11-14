# vulkan-engine

A small, documented C++20 / Vulkan 1.3 game engine for atmospheric 3D
survival/crafting games, plus **Savanna**, a test scene in an African
savannah/swamp setting with a Valheim-like look.

```
engine/    ve_engine   core: loop, input, audio, assets, scene (GameObject/Component),
                       renderer (render graph, materials, CSM + point shadows), ImGui UI
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

Or in VS Code press **F5** (Debug / Release / Debug autotest).

| Key | Action |
|---|---|
| WASD, mouse | move, look |
| Shift / Space | sprint / jump |
| F | toggle fly mode (Space/Ctrl = up/down) |
| E or left click | pick up sunstone |
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
