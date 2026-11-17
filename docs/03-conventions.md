# Conventions & Recipes

## Code conventions

- **C++20**, namespaces `ve` (engine), `vfx` (effects), `savanna` (sandbox).
- Files: `PascalCase.hpp/.cpp`, one main class per file. Headers start with a
  comment block explaining *what* and *why*, and short usage examples.
- Members `m_camelCase`, constants `kPascalCase`, public settings plain
  `camelCase` fields (no getters/setters for simple tunables).
- Includes: engine headers use `"ve/..."` inside the engine and `<ve/...>` from
  effects/games.
- Errors: throw `std::runtime_error` for unrecoverable setup errors. `VK_CHECK`
  wraps every Vulkan call that returns `VkResult`. Log with `ve::log::info/warn/error`.
- No warnings: targets use `-Wall -Wextra -Wpedantic` (`ve_set_warnings`).
  Keep the build warning-free.
- GPU objects are **never destroyed directly while frames may be in flight**.
  Use `device.deferDestroy(...)`, `renderer.destroyPipeline(...)`, or let the
  owning class (Mesh/Texture/Material/Shader) do it in its destructor.
- Comments explain intent and Vulkan "why". This codebase doubles as a
  learning resource.

## Coordinate & rendering conventions

| Thing | Convention |
|---|---|
| World | Right-handed, **+Y up**, meters. Objects face **-Z** (`Transform::forward()`) |
| Angles | Radians in code (`GLM_FORCE_RADIANS`); `*Degrees` suffix where degrees are used |
| Main camera depth | **Reversed-Z**: near = 1, far = 0, compare `GREATER_OR_EQUAL`, clear 0 |
| Shadow depth | Standard: near = 0, far = 1, compare `LESS_OR_EQUAL`, clear 1 |
| Clip space | Vulkan (Y down): projection has `proj[1][1] *= -1` |
| Screen UV | (0,0) top-left |
| Front faces | Counter-clockwise (glTF convention) |
| Colour | Linear HDR in `SceneColor`. Textures that hold colour are `*_SRGB`; data textures (normal, roughness) are `*_UNORM` |
| Light units | Arbitrary but consistent: sun intensity ≈ 3, exposure 1 |

## Recipes

### Start a new game (without touching the engine)

Copy `templates/game` out of the repo and build it with
`-DVE_ENGINE_DIR=/path/to/vulkan-engine` (details in `templates/game/README.md`).
Its CMakeLists is just:

```cmake
add_subdirectory(${VE_ENGINE_DIR} vulkan-engine)
add_executable(mygame ${GAME_SOURCES})
ve_setup_game(mygame ASSETS ${CMAKE_CURRENT_SOURCE_DIR}/assets SHADERS ${GAME_SHADERS})  # [NO_EFFECTS]
```

Prefix your shader file names (e.g. `mygame_toon.frag`). Shaders are looked
up by file name and must not clash with the engine's.

### Write gameplay: a GameObject with behaviour

```cpp
class Crate : public ve::GameObject {
public:
    explicit Crate(std::shared_ptr<ve::Mesh> mesh, std::shared_ptr<ve::Material> mat) : GameObject("Crate") {
        addComponent<ve::MeshRenderer>(mesh, mat);      // components can be added in the constructor
    }
    void start() override { m_spawn = transform().position(); }
    void update(float dt) override { transform().rotate({0, 1, 0}, dt); }
    void fixedUpdate(float dt) override { /* physics-like logic at 50 Hz */ }
    void onGui() override { /* ImGui HUD bits */ }
private:
    glm::vec3 m_spawn{};
};
auto& crate = engine.scene().spawn<Crate>(mesh, mat);
crate.transform().setPosition({0, 1, 0});
```

Read one-shot inputs (`keyPressed`, `actionPressed`) in `update()`. If physics
in `fixedUpdate()` needs them, pass them over as a flag (see `Player`).

### Input: actions, axes, gamepads, rebinding

Gameplay reads named actions and axes, never keys, so players can rebind and a
gamepad works without extra code:

```cpp
ve::Input& in = input();
in.bindAction("Jump", ve::Key::Space);
in.bindAction("Jump", ve::GamepadButton::A);
in.bindAction("Throw", ve::GamepadAxis::RightTrigger);   // trigger past triggerThreshold
in.bindAxis("MoveY", ve::Key::S, ve::Key::W);            // -1 .. +1
in.bindAxis("MoveY", ve::GamepadAxis::LeftY, -1.0f);     // stick up = forward
float fwd = in.axisValue("MoveY");                       // strongest source wins
```

