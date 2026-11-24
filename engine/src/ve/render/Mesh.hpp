#pragma once

// =============================================================================
// Mesh - GPU geometry: one vertex buffer + one index buffer (device-local),
// split into Submeshes that can each use a different material.
//
// MeshData is the CPU-side builder: fill vertices/indices (or use the
// primitive generators in Primitives.hpp), then upload with Mesh::create().
// =============================================================================

#include "ve/gfx/Resources.hpp"
#include "ve/render/Geometry.hpp"

#include <memory>
#include <string>
#include <vector>

namespace ve {

class Renderer;

struct Submesh {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    uint32_t materialSlot = 0; ///< which entry of MeshRenderer::materials to use
    AABB bounds;               ///< local-space bounds of just this submesh
};

/// Options for MeshData::simplified() (LOD generation, via meshoptimizer).
struct SimplifyOptions {
    float ratio = 0.5f;     ///< target fraction of triangles to keep
    float maxError = 0.05f; ///< max geometric deviation, relative to the mesh size (0.01 = 1%)
    /// "Sloppy" mode ignores topology: much more aggressive, good for far LODs
    /// of organic shapes (foliage blobs), bad for anything that must stay closed.
    bool sloppy = false;
    /// Faceted/low-poly meshes (unshared vertices per triangle): weld by
    /// position + colour only, then re-flatten normals so the LOD stays faceted.
    /// For smooth meshes leave false: normals and UVs are kept.
    bool faceted = false;
};

/// CPU-side mesh data with a few useful operations for procedural geometry.
struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    /// Appends another mesh, transforming positions/normals and optionally tinting it.
    void append(const MeshData& other, const glm::mat4& transform = glm::mat4(1.0f),
                const glm::vec4& tint = glm::vec4(1.0f));
    /// Smooth normals from triangle data (area weighted).
    void computeNormals();
    /// Unshares vertices so every triangle has its own (faceted, low-poly look),
    /// then recomputes flat normals.
    void makeFlatShaded();
    /// Tangents for normal mapping (needs normals + uvs).
    void computeTangents();
    void setColor(const glm::vec4& color);
    AABB bounds() const;
    uint32_t triangleCount() const { return static_cast<uint32_t>(indices.size() / 3); }

    /// A simplified copy (one LOD level). All triangles are treated as one
    /// submesh, so use it per material part if your mesh has several.
    MeshData simplified(const SimplifyOptions& options) const;
    /// Reorders triangles/vertices for the GPU's vertex cache and memory
    /// fetches (same mesh, renders faster). Cheap; do it for static meshes.
    void optimize();
};

class Mesh {
public:
    /// Uploads `data` to the GPU. If `submeshes` is empty a single submesh
    /// covering all indices is created.
    static std::shared_ptr<Mesh> create(Renderer& renderer, const MeshData& data,
                                        std::vector<Submesh> submeshes = {}, std::string name = {});
    /// A mesh whose vertices live in someone else's buffer (`vertexCount`
    /// vertices from byte `vertexOffset`; e.g. GPU-skinned output). Owns its
    /// indices only. Bounds come from `data` and can be updated (setBounds).
    static std::shared_ptr<Mesh> createShared(Renderer& renderer, VkBuffer vertices, VkDeviceSize vertexOffset,
                                              const MeshData& data, std::vector<Submesh> submeshes = {}, std::string name = {});
    ~Mesh();

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

    VkBuffer vertexBuffer() const { return m_sharedVertices ? m_sharedVertices : m_vertices.handle; }
    VkDeviceSize vertexOffset() const { return m_vertexOffset; } ///< bind the vertex buffer at this byte offset
    /// Binds the vertex buffer (at vertexOffset()) to binding 0 and the 32-bit index buffer.
    void bind(VkCommandBuffer cmd) const;
    /// Moving geometry (skinning): every submesh gets these bounds.
    void setBounds(const AABB& bounds);
    VkBuffer indexBuffer() const { return m_indices.handle; }
    const std::vector<Submesh>& submeshes() const { return m_submeshes; }
    const AABB& bounds() const { return m_bounds; }
    uint32_t vertexCount() const { return m_vertexCount; }
    uint32_t indexCount() const { return m_indexCount; }
    const std::string& name() const { return m_name; }

private:
    Mesh() = default;
    /// Stores the submeshes (one covering everything if empty) with bounds from `data`.
    void setSubmeshes(const MeshData& data, std::vector<Submesh> submeshes);

    Renderer* m_renderer = nullptr;
    Buffer m_vertices;
    VkBuffer m_sharedVertices = VK_NULL_HANDLE; ///< createShared: not owned
    VkDeviceSize m_vertexOffset = 0;
    Buffer m_indices;
    std::vector<Submesh> m_submeshes;
    AABB m_bounds;
    uint32_t m_vertexCount = 0;
    uint32_t m_indexCount = 0;
    std::string m_name;
};

} // namespace ve
