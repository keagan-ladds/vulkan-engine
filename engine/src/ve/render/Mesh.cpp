#include "ve/render/Mesh.hpp"

#include "ve/gfx/Device.hpp"
#include "ve/render/Renderer.hpp"

#include <glm/gtc/matrix_inverse.hpp>

#include <meshoptimizer.h>

#include <algorithm>

namespace ve {

void MeshData::append(const MeshData& other, const glm::mat4& transform, const glm::vec4& tint)
{
    const auto base = static_cast<uint32_t>(vertices.size());
    const glm::mat3 normalMatrix = glm::inverseTranspose(glm::mat3(transform));
    for (Vertex v : other.vertices) {
        v.position = glm::vec3(transform * glm::vec4(v.position, 1.0f));
        v.normal = glm::normalize(normalMatrix * v.normal);
        v.tangent = glm::vec4(glm::normalize(glm::mat3(transform) * glm::vec3(v.tangent)), v.tangent.w);
        v.color *= tint;
        vertices.push_back(v);
    }
    for (uint32_t i : other.indices) indices.push_back(base + i);
}

void MeshData::computeNormals()
{
    for (auto& v : vertices) v.normal = glm::vec3(0.0f);
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        Vertex& a = vertices[indices[i]];
        Vertex& b = vertices[indices[i + 1]];
        Vertex& c = vertices[indices[i + 2]];
        // Cross product length = 2 * triangle area, so larger faces weigh more.
        const glm::vec3 n = glm::cross(b.position - a.position, c.position - a.position);
        a.normal += n;
        b.normal += n;
        c.normal += n;
    }
    for (auto& v : vertices) {
        const float len = glm::length(v.normal);
        v.normal = len > 1e-12f ? v.normal / len : glm::vec3(0, 1, 0);
    }
}

void MeshData::makeFlatShaded()
{
    std::vector<Vertex> flat;
    flat.reserve(indices.size());
    for (uint32_t i : indices) flat.push_back(vertices[i]);
    vertices = std::move(flat);
    for (uint32_t i = 0; i < indices.size(); ++i) indices[i] = i;
    for (size_t i = 0; i + 2 < vertices.size(); i += 3) {
        const glm::vec3 n = glm::cross(vertices[i + 1].position - vertices[i].position,
                                       vertices[i + 2].position - vertices[i].position);
        const float len = glm::length(n);
        const glm::vec3 nn = len > 1e-12f ? n / len : glm::vec3(0, 1, 0);
        vertices[i].normal = vertices[i + 1].normal = vertices[i + 2].normal = nn;
    }
}

void MeshData::computeTangents()
{
    std::vector<glm::vec3> tan(vertices.size(), glm::vec3(0.0f));
    std::vector<glm::vec3> bitan(vertices.size(), glm::vec3(0.0f));
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t i0 = indices[i], i1 = indices[i + 1], i2 = indices[i + 2];
        const glm::vec3 e1 = vertices[i1].position - vertices[i0].position;
        const glm::vec3 e2 = vertices[i2].position - vertices[i0].position;
        const glm::vec2 d1 = vertices[i1].uv - vertices[i0].uv;
        const glm::vec2 d2 = vertices[i2].uv - vertices[i0].uv;
        const float det = d1.x * d2.y - d2.x * d1.y;
        if (std::abs(det) < 1e-12f) continue;
        const float r = 1.0f / det;
        const glm::vec3 t = (e1 * d2.y - e2 * d1.y) * r;
        const glm::vec3 b = (e2 * d1.x - e1 * d2.x) * r;
        for (uint32_t k : {i0, i1, i2}) {
            tan[k] += t;
            bitan[k] += b;
        }
    }
    for (size_t i = 0; i < vertices.size(); ++i) {
        const glm::vec3 n = vertices[i].normal;
        glm::vec3 t = tan[i] - n * glm::dot(n, tan[i]); // Gram-Schmidt orthogonalize
        if (glm::length(t) < 1e-6f) {
            t = std::abs(n.y) < 0.99f ? glm::cross(glm::vec3(0, 1, 0), n) : glm::vec3(1, 0, 0);
        }
        t = glm::normalize(t);
        const float w = glm::dot(glm::cross(n, t), bitan[i]) < 0.0f ? -1.0f : 1.0f;
        vertices[i].tangent = glm::vec4(t, w);
    }
}

void MeshData::setColor(const glm::vec4& color)
{
    for (auto& v : vertices) v.color = color;
}

AABB MeshData::bounds() const
{
    AABB b;
    for (const auto& v : vertices) b.expand(v.position);
    return b;
}

