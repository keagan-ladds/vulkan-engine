// =============================================================================
// My Game - starting point for a game built on vulkan-engine.
//
// This file shows the usual pieces; delete what you don't need:
//   - engine setup + optional effects (sky, clouds, fog, bloom, particles)
//   - a sun whose colour comes from the atmosphere
//   - the built-in PBR material (StandardMaterial) with a texture from assets/
//   - a custom shader/material (shaders/mygame_toon.frag)
//   - GameObjects with behaviour (CameraController) and a reusable Component (Spinner)
//   - a point light with shadows + a particle emitter
//
// Engine docs: <engine>/docs (architecture, recipes in 03-conventions.md).
// =============================================================================

#include "CameraController.hpp"
#include "Spinner.hpp"

#include <ve/ve.hpp>
#include <vfx/Atmosphere.hpp>
#include <vfx/Particles.hpp>
#include <vfx/Vfx.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>

int main()
{
    try {
        ve::EngineConfig config;
        config.title = "My Game";
        config.width = 1280;
        config.height = 720;
        config.renderer.maxShadowedPointLights = 2;
        ve::Engine engine(config);

        ve::Renderer& renderer = engine.renderer();
        ve::Scene& scene = engine.scene();
        ve::AssetManager& assets = engine.assets();

        // --- Optional effects ---------------------------------------------------------
        // Remove this (and NO_EFFECTS in CMake) for a plain renderer.
        vfx::Effects fx = vfx::addDefaultEffects(renderer);

        // --- Sun -------------------------------------------------------------------------
        const glm::vec3 towardsSun = glm::normalize(glm::vec3(-0.6f, 0.35f, -0.5f));
        auto& sunObject = scene.spawn("Sun");
        sunObject.transform().setRotation(glm::quatLookAt(-towardsSun, glm::vec3(0, 1, 0)));
        auto& sun = sunObject.addComponent<ve::DirectionalLight>();
        sun.intensity = 3.0f;
        // Sunlight that passed through the atmosphere: warm when the sun is low.
        const glm::vec3 t = vfx::AtmospherePass::sunTransmittance(towardsSun);
        sun.color = t / std::max({t.r, t.g, t.b});
        fx.atmosphere->sun = &sun; // the sky draws its sun disc where this light comes from

        // --- Ground: built-in PBR material with a texture from assets/ -----------------
        auto groundMat = renderer.createStandardMaterial();
        groundMat->setAlbedoTexture(assets.loadTexture("textures/checker.png"))
            .setUvScale({25.0f, 25.0f})
            .setRoughness(0.95f);
        auto& ground = scene.spawn("Ground");
        ground.addComponent<ve::MeshRenderer>(ve::Mesh::create(renderer, ve::primitives::plane(200, 200)), groundMat);

        // --- A row of PBR spheres: roughness left to right, metal on the second row --------
        auto sphere = ve::Mesh::create(renderer, ve::primitives::sphere(0.5f));
        for (int row = 0; row < 2; ++row) {
            for (int i = 0; i < 5; ++i) {
                auto mat = renderer.createStandardMaterial();
                mat->setBaseColor(row == 0 ? glm::vec4(0.8f, 0.2f, 0.1f, 1.0f) : glm::vec4(0.95f, 0.75f, 0.4f, 1.0f))
                    .setRoughness(0.1f + 0.2f * static_cast<float>(i))
                    .setMetallic(static_cast<float>(row));
                auto& go = scene.spawn("Sphere");
                go.addComponent<ve::MeshRenderer>(sphere, mat);
                go.transform().setPosition({-4.0f + 2.0f * static_cast<float>(i), 0.5f + 1.2f * static_cast<float>(row), -6.0f});
            }
        }

        // --- Custom shader: toon shading (not PBR), see shaders/mygame_toon.frag ----------
        ve::ShaderDesc toonDesc;
        toonDesc.name = "Toon";
        toonDesc.vertex = "standard.vert";       // reuse the engine's vertex shader
        toonDesc.fragment = "mygame_toon.frag";  // our own fragment shader
        auto toon = renderer.createShader(toonDesc);
        auto cube = ve::Mesh::create(renderer, ve::primitives::cube());
        const glm::vec4 colors[3] = {{0.2f, 0.6f, 0.9f, 1}, {0.9f, 0.8f, 0.2f, 1}, {0.4f, 0.85f, 0.3f, 1}};
        for (int i = 0; i < 3; ++i) {
            auto mat = renderer.createMaterial(toon);
            mat->setVector(0, colors[i]);          // params[0] = colour
            mat->setVector(1, {3.0f, 0, 0, 0});    // params[1].x = number of light bands
            auto& go = scene.spawn("ToonCube");
            go.addComponent<ve::MeshRenderer>(cube, mat);
            go.addComponent<Spinner>().speed = 0.5f + 0.4f * static_cast<float>(i);
            go.transform().setPosition({-3.0f + 3.0f * static_cast<float>(i), 1.0f, -1.0f});
        }

        // --- A "fire": point light with cube shadows + particles ----------------------------
        auto& fire = scene.spawn("Fire");
        fire.transform().setPosition({4.0f, 0.1f, 2.0f});
        auto& light = fire.addComponent<ve::PointLight>();
        light.color = {1.0f, 0.55f, 0.2f};
        light.intensity = 15.0f;
        light.range = 12.0f;
        light.castShadows = true;
        auto& flames = fire.addComponent<vfx::ParticleEmitter>().settings;
        flames.rate = 60.0f;
        flames.blend = ve::BlendMode::Additive;
        flames.spawnRadius = 0.25f;
        flames.velocity = {0.0f, 1.5f, 0.0f};
        flames.lifetimeMin = 0.4f;
        flames.lifetimeMax = 0.9f;
        flames.sizeStart = 0.3f;
        flames.sizeEnd = 0.05f;
        flames.colorStart = {5.0f, 2.0f, 0.5f, 1.0f}; // HDR > 1 => glows with bloom
        flames.colorMid = {3.0f, 0.8f, 0.1f, 0.7f};
        flames.colorEnd = {0.5f, 0.1f, 0.0f, 0.0f};

        // --- Camera ------------------------------------------------------------------------
        auto& camera = scene.spawn<CameraController>();
        camera.transform().setPosition({0.0f, 2.5f, 8.0f});

        engine.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
