#pragma once

// =============================================================================
// LightClusterer - bins this frame's point lights and decals into the camera's
// clusters (part of Renderer).
//
// Fills LightBuffer::clusterParams / clusters / clusterLights so each pixel
// only evaluates the lights that reach it (see ve.glsl), and the same for
// DecalBuffer (one grid for both; decals: see render/Decals.hpp). Done on the
// CPU: there are at most a few hundred lights and a thousand decals, already
// frustum culled. The scratch buffers are kept between frames, so binning
// doesn't allocate.
// =============================================================================

#include "ve/render/FrameData.hpp"

#include <cstdint>
#include <vector>

namespace ve {

class LightClusterer {
public:
    // Bins lights.point[0, pointLights). `enabled` = RendererSettings::clusteredLights
    // (off: the clusters are flagged invalid and shaders loop over every light).
    void build(const CameraData& camera, LightBuffer& lights, uint32_t pointLights, bool enabled);
    // Bins decals.decals[0, worlds.size()); worlds[i] = decal i's box (unit cube -> world).
    void buildDecals(const CameraData& camera, DecalBuffer& decals, const std::vector<glm::mat4>& worlds);
    // Used words of LightBuffer::clusterLights / DecalBuffer::clusterDecals (only that much is uploaded).
    uint32_t usedWords() const { return m_clusterWords; }
    uint32_t usedDecalWords() const { return m_decalWords; }

private:
    struct ClusterRange {
        int x0, x1, y0, y1, z0, z1; // z1 < z0: in no cluster
    };
    // Depth slicing of the camera's clusters: slice = log(view depth) * x + y.
    static glm::vec2 sliceParams(const CameraData& cam);
    // The clusters a view-space box (lo..hi) touches.
    static ClusterRange rangeOf(const CameraData& cam, const glm::vec2& slicing, const glm::vec3& lo, const glm::vec3& hi);
    // Writes `clusters` (first << shift | count) and the index lists (IndexT
    // per entry, little-endian packed into `words`) from m_ranges[0, count).
    // Returns the words used, or ~0u if more than `capacity` indices are needed.
    template <class IndexT>
    uint32_t fill(uint32_t count, uint32_t shift, uint32_t* clusters, uint32_t* words, uint32_t capacity);

    uint32_t m_clusterWords = 0;
    uint32_t m_decalWords = 0;
    std::vector<ClusterRange> m_ranges;    // per light / decal: its cluster box
    std::vector<uint16_t> m_clusterCounts; // per cluster: lights / decals in it
};

} // namespace ve
