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

/// Draw order buckets: lower queues draw first.
enum class RenderQueue : int {
    Background = 1000,
    Opaque = 2000,
    AlphaTest = 2450,
    Transparent = 3000, ///< >= Transparent: drawn by TransparentPass, sorted back to front
    Overlay = 4000,
};

/// The kinds of pass a mesh shader has a pipeline for.
enum class PassKind : uint32_t { Forward, ShadowDirectional, ShadowPoint, Count };

/// What a Shader is made of: its GLSL programs (SPIR-V names) and fixed pipeline state.
struct ShaderDesc {
    std::string name = "unnamed";  ///< Shown in logs and compile events ("water").
    std::string vertex = "standard.vert";   ///< SPIR-V file names without .spv, from the shader folder
    std::string fragment = "standard.frag";  ///< Fragment program (SPIR-V name without .spv).
    /// Shadow pass programs. The defaults handle any opaque mesh; override the
    /// vertex shader if your vertex shader moves vertices (e.g. wind sway), so
    /// shadows move too. Contract: see engine/shaders/shadow.vert.
    std::string shadowVertex = "shadow.vert";
    std::string shadowFragment;                             ///< directional shadows: empty = depth only (fastest)
    std::string shadowPointFragment = "shadow_point.frag";  ///< Point-light shadow fragment program (writes distance).
    BlendMode blend = BlendMode::Opaque;  ///< Colour blending (Opaque, AlphaBlend, Additive...).
    CullMode cull = CullMode::Back;  ///< Which faces are culled (None = double sided).
    bool depthTest = true;  ///< Test against the scene depth.
    bool depthWrite = true;  ///< Write the scene depth (off for most transparent shaders).
    bool castShadows = true;  ///< Draw into shadow maps.
    bool wireframe = false;  ///< Draw as lines (debug; needs fillModeNonSolid).
    RenderQueue queue = RenderQueue::Opaque;  ///< When it's drawn (see RenderQueue).
};

/// A shader: programs + pipeline state (ShaderDesc), shared by many materials.
/// Create with Renderer::createShader; its pipelines are built per pass kind.
class Shader {
public:
    /// Use Renderer::createShader (which also starts compiling its pipelines).
    Shader(Renderer& renderer, ShaderDesc desc);
    ~Shader();

    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    /// The description it was made from.
    const ShaderDesc& desc() const { return m_desc; }
    /// Whether it's drawn in the transparent queue (sorted, after opaque).
    bool transparent() const { return m_desc.queue >= RenderQueue::Transparent; }
    /// Built on first use (or from creation, async) and cached; VK_NULL_HANDLE
    /// while it compiles in the background or if it failed: skip the draw.
    VkPipeline pipeline(PassKind kind);
    /// Starts compiling the pipelines this shader will need (forward, and
    /// shadows if it casts them). Renderer::createShader does it already.
    void prewarm();
    /// Whether any of its pipelines is still compiling.
    bool compiling() const;
    /// Forces pipelines to be rebuilt (e.g. after changing desc or shader hot-reload).
    void invalidate();

private:
    friend class Renderer;
    Renderer* m_renderer;
    ShaderDesc m_desc;
    std::array<VkPipeline, static_cast<size_t>(PassKind::Count)> m_pipelines{};
    std::array<std::shared_ptr<PipelineJob>, static_cast<size_t>(PassKind::Count)> m_jobs{}; ///< compiling
    std::array<bool, static_cast<size_t>(PassKind::Count)> m_failed{}; ///< (not retried until invalidate())
};

/// A Shader plus its parameters (12 vec4s + 16 texture indices) in the
/// global material buffer; shaders read it with veMaterial(). Shared by
/// every renderer that uses it.
class Material {
public:
    /// Use Renderer::createMaterial (or createStandardMaterial).
    Material(Renderer& renderer, std::shared_ptr<Shader> shader);
    virtual ~Material();

