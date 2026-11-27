#pragma once

// =============================================================================
// SkinningSystem - GPU skinning for every SkinnedMeshRenderer (engine
// internal; see ve/anim/Animation.hpp for the components, which register
// themselves through addGatherer).
//
// Shared buffers, like InstanceSystem's (set 0, bindings 10-13 in ve.glsl):
//   rest     rest-pose vertices of every skinned mesh source (Vertex layout)
//   weights  per rest vertex: 4 joint indices + 4 weights
//   matrices this frame's joint matrices (one range per renderer, per frame slot)
//   output   skinned vertices, one range per renderer; also a vertex buffer
// Each frame the Renderer collects the active renderers' joint matrices, and
// SkinningPass (before everything else) runs skin.comp once per renderer.
// The output ranges are drawn like any mesh (Mesh::createShared), so every
// pipeline (colour, shadows, picking, motion vectors) works unchanged.
// =============================================================================

#include "ve/gfx/Resources.hpp"
#include "ve/render/Geometry.hpp"
#include "ve/render/Mesh.hpp"
#include "ve/render/PipelineHandle.hpp"

#include <glm/glm.hpp>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ve {

class Renderer;
class Scene;
class MeshRenderer;

/// A skinned mesh as loaded (shared by every instance of the model).
struct SkinnedMeshSource {
    MeshData data;                   ///< rest pose
    std::vector<Submesh> submeshes;  ///< Index ranges and material slots.
    std::vector<glm::uvec4> joints;  ///< per vertex, indices into the skin's joints
    std::vector<glm::vec4> weights;  ///< per vertex, sum 1
    std::vector<float> jointRadius;  ///< per joint: distance of its farthest weighted vertex (bounds)
    std::string name;                ///< The mesh's name (debugging).
};

/// GPU skinning (a compute pass): rest-pose vertices + joint matrices ->
/// deformed vertices that ordinary pipelines draw. Owned by the Renderer
/// (Renderer::skinning()); SkinnedMeshRenderer uses it.
class SkinningSystem {
public:
    /// One skinned mesh's place in the shared buffers.
    struct Instance {
        uint32_t restBase = 0;    ///< First vertex in the rest-pose buffer.
        uint32_t outputBase = 0;  ///< First vertex in the output buffer.
        uint32_t count = 0;       ///< Vertex count.
        std::shared_ptr<Mesh> mesh; ///< draws the output range
        const SkinnedMeshSource* source = nullptr; ///< What it was created from.
    };

    /// Allocates the shared buffers (sizes fixed for the run).
    void init(Renderer& renderer, uint32_t maxVertices, uint32_t maxJointsPerFrame);
    /// Frees the buffers and pipeline.
    void shutdown();

    /// Places a skinned mesh's rest pose in the buffers and makes its output Mesh.
    Instance create(const std::shared_ptr<const SkinnedMeshSource>& source);
    /// Frees an instance's space (its output mesh too).
    void destroy(Instance& instance);

    /// What to skin comes from gatherers: render can't see the animation module
    /// (whose SkinnedMeshRenderer does the work), so anim adds one at startup
    /// (registerSkinnedMeshes, called by the Engine). Each frame Renderer::gatherScene
    /// runs them in order: each add()s the joint matrices of `scene`'s visible
    /// skinned renderers and lists them in `deformed`.
    using Gatherer = std::function<void(Scene& scene, SkinningSystem& system, std::vector<const MeshRenderer*>& deformed)>;
    void addGatherer(Gatherer gatherer) { m_gatherers.push_back(std::move(gatherer)); } ///< Registers a Gatherer.
    const std::vector<Gatherer>& gatherers() const { return m_gatherers; }                 ///< Every registered Gatherer.

    /// Per frame: collect what to skin (Renderer::gatherScene), then record (SkinningPass).
    void beginFrame(uint32_t slot);
    bool add(const Instance& instance, const glm::mat4* joints, uint32_t jointCount); ///< false = over budget
    void record(VkCommandBuffer cmd);                 ///< Records this frame's skinning dispatches.
    uint32_t skinnedThisFrame() const { return uint32_t(m_jobs.size()); } ///< Meshes skinned this frame.

    VkBuffer restBuffer() const { return m_rest.handle; }        ///< Rest-pose vertices.
    VkBuffer weightBuffer() const { return m_weights.handle; }   ///< Joint indices and weights.
    VkBuffer matrixBuffer(uint32_t slot) const { return m_matrices[slot].handle; } ///< A frame slot's joint matrices.
    VkBuffer outputBuffer() const { return m_output.handle; }    ///< Skinned vertices (drawn by the output meshes).

private:
    struct Range {
        uint32_t base, count;
    };
    static bool allocate(std::vector<Range>& free, uint32_t count, uint32_t& base);
    static void release(std::vector<Range>& free, uint32_t base, uint32_t count);
    struct Job {
        uint32_t restBase, outputBase, count, matrixBase;
    };
    struct SourceEntry {
        uint32_t base, count, users;
    };

    Renderer* m_renderer = nullptr;
    Buffer m_rest, m_weights, m_output;
    std::vector<Buffer> m_matrices; ///< per frame slot, host visible
    uint32_t m_maxVertices = 0, m_maxJoints = 0;
    std::vector<Range> m_freeRest, m_freeOutput;
    std::map<const SkinnedMeshSource*, SourceEntry> m_sources;
    std::vector<std::shared_ptr<const SkinnedMeshSource>> m_keepAlive;
    std::vector<Job> m_jobs;
    std::vector<Gatherer> m_gatherers;
    uint32_t m_slot = 0, m_matrixCount = 0;
    PipelineHandle m_pipeline;
};

} // namespace ve
