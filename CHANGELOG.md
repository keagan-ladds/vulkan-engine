# Changelog

What changed in each tagged version of vulkan-engine, for people building a
game on it. The rules for what may change between versions are in
[docs/09-api-stability.md](docs/09-api-stability.md).

Every breaking change to a stable API gets an entry under **Breaking**, with
what to change in game code. Games should pin a version (a tag) and update on
purpose: see "Pin an engine version" in [templates/game/README.md](templates/game/README.md).

## [Unreleased]

Nothing yet.

## [0.1.0] - 2026-11-22

The first tagged version: the baseline games build against.

### What's in it
- **Core:** the main loop with a fixed physics step, input actions and axes
  (keyboard, mouse, gamepads, rebinding), CPU profiler zones, one engine job
  pool, loading screens, environment switches (`ve::env`), benchmark and
  headless capture modes.
- **Scene:** GameObjects with components (Unity-style hybrid), lifecycle hooks
  that cost nothing unless overridden, typed handles for remembered objects
  (`ObjectRef`, `ComponentRef`), reflection for the editor and scene files,
  prefabs, save games, scene files.
- **Rendering (Vulkan 1.3):** bindless materials, a render graph, cascaded and
  point-light shadows with face caching, clustered lights, temporal AA, GPU
  instancing with LODs, impostors and occlusion culling, static renderers as
  GPU instances, GPU skinning, asynchronous buffer uploads, shader
  hot-reload, a pipeline cache.
- **Effects (optional module):** sky and volumetric clouds, volumetric fog,
  bloom, particles, fire, weather, day/night, water, colour grading,
  screen-space GI and reflections, depth of field, photo mode.
- **World:** streamed procedural terrain with biomes and stamps, foliage
  layers with promotion to interactive objects, GPU grass, LOD chain building.
- **Gameplay:** interaction ("press E"), building placement with snap points,
  placement boxes and fittings, marker nodes in imported models
  (`SNAP_*`, `BOX_*`, `EXCL_*`), inventory, proxy sets (thousands of simple
  things as data, promoted near the player).
- **Physics (Jolt):** rigid bodies, character controller, joints, collision
  layers and surfaces, raycasts and shape casts.
- **Animation:** skinned glTF, crossfades, layers, events, two-bone, foot,
  look-at and aim IK.
- **Navigation:** tiled navmesh building and agents (Recast / Detour).
- **Audio:** 3D sources, buses, zones with muffling, music moods, ambience.
- **UI:** retained-mode game UI (`ve::ui`), ImGui debug overlay.
- **Networking:** host-authoritative co-op foundation (sessions, object and
  transform replication, requests to the host).
- **Editor:** hierarchy, inspector, gizmos, undo, terrain tools, prefabs,
  Play / Stop.
- **Platforms:** Linux (GCC, Clang) and Windows (MinGW-w64 GCC, Visual
  Studio 2022), checked by CI on Windows.
- **Tooling:** `tools/check_layering.py` enforces the module layering; the
  sandbox's autotest, editor test and two-process multiplayer test.

### Known limitations
See [docs/09-api-stability.md](docs/09-api-stability.md) for the areas still
expected to change (multiplayer gameplay through the host, world streaming)
and the roadmap's known issues.
