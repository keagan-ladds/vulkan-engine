#include "World.hpp"

#include <ve/ve.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace {
// The flat example ground; replace with your terrain's height function.
float groundHeight(glm::vec2) { return 0.0f; }

constexpr float kWorldHalfSize = 95.0f;      // the ground plane is 200 x 200
const glm::vec2 kPlayArea{0.0f, -2.0f};      // spheres, cubes and the fire live here
constexpr float kPlayAreaRadius = 14.0f;
} // namespace

void addGrass(ve::Renderer& renderer)
{
    ve::GrassDesc grass;
    grass.boundsMin = glm::vec2(-kWorldHalfSize);
    grass.boundsMax = glm::vec2(kWorldHalfSize);
    grass.resolution = 2;     // flat ground: the corners are enough (use your heightfield's resolution)
    grass.maskResolution = 256;
    grass.height = groundHeight;
    // x = density, y = colour mix (colorA -> colorB), z = height scale.
    grass.mask = [](glm::vec2 p) {
        const float clear = glm::smoothstep(kPlayAreaRadius - 4.0f, kPlayAreaRadius + 2.0f, glm::length(p - kPlayArea));
        const float patches = 0.5f + 0.5f * std::sin(p.x * 0.11f) * std::cos(p.y * 0.07f);
        return glm::vec4(clear * glm::mix(0.4f, 1.0f, patches), patches, glm::mix(0.6f, 1.0f, patches), 0.0f);
    };
    grass.colorA = {0.35f, 0.55f, 0.18f}; // lush
    grass.colorB = {0.62f, 0.6f, 0.28f};  // dry
    grass.maxTilt = 0.25f;                // random lean per clump
    grass.castShadows = true;             // within grass.shadowDistance (30 m)
    // Alpha-clipped cards instead of procedural blades:
    //   grass.cardMesh = ve::GrassDesc::cardMeshCrossed(3, 1.0f);
    //   grass.cardTexture = assets.loadTexture("textures/grass_card.png", true);
    renderer.graph().addPass<ve::GrassPass>(grass);
}

ve::FoliageSystem& addFoliage(ve::Scene& scene, ve::Renderer& renderer, ve::GameObject& colliderFocus)
{
    // One vertex-coloured material for all props.
    auto material = renderer.createStandardMaterial();
    material->setBaseColor(glm::vec4(1.0f)).setRoughness(0.85f).setVertexColorWeight(1.0f);

    // --- Meshes: a low-poly tree and a rock, built from primitives --------------------
    ve::MeshData tree;
    tree.append(ve::primitives::cylinder(0.28f, 0.18f, 2.6f, 7), glm::translate(glm::mat4(1.0f), {0.0f, 1.3f, 0.0f}),
                {0.45f, 0.32f, 0.2f, 1.0f});
    tree.append(ve::primitives::icosphere(1.5f, 1),
                glm::scale(glm::translate(glm::mat4(1.0f), {0.0f, 3.4f, 0.0f}), {1.0f, 0.8f, 1.0f}), {0.25f, 0.5f, 0.2f, 1.0f});
    tree.makeFlatShaded();
    ve::MeshData rock = ve::primitives::icosphere(0.6f, 1);
    rock.setColor({0.55f, 0.53f, 0.5f, 1.0f});
    rock.makeFlatShaded();

    // LOD chains: full mesh -> simplified mesh (meshoptimizer) -> for the tree an
    // octahedral impostor (a camera-facing card baked from the mesh).
    ve::LodChainDesc treeChain;
    treeChain.name = "tree";
    treeChain.levels = {{tree, {}, {material}}};
    treeChain.simplify = {{.ratio = 0.35f, .maxError = 0.1f, .faceted = true}};
    treeChain.impostor = true;
    treeChain.switches = {{.screenSize = 0.12f}, {.screenSize = 0.04f}}; // screen-size thresholds
    std::vector<ve::InstanceLod> treeLods = ve::buildLodChain(renderer, std::move(treeChain));
    auto rockMesh = ve::Mesh::create(renderer, rock, {}, "rock");

    // --- The foliage system: rules decide where things grow ------------------------------
    ve::FoliageWorld world;
    world.boundsMin = glm::vec2(-kWorldHalfSize);
    world.boundsMax = glm::vec2(kWorldHalfSize);
    world.height = groundHeight; // called on a worker thread: must only read immutable data
    auto& foliage = scene.spawn<ve::FoliageSystem>(world);
    foliage.addExclusion(kPlayArea, kPlayAreaRadius);

    ve::FoliageLayer trees;
    trees.name = "tree";
    trees.variants = {treeLods};
    trees.colliders = {ve::Collider::capsule(1.1f, 0.25f).offset({0.0f, 1.35f, 0.0f})}; // trunk only
    trees.spacing = 6.0f;          // at most one tree per 6 x 6 m
    trees.probability = 0.7f;
    trees.clusterFrequency = 0.03f; // groves
    trees.clusterCoverage = 0.4f;
    trees.minScale = 0.8f;
    trees.maxScale = 1.3f;
    trees.viewDistance = 400.0f;
    const uint32_t treeLayer = foliage.addLayer(trees);

    ve::FoliageLayer rocks;
    rocks.name = "rock";
    rocks.variants = {{{rockMesh, {material}}}};
    rocks.colliders = {ve::Collider::convexHull(rock)};
    rocks.spacing = 5.0f;
    rocks.probability = 0.25f;
    rocks.minScale = 0.4f;
    rocks.maxScale = 1.6f;
    rocks.sink = 0.2f;                   // half-buried
    rocks.avoid = {{treeLayer, 2.0f}};   // not right against a trunk
    rocks.viewDistance = 150.0f;         // small: stream in/out around the camera
    rocks.shadowDistance = 60.0f;
    foliage.addLayer(rocks);

    foliage.setColliderFocus(&colliderFocus); // remembered by id: safe if the player is deleted
    foliage.preload(colliderFocus.transform().position()); // no pop-in on the first frames
    return foliage;
}