    Material(const Material&) = delete;
    Material& operator=(const Material&) = delete;

    /// The shader it draws with.
    Shader& shader() const { return *m_shader; }
    /// The shader, shared.
    const std::shared_ptr<Shader>& shaderPtr() const { return m_shader; }
    /// Switches the shader (the parameters stay; they must mean the same to the new one).
    void setShader(std::shared_ptr<Shader> shader) { m_shader = std::move(shader); }

    /// Raw parameter access (slots 0..11 = vec4 params, 0..15 = texture indices).
    void setVector(uint32_t slot, const glm::vec4& value);
    /// A vec4 parameter slot's value.
    glm::vec4 vector(uint32_t slot) const;
    /// Keeps the texture alive for as long as the material references it.
    void setTexture(uint32_t slot, std::shared_ptr<Texture> texture);
    /// Sets a texture slot to a bindless index directly (no ownership kept).
    void setTextureIndex(uint32_t slot, uint32_t bindlessIndex);
    /// A texture slot's bindless index (kInvalidIndex = none).
    uint32_t textureIndex(uint32_t slot) const;
    /// The texture set in a slot with setTexture (null otherwise).
    const std::shared_ptr<Texture>& texture(uint32_t slot) const { return m_textures[slot]; }

    /// Index of its data in the global material buffer (what draws push).
    uint32_t gpuIndex() const { return m_index; }

    /// Free-form user tag (e.g. for gameplay: "wood", "stone").
    std::string name;

protected:
    /// Its GPU data, marked for upload (write parameters through this).
    GpuMaterial& data();
    /// Its GPU data (read only).
    const GpuMaterial& data() const;

    Renderer* m_renderer;  ///< The renderer that owns its slot.
    std::shared_ptr<Shader> m_shader;  ///< The shader it draws with.
    uint32_t m_index = kInvalidIndex;  ///< Its slot in the material buffer.
    std::array<std::shared_ptr<Texture>, kMaterialTextureCount> m_textures;  ///< Textures kept alive per slot.
};

/// Engine internal: which GpuMaterial slots changed and which per-frame material
/// buffers haven't received them yet. The Renderer keeps one buffer per frame
/// slot, so a change is copied once into each of them (on that slot's next
/// frame) instead of every material into every frame.
class MaterialDirtySet {
public:
    /// Slot `index` changed (Material::data() written, or a material created).
    void mark(uint32_t index);
    /// Runs {first, count} of slots that frame slot `frameSlot` hasn't received
    /// yet, in ascending order; they count as received afterwards.
    void take(uint32_t frameSlot, std::vector<std::pair<uint32_t, uint32_t>>& runs);

private:
    static constexpr uint8_t kAllFrameSlots = (1u << Device::kFramesInFlight) - 1u;
    std::vector<uint8_t> m_pending; ///< per material slot: bit s = frame slot s still needs it
    std::vector<uint32_t> m_dirty;  ///< slots with any bit set (unsorted, no duplicates)
};

/// The built-in PBR material (standard.vert/frag). Parameter layout:
///   params[0] = base colour (rgba)
///   params[1] = roughness, metallic, normal strength, alpha cutoff (0 = off)
///   params[2] = emissive rgb, emissive intensity
///   params[3] = uv scale xy, vertex colour weight (0..1),
///               w = decal layers it does NOT receive (uint bits; 0 = receives every decal)
///   params[11] = translucency: rgb = colour of the light passing through
///               (times the albedo), w = amount 0..1 (0 = off)
///   textures  = 0 albedo, 1 normal, 2 metallic-roughness (glTF: G=rough, B=metal),
///               3 emissive, 4 occlusion (R), 15 translucency map (R)
/// Custom shaders may use params 4-10 and textures 5-14 for their own inputs.
class StandardMaterial : public Material {
public:
    /// Opaque; Cutout (alpha-tested, double sided); Transparent (blended, sorted).
    enum class Mode { Opaque, Cutout, Transparent };