MeshData MeshData::simplified(const SimplifyOptions& o) const
{
    if (indices.empty()) return *this;

    // 1. Weld duplicate vertices so the simplifier can collapse edges. For
    //    faceted meshes only position + colour must match (normals differ per face).
    std::vector<meshopt_Stream> streams;
    streams.push_back({&vertices[0].position, sizeof(glm::vec3), sizeof(Vertex)});
    streams.push_back({&vertices[0].color, sizeof(glm::vec4), sizeof(Vertex)});
    if (!o.faceted) {
        streams.push_back({&vertices[0].normal, sizeof(glm::vec3), sizeof(Vertex)});
        streams.push_back({&vertices[0].uv, sizeof(glm::vec2), sizeof(Vertex)});
    }
    std::vector<unsigned int> remap(vertices.size());
    const size_t unique = meshopt_generateVertexRemapMulti(remap.data(), indices.data(), indices.size(),
                                                           vertices.size(), streams.data(), streams.size());
    MeshData welded;
    welded.vertices.resize(unique);
    welded.indices.resize(indices.size());
    meshopt_remapVertexBuffer(welded.vertices.data(), vertices.data(), vertices.size(), sizeof(Vertex), remap.data());
    meshopt_remapIndexBuffer(welded.indices.data(), indices.data(), indices.size(), remap.data());

    // 2. Simplify towards the target triangle count, within the error budget.
    const size_t target = std::max<size_t>(3, size_t(float(indices.size() / 3) * o.ratio) * 3);
    std::vector<unsigned int> lod(welded.indices.size());
    float error = 0.0f;
    const float* positions = &welded.vertices[0].position.x;
    const size_t count = o.sloppy
        ? meshopt_simplifySloppy(lod.data(), welded.indices.data(), welded.indices.size(), positions, unique,
                                 sizeof(Vertex), target, o.maxError, &error)
        : meshopt_simplify(lod.data(), welded.indices.data(), welded.indices.size(), positions, unique, sizeof(Vertex),
                           target, o.maxError, 0, &error);
    lod.resize(count);

    // 3. Drop unused vertices (and order the rest for fetch locality).
    MeshData out;
    out.indices.assign(lod.begin(), lod.end());
    out.vertices.resize(unique);
    const size_t used = meshopt_optimizeVertexFetch(out.vertices.data(), out.indices.data(), out.indices.size(),
                                                    welded.vertices.data(), unique, sizeof(Vertex));
    out.vertices.resize(used);
    if (o.faceted) out.makeFlatShaded();
    return out;
}

void MeshData::optimize()
{
    if (indices.empty()) return;
    meshopt_optimizeVertexCache(indices.data(), indices.data(), indices.size(), vertices.size());
    std::vector<Vertex> reordered(vertices.size());
    const size_t used = meshopt_optimizeVertexFetch(reordered.data(), indices.data(), indices.size(), vertices.data(),
                                                    vertices.size(), sizeof(Vertex));
    reordered.resize(used);
    vertices = std::move(reordered);
}

std::shared_ptr<Mesh> Mesh::create(Renderer& renderer, const MeshData& data,
                                   std::vector<Submesh> submeshes, std::string name)
{
    if (data.vertices.empty() || data.indices.empty()) {
        throw std::runtime_error("Mesh::create called with empty data: " + name);
    }
    Device& device = renderer.device();

    std::shared_ptr<Mesh> mesh(new Mesh());
    mesh->m_renderer = &renderer;
    mesh->m_name = std::move(name);
    mesh->m_vertexCount = static_cast<uint32_t>(data.vertices.size());
    mesh->m_indexCount = static_cast<uint32_t>(data.indices.size());

    const VkDeviceSize vbSize = data.vertices.size() * sizeof(Vertex);
    const VkDeviceSize ibSize = data.indices.size() * sizeof(uint32_t);
    mesh->m_vertices = device.createBuffer(vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                           MemoryUsage::GpuOnly, "mesh vertices");
    mesh->m_indices = device.createBuffer(ibSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                          MemoryUsage::GpuOnly, "mesh indices");
    device.uploadBuffer(mesh->m_vertices, data.vertices.data(), vbSize);
    device.uploadBuffer(mesh->m_indices, data.indices.data(), ibSize);

    if (submeshes.empty()) {
        submeshes.push_back({0, mesh->m_indexCount, 0, {}});
    }
    for (auto& sm : submeshes) {
        sm.bounds = AABB{};
        for (uint32_t i = sm.firstIndex; i < sm.firstIndex + sm.indexCount; ++i) {
            sm.bounds.expand(data.vertices[data.indices[i]].position);
        }
        mesh->m_bounds.expand(sm.bounds);
    }
    mesh->m_submeshes = std::move(submeshes);
    return mesh;
}

Mesh::~Mesh()
{
    // The GPU may still be drawing this mesh: destroy the buffers later.
    Device* device = &m_renderer->device();
    Buffer vb = m_vertices, ib = m_indices;
    device->deferDestroy([device, vb, ib]() mutable {
        device->destroyBuffer(vb);
        device->destroyBuffer(ib);
    });
}

} // namespace ve
