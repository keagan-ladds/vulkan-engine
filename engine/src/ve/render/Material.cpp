#include "ve/render/Material.hpp"

#include "ve/gfx/Device.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/render/Texture.hpp"

#include <algorithm>
#include <bit>

namespace ve {

// -----------------------------------------------------------------------------
// Shader
// -----------------------------------------------------------------------------

Shader::Shader(Renderer& renderer, ShaderDesc desc)
    : m_renderer(&renderer), m_desc(std::move(desc))
{
}

Shader::~Shader()
{
    invalidate();
}

VkPipeline Shader::pipeline(PassKind kind)
{
    VkPipeline& p = m_pipelines[static_cast<size_t>(kind)];
    if (!p) p = m_renderer->buildMeshPipeline(m_desc, kind);
    return p;
}

void Shader::invalidate()
{
    Device* device = &m_renderer->device();
    for (VkPipeline& p : m_pipelines) {
        if (p) {
            VkPipeline old = p;
            device->deferDestroy([device, old] { vkDestroyPipeline(device->handle(), old, nullptr); });
            p = VK_NULL_HANDLE;
        }
    }
}

// -----------------------------------------------------------------------------
// Material
// -----------------------------------------------------------------------------

Material::Material(Renderer& renderer, std::shared_ptr<Shader> shader)
    : m_renderer(&renderer), m_shader(std::move(shader))
{
    m_index = m_renderer->allocateMaterialSlot();
    GpuMaterial& d = data();
    d = GpuMaterial{};
    for (uint32_t& t : d.textures) t = kInvalidIndex;
}

Material::~Material()
{
    m_renderer->freeMaterialSlot(m_index);
}

GpuMaterial& Material::data()
{
    // Every write goes through here: remember the slot for the per-frame upload.
    m_renderer->markMaterialDirty(m_index);
    return m_renderer->materialData(m_index);
}

const GpuMaterial& Material::data() const
{
    return m_renderer->materialData(m_index);
}

void Material::setVector(uint32_t slot, const glm::vec4& value)
{
    if (slot < kMaterialParamCount) data().params[slot] = value;
}

glm::vec4 Material::vector(uint32_t slot) const
{
    return slot < kMaterialParamCount ? data().params[slot] : glm::vec4(0.0f);
}

void Material::setTexture(uint32_t slot, std::shared_ptr<Texture> texture)
{
    if (slot >= kMaterialTextureCount) return;
    data().textures[slot] = texture ? texture->index() : kInvalidIndex;
    m_textures[slot] = std::move(texture);
}

void Material::setTextureIndex(uint32_t slot, uint32_t bindlessIndex)
{
    if (slot >= kMaterialTextureCount) return;
    data().textures[slot] = bindlessIndex;
    m_textures[slot].reset();
}

uint32_t Material::textureIndex(uint32_t slot) const
{
    return slot < kMaterialTextureCount ? data().textures[slot] : kInvalidIndex;
}

// -----------------------------------------------------------------------------
// MaterialDirtySet
// -----------------------------------------------------------------------------

void MaterialDirtySet::mark(uint32_t index)
{
    if (index >= m_pending.size()) m_pending.resize(index + 1, 0);
    if (m_pending[index] == 0) m_dirty.push_back(index);
    m_pending[index] = kAllFrameSlots;
}

void MaterialDirtySet::take(uint32_t frameSlot, std::vector<std::pair<uint32_t, uint32_t>>& runs)
{
    runs.clear();
    if (m_dirty.empty()) return;
    std::sort(m_dirty.begin(), m_dirty.end());
    const auto bit = static_cast<uint8_t>(1u << frameSlot);
    for (const uint32_t i : m_dirty) {
        if (!(m_pending[i] & bit)) continue;
        m_pending[i] &= static_cast<uint8_t>(~bit);
        if (!runs.empty() && runs.back().first + runs.back().second == i) ++runs.back().second;
        else runs.push_back({i, 1});
    }
    // Keep a slot until every frame slot has received it.
    std::erase_if(m_dirty, [&](uint32_t i) { return m_pending[i] == 0; });
}

// -----------------------------------------------------------------------------
// StandardMaterial
// -----------------------------------------------------------------------------

static std::shared_ptr<Shader> standardShaderFor(Renderer& r, StandardMaterial::Mode mode)
{
    switch (mode) {
    case StandardMaterial::Mode::Cutout: return r.builtinShader(BuiltinShader::StandardCutout);
    case StandardMaterial::Mode::Transparent: return r.builtinShader(BuiltinShader::StandardTransparent);
    default: return r.builtinShader(BuiltinShader::Standard);
    }
}

StandardMaterial::StandardMaterial(Renderer& renderer, Mode mode)
    : Material(renderer, standardShaderFor(renderer, mode))
{
    setVector(0, glm::vec4(1.0f));
    setVector(1, glm::vec4(0.8f, 0.0f, 1.0f, mode == Mode::Cutout ? 0.5f : 0.0f));
    setVector(2, glm::vec4(0.0f));
    setVector(3, glm::vec4(1.0f, 1.0f, 1.0f, 0.0f));
}

// Stored inverted (the layers *not* received), so the all-zero parameters of a
// fresh material receive every decal.
StandardMaterial& StandardMaterial::setDecalLayers(uint32_t receive)
{
    glm::vec4 v = vector(3);
    v.w = std::bit_cast<float>(~receive);
    setVector(3, v);
    return *this;
}

uint32_t StandardMaterial::decalLayers() const
{
    return ~std::bit_cast<uint32_t>(vector(3).w);
}

StandardMaterial& StandardMaterial::setRoughness(float r)
{
    glm::vec4 v = vector(1);
    v.x = r;
    setVector(1, v);
    return *this;
}

StandardMaterial& StandardMaterial::setMetallic(float m)
{
    glm::vec4 v = vector(1);
    v.y = m;
    setVector(1, v);
    return *this;
}

StandardMaterial& StandardMaterial::setNormalStrength(float s)
{
    glm::vec4 v = vector(1);
    v.z = s;
    setVector(1, v);
    return *this;
}

StandardMaterial& StandardMaterial::setAlphaCutoff(float c)
{
    glm::vec4 v = vector(1);
    v.w = c;
    setVector(1, v);
    return *this;
}

StandardMaterial& StandardMaterial::setEmissive(const glm::vec3& color, float intensity)
{
    setVector(2, glm::vec4(color, intensity));
    return *this;
}

StandardMaterial& StandardMaterial::setUvScale(const glm::vec2& scale)
{
    glm::vec4 v = vector(3);
    v.x = scale.x;
    v.y = scale.y;
    setVector(3, v);
    return *this;
}

StandardMaterial& StandardMaterial::setVertexColorWeight(float w)
{
    glm::vec4 v = vector(3);
    v.z = w;
    setVector(3, v);
    return *this;
}

} // namespace ve
