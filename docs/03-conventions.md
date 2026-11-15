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

### Global shader parameters

`renderer.setGlobalParam(i, vec4)` appears as `frame.globalParams[i]` in every
shader. Slots 0–3 are used by `ve_effects` (wind, weather, sun, sun colour;
see `vfx.glsl`). Games should use 4–7.

### Import assets

Put files under `<game>/assets/` (copied next to the exe on build), then
`assets().loadTexture("textures/x.png", srgb)`, `loadModel("models/x.gltf")`
followed by `scene.instantiate(*model)`, or `loadAudio("audio/x.wav")`.

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
anything. To make a change persistent (e.g. a felled tree), keep your own
"removed" set and exclude it, either in `density` or by `addExclusion` before
loading. A built-in removal API is a roadmap item.

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
Tweak live via `pass.desc()` or F1. For a custom look, set `g.fragmentShader`
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
```

Rules:
- Bodies are created in `start()` from the Transform at that moment. After
  that, move dynamic bodies with `teleport()`/forces (not the Transform), and
  static ones with `syncFromTransform()`.
- `Collider::mesh` and `heightfield` are static-only. Use `convexHull` for moving things.
- Add physics components as their exact type (`addComponent<ve::Rigidbody>`).
  The world finds them through the typed registry.

### Shadows configuration

```cpp
config.renderer.maxShadowedDirectionalLights = 2; // e.g. sun + moon
config.renderer.cascadeCount = 4;
config.renderer.maxShadowedPointLights = 4;
config.renderer.maxShadowDistance = 180.0f;
// at runtime: renderer.settings().cascadeCount = 3; renderer.applySettings();
```
Set `castShadows = true` on the `DirectionalLight` / `PointLight` components.
