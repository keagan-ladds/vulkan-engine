# In-engine editor (Phase E)

Goal: build and tweak scenes visually instead of in C++, while behaviour stays
in C++ (the Unity split). Press **F2** in any game built on the engine.

## Stages

| Stage | Contents | Status |
|---|---|---|
| E1 | Reflection (types describe their fields), scene files (JSON), persistent object IDs | ✅ |
| E2 | Editor MVP: hierarchy, inspector, GPU picking, move/rotate/scale gizmo, debug shapes, Play/Stop with restore, undo/redo, save/load, editor camera | ✅ |
| E2b | Savanna's hand-placed content moved to `sandbox/assets/scenes/savanna.json` (camp, huts + audio zones, torches, crates, reed screens, sunstones, ambience, PlayerStart) | ✅ |
| E3 | Prefabs ✅ (Edit > Save as prefab, Create > Prefab, `PrefabInstance` link), snap points ✅ (dragging an object with `SnapPoint`s snaps it onto others', drawn on the selection; toolbar "Snap points"); asset browser with drag & drop, multi-select, prefab overrides / propagation planned | partly |
| E4 | World tools: terrain sculpt/paint ✅; foliage paint/exclusions planned | partly |

## Design

The editor is one class (`ve::Editor`) whose member functions are spread over
one file per part: `Editor.cpp` (modes, menus, scene files),
`EditorHierarchy.cpp`, `EditorInspector.cpp`, `EditorUndo.cpp` (snapshots,
undo / redo) and `EditorViewport.cpp` (camera, picking, gizmo, terrain
tools); shared private helpers live in `EditorInternal.hpp`. The panels share
most of their state (selection, undo, the edit snapshot), so separate panel
classes would add plumbing without hiding anything.

### Reflection (`ve/scene/Reflection.hpp`)
- `TypeRegistry` maps type names to `TypeInfo`. A `TypeInfo` holds:
  - a factory for components or GameObject classes;
  - a `dynamic_cast` matcher;
  - the list of fields;
  - an optional `onChanged` hook, run after an edit or a load. For example,
    AudioSource re-applies its settings and Rigidbody rebuilds its body.
- A field is a name, a kind (bool, int, float, vec2/3/4, colour, string,
  enum, asset path) and an accessor, plus UI hints (range, step, tooltip).
- Registration:
  - engine types: each module's `registerXTypes(TypeRegistry&)` (scene types in
    `registerBuiltinTypes()`), all called once by the Engine;
  - effects: `vfx::registerTypes()`, called by `addDefaultEffects`;
  - game types: `VE_REGISTER_TYPE(...)` in a game .cpp. The executable's
    object files are always linked, so static registrars work there.
- Asset fields (models, audio clips) are stored as paths.

### Persistence rules
- `GameObject::persistent` and `Component::persistent` (both false by default)
  decide what gets saved.
  - The editor sets them on what it creates.
  - The scene loader sets them on what it loads.
  - Code-spawned objects (a game's HUD, the player...) stay non-persistent.
    They can be selected and edited live, but changes aren't saved.
- Components that a GameObject class creates itself (e.g. a Torch's own
  PointLight) stay non-persistent. The class exposes its own fields instead.
- Each GameObject has a 64-bit random `id`, used for parenting in files and
  for undo.
- Imported glTF models are placed via a `ModelInstance` component (path +
  static colliders). It instantiates the model's nodes as non-persistent
  children when it starts. `PrimitiveRenderer` covers blockout shapes.

### Scene file (JSON)
```json
{ "version": 1,
  "objects": [ { "id": "8f2c…", "type": "GameObject", "name": "Hut", "parent": "0",
                 "active": true, "tag": "",
                 "position": [x,y,z], "rotation": [x,y,z,w], "scale": [x,y,z],
                 "fields": { … },
                 "components": [ { "type": "ModelInstance", "enabled": true,
                                   "fields": { "path": "models/rondavel.gltf" } } ] } ] }
```
Saved to the game's *source* assets folder (`VE_SOURCE_ASSETS_DIR`, set by
`ve_setup_game`), so edits are not lost when the build copies assets again.
Load with `ve::SceneSerializer::load(scene, "scenes/x.json")`.

