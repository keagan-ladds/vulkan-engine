# In-engine editor (Phase E)

Goal: build and tweak scenes visually instead of in C++, while behaviour stays
in C++ (the Unity split). Press **F2** in any game built on the engine.

## Stages

| Stage | Contents | Status |
|---|---|---|
| E1 | Reflection (types describe their fields), scene files (JSON), persistent object IDs | ✅ |
| E2 | Editor MVP: hierarchy, inspector, GPU picking, move/rotate/scale gizmo, debug shapes, Play/Stop with restore, undo/redo, save/load, editor camera | ✅ |
| E2b | Savanna's hand-placed content moved to `sandbox/assets/scenes/savanna.json` (camp, huts + audio zones, torches, crates, reed screens, sunstones, ambience, PlayerStart) | ✅ |
| E3 | Prefabs (save a configured object as a reusable asset), asset browser with drag & drop, multi-select, snapping UI polish | planned |
| E4 | World tools: foliage paint/exclusions, terrain sculpt/paint (needs Phase 13 terrain module) | planned |

## Design

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
  - engine types: `ve::registerBuiltinTypes()`, called by the Engine;
  - effects: `vfx::registerTypes()`, called by `addDefaultEffects`;
  - game types: `VE_REGISTER_TYPE(...)` in a game .cpp. The executable's
    object files are always linked, so static registrars work there.
- Asset fields (models, audio clips) are stored as paths.

### Persistence rules
- `GameObject::persistent` and `Component::persistent` (both false by default)
  decide what gets saved.
  - The editor sets them on what it creates.
  - The scene loader sets them on what it loads.
  - Code-spawned objects (terrain, foliage, weather...) stay non-persistent.
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
  (`VE_REGISTER_TYPE`) plus engine components. Procedural systems stay in
  code: terrain, foliage rules, grass, weather, the HUD.
- **Classes stored in files must be default constructible.** They fetch shared
  resources themselves in `start()`, e.g. Savanna's `propMaterial()` or a lazily
  created shared mesh, and find other objects by name, e.g.
  `scene().find("Player")`.
- **Load at startup:** `auto level = SceneSerializer::load(scene, "scenes/savanna.json");`.
  Then use the returned roots for anything code needs from the level: spawn
  the player at the `PlayerStart` marker, collect grass clearings from objects
  tagged `hut`.
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
