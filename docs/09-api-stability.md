# API stability: what a game can rely on

The engine is pre-1.0, but games are being built on it. This page says which
parts are settled, which are still moving, and how changes are made, so game
code doesn't have to be rewritten because the engine changed under it.

## Versions

Versions are git tags `vMAJOR.MINOR.PATCH`, listed in [CHANGELOG.md](../CHANGELOG.md).

- **Patch** (0.1.0 → 0.1.1): fixes only. No API or file format changes.
- **Minor** (0.1 → 0.2): new features. May contain breaking changes to the
  *stable* APIs below, but only when unavoidable, each one listed in the
  changelog under **Breaking** with what to change in game code. Where
  practical the old API stays for one minor version, marked
  `[[deprecated("use ...")]]`, so the compiler points at what to update.
- **1.0**: when the areas marked "evolving" below have settled.

Games pin a tag (see "Pin an engine version" in `templates/game/README.md`)
and update deliberately: read the changelog, update, build, fix the
deprecation warnings.

## Stable

Change only as described above. Recipes in [03-conventions.md](03-conventions.md)
use these, and they're what game code should use.

| Area | Headers / APIs |
|---|---|
| Scene model | `GameObject`, `Component`, the lifecycle hooks, `Scene` (`spawn`, `find*`, `forEach`, `findById`), `Transform`, `ObjectRef` / `ComponentRef` |
| Data | Reflection (`VE_REGISTER_TYPE`, `TypeBuilder` fields), scene / prefab / save-game files (versioned; old files keep loading), `ve::save`, `Saveable`, `prefab::instantiate` |
| Input | `Input` actions, axes, gamepads, rebinding |
| Physics | `Rigidbody`, `Collider`, `CharacterController`, joints, collision layers / surfaces, `PhysicsWorld` queries |
| Rendering for games | `MeshRenderer` (incl. `isStatic` / `refresh`), `Camera`, lights, `StandardMaterial`, custom materials and passes (`RenderPass`, `FullscreenPass`), `InstanceGroup`, `ve::buildLodChain`, `RendererSettings` |
| Assets | `AssetManager` loads, `ModelInstance` (incl. marker nodes), `PrimitiveRenderer` |
| Gameplay helpers | `Interactable` / `Interactor`, placement (`SnapPoint`, `PlacementBox`, `PlacementPreview`, `findSnap`), `Inventory`, `ProxySet` (single-player behaviour) |
| Animation, audio, UI | `Animator`, IK components, `AudioSource` / buses / zones / music / ambience, `ve::ui` |
| World | `Terrain`, foliage layers, grass, `FoliageExclusion` |
| Effects | `vfx::addDefaultEffects` and the effect passes' settings |
| Build | `ve_setup_game`, the template's CMake setup, `VE_*` environment switches |

## Evolving: expect changes, design for them now

| Area | What will change | How to write game code now |
|---|---|---|
| **Multiplayer gameplay** (roadmap N2) | Interactions, pickups, doors, NPC actions going through the host | Write every world-changing action host-authoritative from the start (conventions: "Multiplayer-ready gameplay"); offline play runs the same path. `ProxySet` pickups and foliage chopping are still local: don't copy that pattern for shared state. |
| **World streaming** ([08-world-streaming.md](08-world-streaming.md)) | Placed objects may exist only near players | Remember objects with `ObjectRef` and handle `get()` returning null; keep state that must survive in `Saveable` components. Streaming will be opt-in per object. |
| **`ve::net` API** | Interest management, prediction, batching | Use the documented request / replicate pattern, not transport details. |
| **Decals** (`DecalProjector`, `Renderer::decals()`, `veApplyDecals`) | New: settings may be added or renamed (emissive, atlases), and more shaders may receive them | Set fields by name rather than relying on defaults; custom shaders call `veApplyDecals` exactly as `standard.frag` does. |
| **Asset cooking** (`vecook`, the cook cache, `.texture` files) | Cooked file (`.vemodel` included) and `.vepak` layouts may change (old files are re-cooked, archives re-packaged); `ve::vfs`, `Cooker`, `ModelData` / `ModelSettings` and the `.model` file are new and may still change | Keep source assets as the truth; don't hand-edit or commit `cooked/` |
| **Material files** (`*.material`, `MaterialAsset`) | New: fields may be added (more shader options, per-instance overrides); the file format is versioned | Keep `"version": 1` in files; old files will keep loading. |
| **Editor** | New tools (painting proxies, streaming previews) | Editor-only; doesn't affect game code. |

## Internal: not for game code

Anything not reachable through the documented headers: `ve/gfx/*` details,
`render/*` internals (`SceneGatherer`, `StaticBatcher`, `InstanceSystem`
internals), `physics/JoltInternal.hpp`, editor internals, and anything in a
`detail` namespace. These change whenever the engine needs them to.
`tools/check_layering.py` keeps the engine's own modules honest; game code
should stay on the public side in the same spirit.