### How a game uses scene files (Savanna as the example)
- **Level content goes in the file.** Anything placed by hand goes in
  `scenes/<level>.json`, made of registered GameObject classes
  (`VE_REGISTER_TYPE`) plus engine components. Terrain, vegetation, water,
  weather and day/night are scene objects too (docs/06-world.md); code keeps
  behaviour and generated meshes (`ve::MeshLibrary`).
- **Classes stored in files must be default constructible.** They fetch shared
  resources themselves in `start()`, e.g. Savanna's `propMaterial()` or a lazily
  created shared mesh, and find other objects by name, e.g.
  `scene().find("Player")`.
- **Load at startup:** `auto level = SceneSerializer::load(scene, "scenes/savanna.json");`.
  Then use the returned roots for anything code needs from the level: spawn
  the player at the `PlayerStart` marker.
- **Variations are data.** `ModelInstance::hiddenNodes = "Door"` gives the hut
  without a door.
- **Derive counters from the scene, don't hard-code them.** For example, the
  sunstone total is collected + remaining, so stones added in the editor count.

### Editor (`ve/editor/Editor.hpp`)
- **Modes:**
  - Off: the normal game.
  - Edit: the game is paused and the editor UI is shown. Objects with
    `runInEditor` still update (foliage streaming, weather, day/night), and an
    editor fly camera takes over.
  - Play: the game runs inside the editor. Stop restores every persistent
    object from a snapshot taken at Play.
  - F2 toggles Off ↔ Edit (Play → Stop).
- **Picking:** on click, `PickingPass` renders object IDs of every visible
  MeshRenderer into an R32_UINT target, and the pixel is read back next frame.
  Lights, cameras, audio sources and zones get clickable screen icons. Selection
  goes to the nearest persistent ancestor (clicking a hut wall selects the
  hut).
- **Gizmo:** custom, drawn with ImGui:
  - Translate: axis arrows plus plane squares.
  - Rotate: three rings.
  - Scale: axis boxes plus a uniform centre handle.
  - World/local space; snapping (Ctrl or toggle).
- **Debug shapes:** drawn over the scene with ImGui. They cover the
  selection's bounds, collider shapes, AudioZone shapes, point light ranges and
  audio source distances.
- **Undo/redo:** per-object JSON before/after snapshots. Property edits apply
  in place; structural changes (create, delete, reparent, add/remove
  component) re-create the object.

### Terrain tools (E4)
Select the Terrain: the inspector's **Terrain tools** section has a brush
(Raise, Lower, Smooth, Flatten, Paint biome), radius and strength. In the
viewport the brush outline follows the ground; hold the left mouse button to
apply (Shift swaps Raise/Lower; Ctrl+click with Flatten picks the target
height). Each stroke is one undo step; "Clear all edits" removes them.

- Edits are a layer on top of the generator (`TerrainEdits`): a height offset
  and a painted biome per sample, stored only in the 64-cell tiles that were
  touched, relative to the terrain's centre (they stay put when you resize or
  move it). They are saved in the scene file (Terrain's hidden `edits` field:
  heights in cm, biome and strength bytes, base64 per tile).
- Code can brush too: `terrain.applyBrush(brush, {x, z}, dt)`.
- Brushing rebuilds only the brushed area: sample tiles, LOD meshes and
  physics tiles elsewhere are kept, foliage re-places just the nearby cells
  (`FoliageSystem::refresh`; chopped / picked instances keep their state) and
  grass re-bakes its textures in the background (`GrassPass::refresh`).
  Listeners ask `Terrain::changedOnlyIn(version, area)`.
