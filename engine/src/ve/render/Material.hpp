#pragma once

// =============================================================================
// Shader & Material
//
// Shader   = which GLSL programs to run + fixed pipeline state (blending,
//            culling, depth, render queue, shadow casting). Create once,
//            share between many materials. Pipelines are built per pass kind
//            (forward / directional shadow / point shadow), on the job pool
//            from when the shader is created (RendererSettings::asyncPipelines):
//            pipeline() is null until then, and draws using it are skipped.
//
// Material = a Shader + its parameters (12 vec4s + 16 texture indices) that
//            live in the global MaterialBuffer (set 0, binding 2). A shader
//            reads its material with `veMaterial()` (see ve.glsl).
//
// Custom material example:
//     ShaderDesc d;
//     d.name = "water"; d.vertex = "water.vert"; d.fragment = "water.frag";
//     d.blend = BlendMode::AlphaBlend; d.queue = RenderQueue::Transparent;
//     auto shader = renderer.createShader(d);
//     auto mat = renderer.createMaterial(shader);
//     mat->setVector(0, {0.1, 0.3, 0.35, 0.8});   // whatever water.frag expects in params[0]
// =============================================================================

#include "ve/gfx/Device.hpp"
#include "ve/gfx/Pipeline.hpp"
#include "ve/render/GpuTypes.hpp"

#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ve {

class Renderer;
class Texture;
struct PipelineJob;

enum class RenderQueue : int {
    Background = 1000,
    Opaque = 2000,
    AlphaTest = 2450,
    Transparent = 3000, // >= Transparent: drawn by TransparentPass, sorted back to front
    Overlay = 4000,
};

enum class PassKind : uint32_t { Forward, ShadowDirectional, ShadowPoint, Count };

struct ShaderDesc {
    std::string name = "unnamed";
    std::string vertex = "standard.vert";   // SPIR-V file names without .spv, from the shader folder
    std::string fragment = "standard.frag";
    // Shadow pass programs. The defaults handle any opaque mesh; override the
    // vertex shader if your vertex shader moves vertices (e.g. wind sway), so
    // shadows move too. Contract: see engine/shaders/shadow.vert.
    std::string shadowVertex = "shadow.vert";
    std::string shadowFragment;                             // directional shadows: empty = depth only (fastest)
    std::string shadowPointFragment = "shadow_point.frag";
    BlendMode blend = BlendMode::Opaque;
    CullMode cull = CullMode::Back;
    bool depthTest = true;
    bool depthWrite = true;
    bool castShadows = true;
    bool wireframe = false;
    RenderQueue queue = RenderQueue::Opaque;
};

class Shader {
public:
    Shader(Renderer& renderer, ShaderDesc desc);
    ~Shader();

    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    const ShaderDesc& desc() const { return m_desc; }
    bool transparent() const { return m_desc.queue >= RenderQueue::Transparent; }
    // Built on first use (or from creation, async) and cached; VK_NULL_HANDLE
    // while it compiles in the background or if it failed: skip the draw.
    VkPipeline pipeline(PassKind kind);
    // Starts compiling the pipelines this shader will need (forward, and
    // shadows if it casts them). Renderer::createShader does it already.
    void prewarm();
    bool compiling() const;
    // Forces pipelines to be rebuilt (e.g. after changing desc or shader hot-reload).
    void invalidate();

private:
    friend class Renderer;
    Renderer* m_renderer;
    ShaderDesc m_desc;
    std::array<VkPipeline, static_cast<size_t>(PassKind::Count)> m_pipelines{};
    std::array<std::shared_ptr<PipelineJob>, static_cast<size_t>(PassKind::Count)> m_jobs{}; // compiling
    std::array<bool, static_cast<size_t>(PassKind::Count)> m_failed{}; // (not retried until invalidate())
};

class Material {
public:
    Material(Renderer& renderer, std::shared_ptr<Shader> shader);
    virtual ~Material();

    Material(const Material&) = delete;
    Material& operator=(const Material&) = delete;

    Shader& shader() const { return *m_shader; }
    const std::shared_ptr<Shader>& shaderPtr() const { return m_shader; }
    void setShader(std::shared_ptr<Shader> shader) { m_shader = std::move(shader); }

