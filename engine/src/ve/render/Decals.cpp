#include "ve/render/Decals.hpp"

#include "ve/render/Texture.hpp"

#include <algorithm>
#include <cmath>

namespace ve {

void Decal::toGpu(const glm::mat4& world, float opacity, GpuDecal& out) const
{
    // World -> box space. The rows of the inverse are the box's axes divided
    // by its size, which the shader also uses as directions (normalized).
    const glm::mat4 inv = glm::inverse(world);
    for (int r = 0; r < 3; ++r) out.rows[r] = glm::vec4(inv[0][r], inv[1][r], inv[2][r], inv[3][r]);
    out.color = glm::vec4(glm::vec3(color), std::clamp(color.a * opacity, 0.0f, 1.0f));
    out.opacity = glm::clamp(glm::vec4(albedoOpacity, normalOpacity, roughnessOpacity, metallicOpacity), 0.0f, 1.0f);
    out.params = glm::vec4(roughness, metallic, normalStrength, std::clamp(flatten, 0.0f, 1.0f));
    // smoothstep(end, start, facing) in the shader: keep start > end.
    float start = std::cos(glm::radians(std::clamp(angleFadeStart, 0.0f, 179.0f)));
    float end = std::cos(glm::radians(std::clamp(angleFadeEnd, 0.0f, 180.0f)));
    if (end >= start - 1e-3f) end = start - 1e-3f;
    out.fade = glm::vec4(start, end, std::clamp(depthFade, 1e-3f, 1.0f), blend == DecalBlend::Multiply ? 1.0f : 0.0f);
    out.textures = glm::uvec4(albedoTexture ? albedoTexture->index() : kInvalidIndex,
                              normalTexture ? normalTexture->index() : kInvalidIndex,
                              maskTexture ? maskTexture->index() : kInvalidIndex, layers);
}

glm::mat4 DecalSystem::onSurface(const glm::vec3& point, const glm::vec3& normal, const glm::vec2& size, float depth,
                                 float spin)
{
    // The box's +Z = the surface normal (it projects along -Z, into the
    // surface); +Y = "up" of the texture, chosen to point as far up the world
    // as possible (a crack on a wall stays upright), then spun.
    const glm::vec3 z = glm::normalize(normal);
    const glm::vec3 ref = std::abs(z.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, -1);
    glm::vec3 x = glm::normalize(glm::cross(ref, z));
    glm::vec3 y = glm::cross(z, x);
    const float c = std::cos(spin), s = std::sin(spin);
    const glm::vec3 xs = x * c + y * s, ys = y * c - x * s;
    glm::mat4 m(1.0f);
    m[0] = glm::vec4(xs * size.x, 0.0f);
    m[1] = glm::vec4(ys * size.y, 0.0f);
    m[2] = glm::vec4(z * depth, 0.0f);
    m[3] = glm::vec4(point, 1.0f);
    return m;
}

DecalSystem::Handle DecalSystem::spawn(const Decal& decal, const glm::mat4& world, float lifetime, float fadeOut)
{
    if (m_capacity == 0) return kNone;
    // A ring: every spawn writes the next slot, so the slot written over is
    // always the oldest (or a free one).
    if (m_entries.size() < m_capacity) m_entries.resize(m_capacity);
    Entry& e = m_entries[m_next];
    if (e.serial == 0) ++m_live;
    m_next = (m_next + 1) % m_capacity;
    e.decal = decal;
    e.world = world;
    e.spawnTime = m_time;
    e.lifetime = std::max(lifetime, 0.0f);
    e.fadeOut = std::max(fadeOut, 0.0f);
    e.serial = ++m_serial;
    return e.serial;
}

DecalSystem::Entry* DecalSystem::find(Handle handle)
{
    if (handle == kNone) return nullptr;
    for (Entry& e : m_entries) {
        if (e.serial == handle) return &e;
    }
    return nullptr;
}

void DecalSystem::remove(Handle handle)
{
    if (Entry* e = find(handle)) {
        *e = Entry{}; // (releases its textures)
        --m_live;
    }
}

void DecalSystem::setTransform(Handle handle, const glm::mat4& world)
{
    if (Entry* e = find(handle)) e->world = world;
}

void DecalSystem::clear()
{
    m_entries.clear();
    m_next = 0;
    m_live = 0;
}

void DecalSystem::setCapacity(uint32_t capacity)
{
    if (capacity == m_capacity) return;
    // Keep the newest `capacity` decals, oldest first, so the ring order holds.
    std::vector<Entry> live;
    for (Entry& e : m_entries) {
        if (e.serial != 0) live.push_back(std::move(e));
    }
    std::sort(live.begin(), live.end(), [](const Entry& a, const Entry& b) { return a.serial < b.serial; });
    if (live.size() > capacity) live.erase(live.begin(), live.end() - long(capacity));
    m_capacity = capacity;
    m_live = static_cast<uint32_t>(live.size());
    m_next = capacity == 0 ? 0 : m_live % capacity;
    m_entries = std::move(live);
    if (capacity > 0) m_entries.resize(capacity);
}

float DecalSystem::lifeFade(const Entry& e, float time)
{
    if (e.lifetime <= 0.0f) return 1.0f;
    const float over = time - e.spawnTime - e.lifetime; // seconds into the fade-out
    if (over <= 0.0f) return 1.0f;
    return e.fadeOut > 0.0f ? std::max(0.0f, 1.0f - over / e.fadeOut) : 0.0f;
}

void DecalSystem::update(float time)
{
    m_time = time;
    for (Entry& e : m_entries) {
        if (e.serial == 0 || e.lifetime <= 0.0f) continue;
        if (time - e.spawnTime >= e.lifetime + e.fadeOut) {
            e = Entry{};
            --m_live;
        }
    }
}

} // namespace ve
