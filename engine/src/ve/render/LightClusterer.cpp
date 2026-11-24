#include "ve/render/LightClusterer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ve {

glm::vec2 LightClusterer::sliceParams(const CameraData& cam)
{
    const float z0 = std::max(cam.nearPlane, 0.5f), z1 = std::max(cam.farPlane, z0 * 2.0f);
    const float scale = float(kClusterZ) / std::log(z1 / z0);
    return {scale, -std::log(z0) * scale};
}

// A box covers the screen tiles of its projection in the depth slices it
// spans (all tiles if it reaches in front of the near plane).
LightClusterer::ClusterRange LightClusterer::rangeOf(const CameraData& cam, const glm::vec2& slicing, const glm::vec3& lo,
                                                     const glm::vec3& hi)
{
    auto slice = [&](float z) { return std::clamp(int(std::floor(std::log(std::max(z, 1e-3f)) * slicing.x + slicing.y)), 0, int(kClusterZ) - 1); };
    const float nearDepth = -hi.z, farDepth = -lo.z; // view space looks down -Z
    ClusterRange R{0, int(kClusterX) - 1, 0, int(kClusterY) - 1, slice(nearDepth), slice(farDepth)};
    if (farDepth < 0.0f) {
        R.z1 = R.z0 - 1; // behind the camera
        return R;
    }
    if (nearDepth > cam.nearPlane) {
        // In front of the camera: the screen rectangle of the box.
        glm::vec2 a(1e9f), b(-1e9f);
        for (int c = 0; c < 8; ++c) {
            const glm::vec3 v(c & 1 ? hi.x : lo.x, c & 2 ? hi.y : lo.y, c & 4 ? hi.z : lo.z);
            const glm::vec4 clip = cam.proj * glm::vec4(v, 1.0f);
            const glm::vec2 uv = glm::vec2(clip) / clip.w * 0.5f + 0.5f;
            a = glm::min(a, uv);
            b = glm::max(b, uv);
        }
        if (b.x < 0.0f || b.y < 0.0f || a.x > 1.0f || a.y > 1.0f) {
            R.z1 = R.z0 - 1; // off screen
            return R;
        }
        R.x0 = std::clamp(int(a.x * kClusterX), 0, int(kClusterX) - 1);
        R.x1 = std::clamp(int(b.x * kClusterX), 0, int(kClusterX) - 1);
        R.y0 = std::clamp(int(a.y * kClusterY), 0, int(kClusterY) - 1);
        R.y1 = std::clamp(int(b.y * kClusterY), 0, int(kClusterY) - 1);
    }
    return R;
}

template <class IndexT>
uint32_t LightClusterer::fill(uint32_t count, uint32_t shift, uint32_t* clusters, uint32_t* words, uint32_t capacity)
{
    std::vector<uint16_t>& perCluster = m_clusterCounts;
    perCluster.assign(kClusterCount, 0);
    for (uint32_t i = 0; i < count; ++i) {
        const ClusterRange& R = m_ranges[i];
        for (int z = R.z0; z <= R.z1; ++z)
            for (int y = R.y0; y <= R.y1; ++y)
                for (int x = R.x0; x <= R.x1; ++x) ++perCluster[size_t((z * kClusterY + y) * kClusterX + x)];
    }
    // Offsets (prefix sum), then the lists.
    uint32_t total = 0;
    for (uint32_t c = 0; c < kClusterCount; ++c) {
        clusters[c] = total << shift | perCluster[c];
        total += perCluster[c];
        perCluster[c] = 0;
    }
    if (total > capacity) return ~0u;
    // Packed little-endian: entry k of a word = its bits [k * width, (k + 1) * width).
    auto* bytes = reinterpret_cast<unsigned char*>(words);
    for (uint32_t i = 0; i < count; ++i) {
        const ClusterRange& R = m_ranges[i];
        for (int z = R.z0; z <= R.z1; ++z)
            for (int y = R.y0; y <= R.y1; ++y)
                for (int x = R.x0; x <= R.x1; ++x) {
                    const size_t c = size_t((z * kClusterY + y) * kClusterX + x);
                    const IndexT index = IndexT(i);
                    std::memcpy(bytes + size_t((clusters[c] >> shift) + perCluster[c]++) * sizeof(IndexT), &index, sizeof(IndexT));
                }
    }
    const uint32_t perWord = 4 / sizeof(IndexT);
    return (total + perWord - 1) / perWord;
}

// If the index list overflows, the clusters are flagged invalid and shaders
// fall back to looping over every light.
void LightClusterer::build(const CameraData& cam, LightBuffer& lb, uint32_t count, bool enabled)
{
    const glm::vec2 slicing = sliceParams(cam);
    lb.clusterParams = glm::vec4(slicing, enabled ? 1.0f : 0.0f, 0.0f);
    m_clusterWords = 0;
    if (!enabled) return;
    if (count == 0) {
        std::memset(lb.clusters, 0, sizeof(lb.clusters));
        return;
    }
    m_ranges.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        const glm::vec4 pr = lb.point[i].positionRange;
        const glm::vec3 vc = glm::vec3(cam.view * glm::vec4(glm::vec3(pr), 1.0f));
        m_ranges[i] = rangeOf(cam, slicing, vc - glm::vec3(pr.w), vc + glm::vec3(pr.w));
    }
    const uint32_t words = fill<uint8_t>(count, 9, lb.clusters, lb.clusterLights, kClusterIndexCapacity);
    if (words == ~0u) {
        lb.clusterParams.z = 0.0f; // too many: every pixel loops over all lights
        return;
    }
    m_clusterWords = words;
}

void LightClusterer::buildDecals(const CameraData& cam, DecalBuffer& db, const std::vector<glm::mat4>& worlds)
{
    const auto count = static_cast<uint32_t>(std::min<size_t>(worlds.size(), kMaxDecals));
    const glm::vec2 slicing = sliceParams(cam);
    db.info = glm::uvec4(count, 1u, 0u, 0u);
    db.clusterParams = glm::vec4(slicing, 0.0f, 0.0f);
    m_decalWords = 0;
    if (count == 0) {
        std::memset(db.clusters, 0, sizeof(db.clusters));
        return;
    }
    m_ranges.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        // View-space bounds of the box's corners (tighter than a sphere for
        // the flat boxes decals usually are).
        const glm::mat4 m = cam.view * worlds[i];
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (int c = 0; c < 8; ++c) {
            const glm::vec3 v = glm::vec3(m * glm::vec4(c & 1 ? 0.5f : -0.5f, c & 2 ? 0.5f : -0.5f, c & 4 ? 0.5f : -0.5f, 1.0f));
            lo = glm::min(lo, v);
            hi = glm::max(hi, v);
        }
        m_ranges[i] = rangeOf(cam, slicing, lo, hi);
    }
    const uint32_t words = fill<uint16_t>(count, 16, db.clusters, db.clusterDecals, kDecalIndexCapacity);
    if (words == ~0u) {
        db.info.y = 0u; // too many: every pixel tests every decal
        return;
    }
    m_decalWords = words;
}

} // namespace ve
