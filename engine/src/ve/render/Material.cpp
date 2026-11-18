#include "ve/render/Material.hpp"

#include <algorithm>

#include "ve/gfx/Device.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/render/Texture.hpp"

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
    return m_renderer->materialData(m_index);
}

const GpuMaterial& Material::data() const
{
    return m_renderer->materialData(m_index);
}

void Material::setVector(uint32_t slot, const glm::vec4& value)
{
    if (slot < 12) data().params[slot] = value;
}

glm::vec4 Material::vector(uint32_t slot) const
{
    return slot < 12 ? data().params[slot] : glm::vec4(0.0f);
}

void Material::setTexture(uint32_t slot, std::shared_ptr<Texture> texture)
{
    if (slot >= 16) return;
    data().textures[slot] = texture ? texture->index() : kInvalidIndex;
    m_textures[slot] = std::move(texture);
}

void Material::setTextureIndex(uint32_t slot, uint32_t bindlessIndex)
{
    if (slot >= 16) return;
    data().textures[slot] = bindlessIndex;
    m_textures[slot].reset();
}

uint32_t Material::textureIndex(uint32_t slot) const
{
    return slot < 16 ? data().textures[slot] : kInvalidIndex;
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
    setVector(4, glm::vec4(1.0f));
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

StandardMaterial& StandardMaterial::setTranslucency(float strength, const glm::vec3& tint)
{
    glm::vec4 v = vector(3);
    v.w = std::max(strength, 0.0f);
    setVector(3, v);
    setVector(4, glm::vec4(tint, 1.0f));
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