- Sticks have a radial dead zone (`stickDeadZone`); the first connected
  controller is used (`gamepadConnected()`, `gamepadName()`).
- Rebinding UI: list `in.actions()` with `in.bindingNames(action)`, call
  `in.startRebind(action)`; the next key, mouse button or pad button/trigger
  replaces that device's bindings (Escape cancels, `rebinding()` tells you it
  is waiting). Persist with `saveBindings()` / `loadBindings(json)`. The
  savanna's F10 window does this and keeps `controls.json` in the save folder.
- Tests: `attachVirtualGamepad()` + `setVirtualGamepadAxis/Button` drive an SDL
  virtual controller (works headless).

### Profile CPU time

Wrap a scope in `VE_PROFILE("Name")` (main thread; nested scopes become child
rows). The F1 overlay's "CPU zones" table and `VE_BENCH` output show the
average and the worst frame of the last second. The engine already times the
frame phases (fixed update, update, GUI, audio, render and its sub-steps) and
the terrain, foliage and grass systems; GPU time per pass is in "Render passes".

### Write a reusable Component

```cpp
class Spin : public ve::Component {
public:
    float speed = 1.0f;
    void update(float dt) override { transform().rotate({0, 1, 0}, speed * dt); }
};
go.addComponent<Spin>().speed = 3.0f;
```

### Custom material with its own shader

1. Write `mything.vert` and `mything.frag` in your game's `shaders/` folder.
   Copy `engine/shaders/standard.vert` as a starting point. Fragment shaders
   that want lighting `#include "ve_lighting.glsl"`, build a `VeSurface` and
   call `veEvaluateLighting(s)`. Read parameters from `veMaterial().params[i]`
   and textures with `veSample(veMaterial().textures[i], uv)`.
   **Opaque shaders also write the thin G-buffer** (used by SSGI/SSR):
   `layout(location = 1) out vec4 outGBuffer;` … `outGBuffer = veGBuffer(N, roughness);`.
   If you leave it out, the shader still works, but SSGI/SSR see garbage for those pixels.
   Shaders with their own lighting loop should only visit the point lights of
   the pixel's cluster: `VeLightList list = veLightList(P);` then
   `lights.point[veLightAt(list, k)]` for `k < list.count` (see the template's
   `mygame_toon.frag`).
2. Document the parameter layout at the top of the shader.
3. In C++:
   ```cpp
   ve::ShaderDesc d;
   d.name = "MyThing"; d.vertex = "mything.vert"; d.fragment = "mything.frag";
   d.cull = ve::CullMode::None;                // optional state
   auto mat = renderer.createMaterial(renderer.createShader(d));
   mat->setVector(0, {1, 0.5f, 0.2f, 1});
   mat->setTexture(0, assets.loadTexture("textures/foo.png"));
   go.addComponent<ve::MeshRenderer>(mesh, mat);
   ```
4. If the vertex shader moves vertices (wind etc.), also write a shadow vertex
   shader following the contract in `engine/shaders/shadow.vert` and set
   `d.shadowVertex`. Otherwise set `d.castShadows = false`.
5. Transparent? Set `d.blend = AlphaBlend; d.queue = RenderQueue::Transparent; d.depthWrite = false;`.
   Transparent shaders may sample scene depth via `frame.builtinTextures.x` (see `water.frag`).

The CMake glob picks up new shader files automatically. Re-run CMake when you
add a file (`cmake -B build`); the VS Code build task does this every time.

**Hot-reload.** While the app runs, saving a shader (or anything it
`#include`s) recompiles it with the build's own glslc command and rebuilds the
pipelines that use it, within about half a second. Compile errors go to the log
and the old version keeps running. This covers `ShaderDesc` shaders and pipelines
made with `createFullscreenPipeline` / `createComputePipeline`. Keep those as
`ve::PipelineHandle` members, which always give the current `VkPipeline`. New
shader files still need a rebuild. `VE_SHADER_HOT_RELOAD=0` turns it off.

### Add a full-screen post-process effect

