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
    uint32_t materialSlot = 0; // which entry of MeshRenderer::materials to use
    AABB bounds;               // local-space bounds of just this submesh
};

// CPU-side mesh data with a few useful operations for procedural geometry.
struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    // Appends another mesh, transforming positions/normals and optionally tinting it.
    void append(const MeshData& other, const glm::mat4& transform = glm::mat4(1.0f),
                const glm::vec4& tint = glm::vec4(1.0f));
    // Smooth normals from triangle data (area weighted).
    void computeNormals();
    // Unshares vertices so every triangle has its own (faceted, low-poly look),
    // then recomputes flat normals.
    void makeFlatShaded();
    // Tangents for normal mapping (needs normals + uvs).
    void computeTangents();
    void setColor(const glm::vec4& color);
    AABB bounds() const;
};

class Mesh {
public:
    // Uploads `data` to the GPU. If `submeshes` is empty a single submesh
    // covering all indices is created.
    static std::shared_ptr<Mesh> create(Renderer& renderer, const MeshData& data,
                                        std::vector<Submesh> submeshes = {}, std::string name = {});
    ~Mesh();

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

    VkBuffer vertexBuffer() const { return m_vertices.handle; }
    VkBuffer indexBuffer() const { return m_indices.handle; }
    const std::vector<Submesh>& submeshes() const { return m_submeshes; }
    const AABB& bounds() const { return m_bounds; }
    uint32_t vertexCount() const { return m_vertexCount; }
    uint32_t indexCount() const { return m_indexCount; }
    const std::string& name() const { return m_name; }

private:
    Mesh() = default;

    Renderer* m_renderer = nullptr;
    Buffer m_vertices;
    Buffer m_indices;
    std::vector<Submesh> m_submeshes;
    AABB m_bounds;
    uint32_t m_vertexCount = 0;
    uint32_t m_indexCount = 0;
    std::string m_name;
};

} // namespace ve