    // Raw parameter access (slots 0..11 = vec4 params, 0..15 = texture indices).
    void setVector(uint32_t slot, const glm::vec4& value);
    glm::vec4 vector(uint32_t slot) const;
    // Keeps the texture alive for as long as the material references it.
    void setTexture(uint32_t slot, std::shared_ptr<Texture> texture);
    void setTextureIndex(uint32_t slot, uint32_t bindlessIndex);
    uint32_t textureIndex(uint32_t slot) const;
    const std::shared_ptr<Texture>& texture(uint32_t slot) const { return m_textures[slot]; }

    uint32_t gpuIndex() const { return m_index; }

    // Free-form user tag (e.g. for gameplay: "wood", "stone").
    std::string name;

protected:
    GpuMaterial& data();
    const GpuMaterial& data() const;

    Renderer* m_renderer;
    std::shared_ptr<Shader> m_shader;
    uint32_t m_index = kInvalidIndex;
    std::array<std::shared_ptr<Texture>, kMaterialTextureCount> m_textures;
};

// Engine internal: which GpuMaterial slots changed and which per-frame material
// buffers haven't received them yet. The Renderer keeps one buffer per frame
// slot, so a change is copied once into each of them (on that slot's next
// frame) instead of every material into every frame.
class MaterialDirtySet {
public:
    // Slot `index` changed (Material::data() written, or a material created).
    void mark(uint32_t index);
    // Runs {first, count} of slots that frame slot `frameSlot` hasn't received
    // yet, in ascending order; they count as received afterwards.
    void take(uint32_t frameSlot, std::vector<std::pair<uint32_t, uint32_t>>& runs);

private:
    static constexpr uint8_t kAllFrameSlots = (1u << Device::kFramesInFlight) - 1u;
    std::vector<uint8_t> m_pending; // per material slot: bit s = frame slot s still needs it
    std::vector<uint32_t> m_dirty;  // slots with any bit set (unsorted, no duplicates)
};

// The built-in PBR material (standard.vert/frag). Parameter layout:
//   params[0] = base colour (rgba)
//   params[1] = roughness, metallic, normal strength, alpha cutoff (0 = off)
//   params[2] = emissive rgb, emissive intensity
//   params[3] = uv scale xy, vertex colour weight (0..1),
//               w = decal layers it does NOT receive (uint bits; 0 = receives every decal)
//   textures  = 0 albedo, 1 normal, 2 metallic-roughness (glTF: G=rough, B=metal),
//               3 emissive, 4 occlusion (R)
class StandardMaterial : public Material {
public:
    enum class Mode { Opaque, Cutout, Transparent };

    explicit StandardMaterial(Renderer& renderer, Mode mode = Mode::Opaque);

    StandardMaterial& setBaseColor(const glm::vec4& c) { setVector(0, c); return *this; }
    StandardMaterial& setRoughness(float r);
    StandardMaterial& setMetallic(float m);
    StandardMaterial& setNormalStrength(float s);
    StandardMaterial& setAlphaCutoff(float c);
    StandardMaterial& setEmissive(const glm::vec3& color, float intensity = 1.0f);
    StandardMaterial& setUvScale(const glm::vec2& scale);
    StandardMaterial& setVertexColorWeight(float w); // 1 = multiply albedo by vertex colour
    // Which decals land on it: those whose layers overlap `receive` (Decals.hpp;
    // default kDecalLayerAll, 0 = none). E.g. characters: ~kDecalLayerWorld.
    StandardMaterial& setDecalLayers(uint32_t receive);
    uint32_t decalLayers() const;
    StandardMaterial& setAlbedoTexture(std::shared_ptr<Texture> t) { setTexture(0, std::move(t)); return *this; }
    StandardMaterial& setNormalTexture(std::shared_ptr<Texture> t) { setTexture(1, std::move(t)); return *this; }
    StandardMaterial& setMetallicRoughnessTexture(std::shared_ptr<Texture> t) { setTexture(2, std::move(t)); return *this; }
    StandardMaterial& setEmissiveTexture(std::shared_ptr<Texture> t) { setTexture(3, std::move(t)); return *this; }
    StandardMaterial& setOcclusionTexture(std::shared_ptr<Texture> t) { setTexture(4, std::move(t)); return *this; }
};

} // namespace ve
