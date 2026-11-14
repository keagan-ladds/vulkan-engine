# Vision

## What

A small, hackable C++20 / Vulkan 1.3 game engine for building 3D survival/crafting
games, plus a test scene that proves the engine out.

The first game target is a **Valheim-like** survival game (gather, craft, build,
then fight enemies and bosses) set in an **African savannah / jungle / swamp**
in the spirit of *Far Cry 2*. The aesthetic follows Valheim: low-poly geometry,
simple textures, and a lot of atmosphere from lighting. That means golden-hour
light, volumetric fog and god rays, dramatic skies with volumetric clouds,
bloom, particles and weather.

## Goals

1. **Generic engine, game-specific content.** Nothing about savannahs, sunsets or
   weather is hard-coded in the engine. The engine provides *mechanisms*
   (render passes, materials, components, particles); the game and the optional
   effects module provide *content*.
2. **Unity-like gameplay programming.** Derive from `ve::GameObject` (or
   `ve::Component`) and override `start()`, `update(dt)`, `fixedUpdate(dt)`,
   `lateUpdate(dt)`, `onGui()`, `onDestroy()`.
3. **Data-driven rendering.** Meshes use a fixed vertex/index format, but each
   mesh renders with its own `Material` → `Shader` (custom GLSL). The engine
   builds pipelines, sorts, culls and draws. Every shader sees the same global
   data (camera, time, lights, shadows, materials, bindless textures) through
   one shared include, `ve.glsl`.
4. **Extensible frame.** Arbitrary render passes (full-screen post-process,
   geometry, compute) can be added to a render graph that handles image
   layouts/barriers and screen-sized resources automatically.
5. **Shadows:** a configurable number of shadowed **directional** lights
   (cascaded shadow maps, configurable cascade count) and shadowed **point**
   lights (cube shadow maps).
6. **Batteries for games:** input (keyboard/mouse with action bindings), audio
   (2D/3D via miniaudio), basic UI (Dear ImGui for HUD + debug tools), asset
   import (PNG/JPG/TGA/HDR textures, glTF 2.0 models, WAV/MP3/FLAC/OGG audio).
7. **Readable & documented.** The code is a learning resource. Comments explain *why*.

## Non-goals (for now)

- A visual editor, scene serialization format, scripting language.
- Multi-threaded rendering / job system (single-threaded is fine at this scale).
- Platform abstraction beyond what SDL2 gives us (Linux first; Windows should
  work, macOS/MoltenVK untested).
- Physically exact sky. "Looks like an African sunset" beats "is correct".

## Test scene ("Savanna")

The `sandbox/` executable is the engine's integration test and demo:

- A procedurally generated savannah terrain with a swamp basin and water, rocky
  outcrops, acacia and baobab trees, and wind-blown grass. The terrain, grass and
  water use **custom shaders**.
- A rondavel hut **imported from glTF**. Textures come from PNG via stb_image.
- A day/night cycle starting at sunset. The sun and moon are two shadowed
  directional lights with cascades.
- A campfire and torches: shadowed point lights with flicker, fire and smoke
  particles, and 3D crackle audio.
- Volumetric clouds, volumetric fog with god rays, bloom, and switchable weather
  (clear / overcast / rain).
- First-person player with walking, sprinting, jumping (`fixedUpdate` gravity) and
  a fly mode. Collectible glowing stones (pickup sound, HUD counter).

### Controls

| Key | Action |
|---|---|
| WASD / mouse | move / look |
| Shift | sprint |
| Space | jump (fly mode: up) |
| Ctrl | fly mode: down |
| F | toggle fly mode |
| E / left click | pick up a sunstone you're looking at |
| T / G (hold) | fast-forward / rewind the time of day |
| R | cycle weather (clear → hazy → overcast → rain) |
| Tab | release/capture mouse |
| F1 | debug UI (render passes + GPU timings, settings, stats, hierarchy) |
| H | hide help text |
| Esc | quit |
