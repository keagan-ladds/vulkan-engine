#include "ve/render/Material.hpp"

#include "ve/core/Log.hpp"
#include "ve/gfx/Device.hpp"
#include "ve/render/PipelineCompiler.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/render/Texture.hpp"

#include <algorithm>
#include <exception>

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
    release();
}

namespace {
void destroyLater(Device* device, VkPipeline p)
{
    if (p) device->deferDestroy([device, p] { vkDestroyPipeline(device->handle(), p, nullptr); });
}

// A build the shader no longer wants. Still running: PipelineCompiler::poll
// destroys the result when it finishes. Already finished (poll may have
// dropped it from its list): destroy the result here.
void abandon(Device* device, std::shared_ptr<PipelineCompile>& c)
{
    if (!c) return;
    if (c->job.done()) {
        if (!c->claimed) destroyLater(device, c->pipeline);
        c->pipeline = VK_NULL_HANDLE;
        c->claimed = true;
    } else {
        c->abandoned = true;
    }
    c.reset();
}
} // namespace

void Shader::start(size_t kind)
{
    if (m_compiles[kind]) return;
    try {
        m_compiles[kind] = m_renderer->pipelineCompiler().submit(m_renderer->meshPipelineInputs(m_desc, PassKind(kind)));
    } catch (const std::exception& e) { // e.g. a missing .spv
        log::error("Shader '{}': {}", m_desc.name, e.what());
        m_failed[kind] = true;
    }
}

void Shader::claim(size_t kind)
{
    PipelineCompile& c = *m_compiles[kind];
    c.claimed = true;
    if (c.pipeline) {
        destroyLater(&m_renderer->device(), m_pipelines[kind]); // the one it replaces (hot-reload)
        m_pipelines[kind] = c.pipeline;
        c.pipeline = VK_NULL_HANDLE;
    } else {
        if (!c.error.empty()) log::error("Shader '{}': {}", m_desc.name, c.error);
        if (!m_pipelines[kind]) m_failed[kind] = true; // a failed rebuild keeps the old one
    }
    m_compiles[kind].reset();
}

VkPipeline Shader::pipeline(PassKind kind)
{
    const size_t k = static_cast<size_t>(kind);
    if (!m_compiles[k]) {
        if (m_pipelines[k] || m_failed[k]) return m_pipelines[k];
        start(k);
        if (!m_compiles[k]) return VK_NULL_HANDLE;
    }
    // Headless / async off: build right here (runs the job inline if no worker took it yet).
    if (!m_renderer->asyncPipelines()) m_compiles[k]->job.wait();
    if (m_compiles[k]->job.done()) claim(k);
    return m_pipelines[k]; // the old pipeline while a rebuild is still going
}

void Shader::prewarm()
{
    if (!m_renderer->asyncPipelines()) return; // the old behaviour: built (blocking) on first use
    start(size_t(PassKind::Forward));
    if (m_desc.castShadows) {
        start(size_t(PassKind::ShadowDirectional));
        start(size_t(PassKind::ShadowPoint));
    }
}

void Shader::invalidate()
{
    for (size_t k = 0; k < kKinds; ++k) {
        bool wanted = m_pipelines[k] != VK_NULL_HANDLE;
        m_failed[k] = false;
        if (m_compiles[k]) { // a build of the old version: let it go
            abandon(&m_renderer->device(), m_compiles[k]);
            wanted = true;
        }
        if (wanted) start(k); // the rest is built on first use
    }
}

void Shader::release()
{
    Device* device = &m_renderer->device();
    for (size_t k = 0; k < kKinds; ++k) {
        abandon(device, m_compiles[k]);
        destroyLater(device, m_pipelines[k]);
        m_pipelines[k] = VK_NULL_HANDLE;
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