    /// A standard material in `mode` (normally via Renderer::createStandardMaterial).
    explicit StandardMaterial(Renderer& renderer, Mode mode = Mode::Opaque);

    /// Base colour (linear RGBA; alpha: cutout / transparency). Multiplies the albedo texture.
    StandardMaterial& setBaseColor(const glm::vec4& c) { setVector(0, c); return *this; }
    /// Roughness 0..1 (multiplies the metallic-roughness texture's G).
    StandardMaterial& setRoughness(float r);
    /// Metallic 0..1 (multiplies the texture's B).
    StandardMaterial& setMetallic(float m);
    /// Normal-map strength (0 = flat, 1 = as authored).
    StandardMaterial& setNormalStrength(float s);
    /// Cutout: alpha below this is discarded (0 = no alpha test).
    StandardMaterial& setAlphaCutoff(float c);
    /// Emitted light: colour (linear RGB) times intensity (HDR: > 1 blooms).
    StandardMaterial& setEmissive(const glm::vec3& color, float intensity = 1.0f);
    /// Texture repeats across the mesh's UVs.
    StandardMaterial& setUvScale(const glm::vec2& scale);
    StandardMaterial& setVertexColorWeight(float w); ///< 1 = multiply albedo by vertex colour
    /// Which decals land on it: those whose layers overlap `receive` (Decals.hpp;
    /// default kDecalLayerAll, 0 = none). E.g. characters: ~kDecalLayerWorld.
    StandardMaterial& setDecalLayers(uint32_t receive);
    /// The decal layers it receives.
    uint32_t decalLayers() const;
    /// Colour texture (sRGB), slot 0.
    StandardMaterial& setAlbedoTexture(std::shared_ptr<Texture> t) { setTexture(0, std::move(t)); return *this; }
    /// Normal map, slot 1 (use veUnpackNormal in custom shaders).
    StandardMaterial& setNormalTexture(std::shared_ptr<Texture> t) { setTexture(1, std::move(t)); return *this; }
    /// Metallic-roughness texture (glTF: G = roughness, B = metallic), slot 2.
    StandardMaterial& setMetallicRoughnessTexture(std::shared_ptr<Texture> t) { setTexture(2, std::move(t)); return *this; }
    /// Emissive colour texture, slot 3.
    StandardMaterial& setEmissiveTexture(std::shared_ptr<Texture> t) { setTexture(3, std::move(t)); return *this; }
    /// Ambient-occlusion texture (R), slot 4.
    StandardMaterial& setOcclusionTexture(std::shared_ptr<Texture> t) { setTexture(4, std::move(t)); return *this; }

    static constexpr uint32_t kTranslucencyParam = 11;   ///< params slot of the translucency (see setTranslucency)
    static constexpr uint32_t kTranslucencyTexture = 15; ///< texture slot of the translucency map

    /// Thin translucent surfaces (leaves, paper, cloth, lampshades): light
    /// reaching the back passes through, spread evenly (like Blender's
    /// Translucent BSDF). That share is no longer reflected at the front:
    /// the front's diffuse colour becomes albedo * (1 - amount), and the back
    /// lets albedo * color * amount through. Metals don't transmit.
    /// @param amount 0..1 (0 = off: no cost), times the translucency map's R.
    /// @param color Linear tint of the transmitted light (white = the albedo's colour).
    StandardMaterial& setTranslucency(float amount, const glm::vec3& color = glm::vec3(1.0f));
    /// The translucency amount (params[11].w).
    float translucency() const { return vector(kTranslucencyParam).w; }
    /// Where light gets through (R, linear: 1 = thin leaf, 0 = veins / stem), slot 15.
    StandardMaterial& setTranslucencyTexture(std::shared_ptr<Texture> t) { setTexture(kTranslucencyTexture, std::move(t)); return *this; }
};

} // namespace ve