```cpp
struct TintPush { uint32_t src; float strength; };
class TintPass : public ve::FullscreenPass {
public:
    TintPass() : FullscreenPass("Tint", ve::RenderStage::PostProcess, 10,
                  {.fragment = "tint.frag", .output = "TintOut", .inputs = {"SceneColor"}}) {}
    void setup(ve::RenderGraph& g) override {
        g.createTexture("TintOut", {.format = VK_FORMAT_R16G16B16A16_SFLOAT}); // screen-sized, auto-resized
        FullscreenPass::setup(g);
    }
    void prepare(ve::FrameData&) override { setPush(TintPush{inputIndex(0), strength}); }
    void onGui() override { ImGui::SliderFloat("Strength", &strength, 0, 1); }
    float strength = 0.5f;
};
renderer.graph().addPass<TintPass>();
```

`tint.frag` starts with `#define VE_CUSTOM_PUSH_CONSTANTS`, includes `ve.glsl`,
declares a matching `layout(push_constant)` block and reads the input with
`veSample(pc.src, inUV)`. You cannot read and write the same texture in one
pass. Either write a new texture and have later passes use it, or blend onto
the target (`Desc::blend`) as `VolumetricFogPass` does.

### Optional screen-space effects (SSGI, SSR, depth of field)

They're opt-in passes in `ve_effects`. Add the ones you want after `addDefaultEffects`:
```cpp
#include <vfx/ScreenSpaceGI.hpp>            // bounce light + AO
#include <vfx/ScreenSpaceReflections.hpp>   // wet ground, polished surfaces
#include <vfx/DepthOfField.hpp>
renderer.graph().addPass<vfx::ScreenSpaceGIPass>().intensity = 1.0f;
renderer.graph().addPass<vfx::ScreenSpaceReflectionsPass>();
auto& dof = renderer.graph().addPass<vfx::DepthOfFieldPass>();
dof.enabled = false;              // e.g. turn on while aiming / in menus
dof.autoFocus = false; dof.focusDistance = 2.0f; dof.aperture = 4.0f;
```
Each one is listed in the F1 overlay with a checkbox and its sliders. SSGI/SSR
need opaque shaders to write the thin G-buffer (see the custom-material recipe).
SSR reflects only smooth surfaces (`maxRoughness`), so lower a material's
roughness (e.g. wet terrain) to make it reflective.

### Add a custom (non-full-screen) render pass

Derive from `ve::RenderPass`. In `declare()` list every texture you touch with
its `Access`. In `execute()` use `ctx.beginRendering(...)`, bind pipelines
built with `ve::PipelineBuilder` (always `.layout(renderer.pipelineLayout())`),
push constants, and draw. You can reuse `renderer.drawItems(ctx, indices, PassKind::Forward)`
to draw scene meshes. If you need to switch a texture from writing to
sampling inside one pass, call `graph().transition(ctx.cmd, tex, Access::Sampled)`.

Multi-step full-screen passes (downsample chains, trace + apply) don't need
`FullscreenPass`: create pipelines with `renderer.createFullscreenPipeline(...)`
and record each step as one call, with `graph().sampled(tex)` giving the
bindless index a shader samples:
```cpp
ctx.fullscreen({{m_half}}, m_trace, TracePush{graph().sampled(r.sceneDepth())});
graph().transition(ctx.cmd, m_half, ve::Access::Sampled);
ctx.fullscreen({{r.sceneColor()}}, m_apply, ApplyPush{graph().sampled(m_half)});
```

### Global shader parameters

`renderer.setGlobalParam(i, vec4)` appears as `frame.globalParams[i]` in every
shader. Slots 0–3 are used by `ve_effects` (wind, weather, sun, sun colour;
see `vfx.glsl`). Games should use 4–7.

### Import assets

Put files under `<game>/assets/` (copied next to the exe on build), then
`assets().loadTexture("textures/x.png", srgb)`, `loadModel("models/x.gltf")`
followed by `scene.instantiate(*model)`, or `loadAudio("audio/x.wav")`.

### Interaction ("press E to ...")

```cpp
// The thing (a component; subclass it or set the callbacks):
auto& use = chest.addComponent<ve::Interactable>();
use.prompt = "Open chest";                       // or use.promptFn = [&] { return ...; } ("" = can't now)
use.onUse = [&](ve::GameObject& user) { chest.open(); };
use.focusOffset = {0, 0.5f, 0};                  // aim point for objects without a collider

// The player: finds the focus each frame (main camera view) and uses it on the action.
auto& who = player.addComponent<ve::Interactor>();
who.action = "Use";                              // an Input action (in.bindAction("Use", Key::E))
// HUD: if (auto p = who.prompt(); !p.empty()) draw("[E] " + p);
```

Focus is what the eye ray hits (an Interactable on that object or a
parent), else the best Interactable in a `coneDegrees` view cone within
reach that nothing blocks. The engine never draws prompts: the game decides
how (Savanna's HUD shows "[E] <prompt>").

### Save games

```cpp
// Anything with state worth keeping: implement ve::Saveable (GameObject or Component).
class Chest : public ve::GameObject, public ve::Saveable {
    void saveState(ve::Json& out) const override { out["open"] = m_open; }
    void loadState(const ve::Json& in) override { m_open = in.value("open", false); }
};

ve::save::saveGame(scene, "slot1", {{"gold", gold}});          // + the game's own data
// Loading: rebuild the level first (pickups collected after the save come
// back), then apply. Savanna does it behind the loading screen (World.hpp):
engine.beginLoading(makeLoadingScene());
engine.addLoadStep("Leaving", [&] { engine.scene().clear(); });
engine.addLoadStep("Loading", [&] { buildLevel(engine); });
engine.addLoadStep("Restoring", [&, save] { ve::save::apply(engine.scene(), save); /* + game data */ });
```

A save records moved physics objects of the level, level objects that are
gone, and every Saveable's state, keyed by persistent id (level objects) or
name (code-spawned ones, so give those unique names). The engine's Foliage
(chopped / picked instances), Terrain (edits) and DayNightCycle (time) are
Saveable. Slots: `ve::save::folder()` (per-user data folder, or
`EngineConfig::saveFolder`, or `VE_SAVE_DIR`), written atomically.

### Loading screens

```cpp
ve::Engine engine(config);
engine.beginLoading(makeLoadingScene(), 0.5f);            // your own small scene (UI in onGui)
engine.addLoadStep("Loading the world", [&] { loadWorld(engine); }); // main thread, one step per frame
engine.run();
```

The loading scene's objects read `engine().loadProgress()` (0..1) and
`engine().loadStatus()` (the current step / task name). Systems that fill
the world over several frames register a task:
`engine().addLoadTask("Placing vegetation", [] { return progress; })`
(Foliage does, instead of its synchronous preload). The world updates in
editor-paused mode meanwhile, so only `runInEditor` objects run. The
backbuffer is sRGB: convert display colours to linear for ImGui (see the
sandbox's `LoadingScreen.cpp`).

Wait for what the player needs to start, not the whole world: a task's
progress should reach 1 when the area around the viewer is ready and keep
streaming the rest afterwards (foliage: `FoliageSettings::readyDistance`).
When the editor is open the same calls show no loading scene: the editor
draws a progress panel over the world as it fills in. So a game that can
start in the editor should ask for Edit mode before `beginLoading`.

### Fire (campfires, torches)

```cpp
auto& fire = flames.addComponent<vfx::Fire>();   // at the fuel, flames go up
fire.height = 1.4f; fire.radius = 0.5f; fire.intensity = 4.0f;
fire.coals = 0.0f;                               // torches / candles: no ember bed
// update(): the light follows the flames
light->intensity = base * fire.flicker();
light->transform().setLocalPosition(lightPos + fire.lightJitter());
```

Smoke and sparks are `ParticleEmitter`s on top (see the sandbox's
`Campfire`). `temperature` < 1 gives a lazier, redder fire; more `layers`
gives a fuller one. `haze` (0 = off) sets the heat shimmer above the flames;
the sandbox uses 1 for the campfire and 0.6 for torches. Everything is
editable in the editor's inspector.

### Animated characters (skinned glTF)

```cpp
auto model = assets().loadModel("models/villager.gltf"); // skin + clips (Idle, Walk)
ve::GameObject& v = scene.instantiate(*model, this);    // under this object
auto* anim = v.getComponent<ve::Animator>();
anim->play("Walk", 0.3f);                               // crossfade 0.3 s
anim->play("Idle", 0.3f);                               // and back
ve::GameObject* hand = scene.find("LowerArm.R");        // joints are objects: parent props to them
```

In a scene file: a `ModelInstance` with `path` and `animation` (the clip to
loop). Models face +Z (glTF). See the sandbox's `Villager` (wanders around
the camp, walks and idles) and `tools/generate_assets.py` (`villager()`
writes a rigged model with keyframed clips).

### Prefabs

```cpp
ve::prefab::save(object, "prefabs/hut.prefab");                       // or Edit > Save as prefab
ve::GameObject* hut = ve::prefab::instantiate(scene(), "prefabs/hut.prefab", pos, rot);
```

A prefab stores the object's registered type, fields, persistent components
and children. Code that builds meshes from fields in `start()` (like
`BuildPiece`) makes tiny prefabs: just the type and its fields. Instances
spawned while playing are kept by save games automatically.

### Building: snap points, previews, placing

Engine mechanics, game rules. A piece is a prefab with `SnapPoint`s
(and optionally `PlacementBox`es if its bounds are a poor fit, e.g. sloped
roofs); the game's build tool runs:

```cpp
preview.show(scene(), piecePath);  preview.update();               // ghost (late update)
glm::quat rot = glm::angleAxis(ve::placement::snapAngle(yaw, glm::radians(45.0f)), up);
glm::vec3 pos = aim;
if (auto s = ve::placement::findSnap(scene(), preview.snapPoints(), rot, aim, 0.9f, preview.object())) pos = s->position;
preview.setPose(pos, rot);
preview.setValid(ve::placement::fits(physics(), preview.volume(), pos, rot, 0.1f, allowTerrain));
if (placePressed && preview.valid()) ve::prefab::instantiate(scene(), piecePath, pos, rot);
```

- Snap points are positions; the piece keeps the player's rotation. Lay
  them out on one lattice (Savanna: tile corners / edge midpoints every
  1 m, wall tops at 2.5 m, foundation tops at 0.5 m) and any pair lines up.
- Pieces that create colliders in `start()` skip them on ghosts
  (`PlacementGhost::is(*this)`).
- Rebuild the navmesh around placed / removed pieces (`NavMeshSurface::rebuild`).
- Roofs: eaves (low edge) on wall tops; a roof whose slope cuts through a
  side wall is (correctly) blocked.

### Walking AI: navmesh + agents

```cpp
// The level: a NavMeshSurface over the area (scene file or code).
auto& nav = scene.spawn("Navigation").addComponent<ve::NavMeshSurface>();
nav.size = {192.0f, 400.0f, 192.0f};        // box around the object
nav.settings.agentRadius = 0.35f;

// A character: the engine walks, the game decides where.
auto& agent = addComponent<ve::NavAgent>();
agent.speed = 1.3f;
if (agent.arrived())
    if (auto p = ve::NavMeshSurface::find(scene())->randomPointNear(home, 8.0f)) agent.setDestination(*p);
```

- Agents join once the navmesh under them is built (a few frames; behind a
  loading screen it's done before play starts). Move them with `warp()`,
  not the Transform. Following something that moves: call `setDestination`
  a few times a second (see `Villager`).
- Static Rigidbodies, the terrain and foliage colliders block; kinematic and
  dynamic bodies don't (agents avoid each other, not crates). After moving
  static geometry call `rebuild(area)`; chopped trees rebuild by themselves.
- Big worlds: `streamRadius` keeps only tiles around the camera (or `focus`).

### Thousands of props: instancing

```cpp
ve::InstanceGroupDesc desc;
desc.name = "acacia";
desc.lods = {{acaciaMesh, {barkLeavesMaterial}}};   // up to 4 LODs: {mesh, materials, maxDistance}
desc.shadowDistance = 150.0f;                       // small props: keep short
auto trees = renderer.createInstanceGroup(desc, 20000);
auto handle = trees->add(transform, glm::vec4(tint, 1.0f)); // w > 0: tint (standard shader)
trees->remove(handle);
```

LOD chains (meshoptimizer):

```cpp
ve::MeshData lod1 = tree.simplified({.ratio = 0.35f, .maxError = 0.08f, .faceted = true});
ve::MeshData lod2 = tree.simplified({.ratio = 0.10f, .maxError = 0.3f, .sloppy = true, .faceted = true});
desc.lods = {{mesh0, {mat}, /*screenSize*/ 0.10f},   // full detail while radius covers >= 10% of half-screen
             {mesh1, {mat}, 0.035f},
             {mesh2, {mat}}};                         // last LOD: threshold ignored
```

Far LOD as an impostor (StandardMaterial meshes):

```cpp
auto imp = ve::Impostor::bake(renderer, *mesh0, {material});   // load time, a few ms
desc.lods.push_back(imp->lod());                                // last LOD
```

Custom material shaders: forward `veInstanceLodFade()` in a `flat` varying and
call `veLodDither(fade)` at the start of the fragment shader to get crossfades
(see `standard.vert/frag`).

Keep the `shared_ptr<InstanceGroup>` alive somewhere that dies with the scene
(e.g. a GameObject member, like `savanna::PropField`). Solid props use
`physics().createStaticBody(collider, pos, rot, scale, userValue)`; the
`userValue` comes back in `RaycastHit::userValue`. Custom vertex shaders must
use `veModelMatrix()` (not `draw.model`) to work with instancing.

### Forests, rocks, bushes: foliage layers

For anything scattered by rules rather than placed by hand, use
`ve::FoliageSystem` (it streams, so world size doesn't matter):
```cpp
ve::FoliageWorld world;
world.boundsMin = {-2000, -2000}; world.boundsMax = {2000, 2000};
world.height = [&](glm::vec2 p) { return terrain.height(p.x, p.y); }; // called on a worker thread
auto& foliage = scene.spawn<ve::FoliageSystem>(world);
foliage.addExclusion(campCentre, 25.0f);

ve::FoliageLayer trees;
trees.name = "acacia";
trees.variants = {lodChainA, lodChainB};       // vector<InstanceLod> per mesh variant
trees.colliders = {trunkCapsule};
trees.colliderUserValue = kAcacia;             // comes back in RaycastHit::userValue
trees.spacing = 8.0f;  trees.probability = 0.8f;
trees.clusterFrequency = 0.01f;                // groves
trees.density = [&](const ve::FoliageSample& s) { return biome.savannah(s.position); };
trees.viewDistance = 1500.0f;
const uint32_t treeLayer = foliage.addLayer(trees);

ve::FoliageLayer bushes = /* ... */;
bushes.avoid = {{treeLayer, 2.5f}};            // keep clear of trunks
foliage.addLayer(bushes);
foliage.colliderFocus = &player;
foliage.preload(player.transform().position());
```
Rules are deterministic, so the same world appears every run without saving
anything.

### Interactive foliage: chop trees, pick fruit

The engine keeps per-instance state; what it means is up to the game.
```cpp
// Scene file (FoliageType): an extra look, and promotion near the player.
//   "states": "Stump = acacia stump",        // MeshLibrary name or glTF path(s)
//   "promoteDistance": 8, "promoteType": "ChoppableTree"
// Code (FoliageLayer): layer.states = {{"Stump", stumpChains, stumpColliders}};
//                      layer.promoteDistance = 8; layer.promote = [](Scene& s, auto&) { return &s.spawn<ChoppableTree>(); };

class ChoppableTree : public ve::GameObject {          // VE_REGISTER_TYPE(ChoppableTree, "ChoppableTree")
    void chopped() { getComponent<ve::FoliageLink>()->setState("Stump"); } // a stump for good
};

auto info = foliage.nearest(layer, playerXZ, 5.0f);   // or raycast the promoted object
foliage.setState(info->id, foliage.stateIndex(layer, "Stump"));
foliage.remove(info->id);                               // gone (no mesh, no collider)
auto save = foliage.overrides();                        // [(id, state)]: store in your save game
foliage.setOverrides(save);                             // ...and give it back after loading
```
- An id (`FoliageInstanceId`: layer + placement square) is the same every run,
  as long as the layer order and rules don't change.
- A changed instance shows its state's mesh and collider now and whenever its
  cell streams in again: walk away and back and the stump is still there.
- Within `promoteDistance` of `colliderFocus` (the player) each instance is
  replaced by a GameObject (`promote`, or a plain one) carrying a
  `FoliageLink` (its id, transform, state and `setState`). Unless the object
  has its own, the system gives it the instance's mesh and a static collider,
  so the swap is invisible; it goes back to being an instance further away
  (+25 %). The object is the system's: keep game state that must last (a
  regrowth timer, chop progress you want to keep) in your game, keyed by the id.
- Savanna's `Harvest.hpp`: `ChoppableTree` (3 chops, then a stump), `BerryBush`
  (picked look, regrows via the game's `Regrowth` timer).

### Grass

```cpp
ve::GrassDesc g;
g.boundsMin = {-320, -320}; g.boundsMax = {320, 320};
g.resolution = 321;                                    // match your heightfield samples for an exact fit
g.height = [&](glm::vec2 p) { return terrain.height(p.x, p.y); };
g.mask = [&](glm::vec2 p) { return glm::vec4(density, colourMix, heightScale, 0.0f); };
g.colorA = {...}; g.colorB = {...};
renderer.graph().addPass<ve::GrassPass>(g);            // one pass per grass type (e.g. grass + reeds)
```
Tweak live via `pass.desc()` or F1. `height` and `mask` are sampled once into
textures when the pass is set up and then released (so whatever they capture,
e.g. a terrain snapshot, is freed); to change the ground, add a new pass. For a custom look, set `g.fragmentShader`
to a shader with `grass_field.frag`'s inputs (the savanna's `grass.frag` adds
wetness). Every clump gets a random yaw plus a tilt up to `g.maxTilt` radians. Grass
casts sun shadows within `g.shadowDistance` (default 30 m; `g.castShadows = false`
turns them off).

Alpha-clipped **grass cards** (Valheim-style) instead of procedural blades:
```cpp
g.cardMesh = ve::GrassDesc::cardMeshCrossed(3, 1.0f);  // or your own: root at origin, height 1
g.cardTexture = assets.loadTexture("textures/grass_card.png", true); // alpha = blade coverage
g.alphaCutoff = 0.5f;
g.colorA = g.colorB = glm::vec3(1.0f);                  // tints multiply the texture
```
The fragment shader defaults to `grass_card.frag`. Its alpha test is sharpened
with `fwidth`, so thin blades survive mipmapping.

LOD chain: fill `GrassDesc::lods` nearest first (each a mesh, or empty for
procedural blades, a texture and `maxDistance`). Each clump picks its LOD by
its own jittered distance, dithered over `lodBlend` m. In a scene, a
`ve::GrassLayer` builds this from a glTF file with `*_LOD0`, `*_LOD1`... nodes
(see docs/06-world.md).

### Audio: 3D sounds, buses, indoor muffling, occlusion

```cpp
auto& fire = go.addComponent<ve::AudioSource>(assets.loadAudio("audio/fire_loop.wav"));
fire.loop = true; fire.playOnStart = true; fire.maxDistance = 28.0f; // 3D: pans + fades
fire.occlusion = true;                     // muffled when walls/terrain block it
rain.spatial = false; rain.bus = "Weather"; // 2D bed on the Weather bus

auto& zone = hut.addComponent<ve::AudioZone>();            // "indoors"
zone.shape = ve::AudioZone::Shape::Cylinder; zone.radius = 2.4f; zone.height = 4.0f;
zone.effects = {{"Weather", 0.3f, 450.0f}, {"Ambient", 0.45f, 900.0f}}; // bus, volume, low-pass Hz

audio().setBusVolume("Music", 0.5f);       // options menu
```
Set `bus` and `occlusion` before the sound first plays.

### Colour grading / tonemapping

```cpp
#include <vfx/ColorGrading.hpp>
auto& grade = renderer.graph().addPass<vfx::ColorGradingPass>();
grade.settings.tonemapper = vfx::ColorGradingSettings::Tonemapper::AgX;
grade.settings.temperature = 10.0f; grade.settings.contrast = 1.15f;   // or a preset: ...::farCry2()
grade.settings.lut = assets.loadTexture("textures/my_lut.png", /*srgb*/ false); // optional strip LUT
```
Tweak everything live in F1 > Render passes > Color grading. Night brightness
in Savanna: `DayNightCycle::nightExposure`, `moonIntensity`, and the
atmosphere's `nightAmbient` (all in F1 > Day / night).

### Physics

```cpp
// Static scenery (shared shape, scaled by the object's Transform):
static const ve::Collider trunk = ve::Collider::capsule(1.5f, 0.3f).offset({0, 1.5f, 0});
tree.addComponent<ve::Rigidbody>(trunk, ve::BodyType::Static);

// Dynamic prop:
auto& rb = crate.addComponent<ve::Rigidbody>(ve::Collider::box({0.4f, 0.4f, 0.4f}));  // Dynamic
rb.mass = 15.0f;
rb.setLinearVelocity({0, 5, 0});          // fine before start(): applied on creation

// Moving platform: kinematic, just move its Transform in update().
platform.addComponent<ve::Rigidbody>(ve::Collider::box({2, 0.2f, 2}), ve::BodyType::Kinematic);

// Character: add in the constructor, drive it from fixedUpdate().
m_cc = &addComponent<ve::CharacterController>();
void fixedUpdate(float dt) override { if (jumpPressed) m_cc->jump(5.5f); m_cc->move(wishVelocity, dt); }

// Queries:
if (auto hit = physics().raycast(eye, forward, 30.0f)) { /* hit->gameObject, hit->point, hit->normal */ }
auto hit = physics().sphereCast(eye, 0.3f, forward, 3.0f);         // a fat ray (melee reach, camera collision)
auto box = physics().boxCast(center, half, rotation, dir, 5.0f);
for (const ve::RaycastHit& h : physics().overlapSphere(swordTip, 0.4f, {.ignore = myBody})) { /* h.gameObject */ }
ve::QueryFilter f{.statics = false, .characters = true};           // only moving bodies and characters
auto near = physics().overlapBox(center, half, rotation, f);       // also: shapeCast / overlap with any Collider

// Trigger volume (pickup, damage zone, door sensor): no collision response.
auto& zone = pickup.addComponent<ve::Rigidbody>(ve::Collider::sphere(1.0f), ve::BodyType::Static);
zone.isTrigger = true;

// Events: override in any Component (or GameObject subclass) on either object.
class Pickup : public ve::Component {
    void onTriggerEnter(ve::GameObject& other) override { if (other.tag == "player") gameObject().destroy(); }
};
class Breakable : public ve::Component {
    void onCollisionEnter(const ve::Collision& c) override { if (c.impactSpeed > 8.0f) shatter(c.point); }
};
```

Rules:
- Bodies are created in `start()` from the Transform at that moment. After
  that, move dynamic bodies with `teleport()`/forces (not the Transform), and
  static ones with `syncFromTransform()`.
- `Collider::mesh` and `heightfield` are static-only. Use `convexHull` for moving things.
- The world finds physics components through the typed registry, which also
  matches subclasses (`class Door : public ve::Rigidbody` works).
- Events are sent after each physics step on the main thread, to every
  component of both objects (`Collision::normal` points from the other object
  towards you; `impactSpeed` is the closing speed when the contact began).
  `onCollisionStay` runs every step while touching. Exit also fires when two
  bodies fall asleep against each other, and Enter fires again when they wake.
  Triggers only send Enter/Exit.
- Characters enter triggers but don't get collision events (their movement is
  a sweep, not a contact). Triggers need a convex shape to see characters.
- Queries skip characters and triggers unless the `QueryFilter` asks for them;
  `raycast(origin, dir, dist, ignore, hitCharacters)` still works.

### Photo mode (promotional screenshots)

```cpp
scene.spawn<vfx::PhotoMode>();   // F12 in game
```

F12 pauses the game and freezes time, including wind, water and particles. A
free camera takes over:
- right mouse: look; WASD: move; Q/E: down/up; Shift/Ctrl: speed
- Enter: take photo; F12 or Esc: leave

The panel has FOV, exposure (EV), time of day, depth of field (autofocus,
"focus on what's in the centre", aperture) and capture settings.

**Take photo** raises every effect to maximum quality: 4096 shadow maps,
`lodBias` 4, 128-step clouds, 96-step fog, more SSR/SSGI steps and DOF taps.
`Renderer::beginPhotoCapture` then renders offscreen at the chosen resolution
(window, ×2, 1080p, 1440p or 4K). It averages 16–256 frames, each with the
camera shifted by a fraction of a pixel. Edges come out anti-aliased and fog,
clouds and GI come out noise-free.

A 64-sample 3200×1800 shot takes about 2 s in Release. The window doesn't
update meanwhile. The PNG goes to `screenshots/photo_<date>_<time>.png`
next to the executable.

Games hide their HUD while `vfx::PhotoMode::anyActive()`; captures never
include UI. Scripted shots: `enter()`, `setView(pos, yaw, pitch)`,
`takePhoto()`, wait for `lastPhoto()`. In the sandbox,
`SAVANNA_PHOTO=64,1,4` does this headless (samples, resolution, DOF aperture).

### Shadows configuration

```cpp
config.renderer.maxShadowedDirectionalLights = 2; // e.g. sun + moon
config.renderer.cascadeCount = 4;
config.renderer.maxShadowedPointLights = 4;
config.renderer.maxShadowDistance = 180.0f;
// at runtime: renderer.settings().cascadeCount = 3; renderer.applySettings();
```
Set `castShadows = true` on the `DirectionalLight` / `PointLight` components.
