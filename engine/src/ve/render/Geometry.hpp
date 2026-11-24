#pragma once

// =============================================================================
// Geometry - the engine's fixed vertex format plus bounding volumes.
// =============================================================================

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>

namespace ve {

/// The one vertex layout every mesh uses (64 bytes). Having a single format
/// means any material/shader can draw any mesh. Unused attributes cost a bit
/// of memory but keep everything simple.
struct Vertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv{0.0f};
    glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f}; ///< xyz = tangent, w = bitangent sign
    glm::vec4 color{1.0f};                     ///< vertex colour (great for low-poly art)

    static VkVertexInputBindingDescription binding()
    {
        return {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    }

    static std::array<VkVertexInputAttributeDescription, 5> attributes()
    {
        return {{
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
            {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
            {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, tangent)},
            {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)},
        }};
    }
};
static_assert(sizeof(Vertex) == 64);

/// Axis-aligned bounding box.
struct AABB {
    glm::vec3 min{FLT_MAX};
    glm::vec3 max{-FLT_MAX};

    bool valid() const { return min.x <= max.x; }
    glm::vec3 center() const { return (min + max) * 0.5f; }
    glm::vec3 extents() const { return (max - min) * 0.5f; }

    void expand(const glm::vec3& p)
    {
        min = glm::min(min, p);
        max = glm::max(max, p);
    }
    void expand(const AABB& b)
    {
        if (!b.valid()) return;
        expand(b.min);
        expand(b.max);
    }

    /// Bounds of this box after an affine transform (Arvo's method: project
    /// the extents through the absolute rotation/scale matrix).
    AABB transformed(const glm::mat4& m) const
    {
        const glm::vec3 c = glm::vec3(m * glm::vec4(center(), 1.0f));
        const glm::vec3 e = extents();
        glm::vec3 r;
        for (int i = 0; i < 3; ++i) {
            r[i] = std::abs(m[0][i]) * e.x + std::abs(m[1][i]) * e.y + std::abs(m[2][i]) * e.z;
        }
        return {c - r, c + r};
    }
};

/// Six planes (ax + by + cz + d >= 0 is inside) extracted from a view-projection matrix.
struct Frustum {
    std::array<glm::vec4, 6> planes{};

    static Frustum fromMatrix(const glm::mat4& m)
    {
        // Gribb-Hartmann extraction. glm is column-major: row i = (m[0][i], m[1][i], m[2][i], m[3][i]).
        auto row = [&](int i) { return glm::vec4(m[0][i], m[1][i], m[2][i], m[3][i]); };
        Frustum f;
        f.planes[0] = row(3) + row(0); // left
        f.planes[1] = row(3) - row(0); // right
        f.planes[2] = row(3) + row(1); // bottom/top (Y flip makes the names irrelevant)
        f.planes[3] = row(3) - row(1);
        f.planes[4] = row(2);          // z >= 0   (Vulkan depth range [0,1])
        f.planes[5] = row(3) - row(2); // z <= w
        for (auto& p : f.planes) p /= glm::length(glm::vec3(p));
        return f;
    }

    bool intersects(const AABB& box) const
    {
        const glm::vec3 c = box.center();
        const glm::vec3 e = box.extents();
        for (const auto& p : planes) {
            const float r = e.x * std::abs(p.x) + e.y * std::abs(p.y) + e.z * std::abs(p.z);
            if (glm::dot(glm::vec3(p), c) + p.w < -r) return false;
        }
        return true;
    }

    bool intersectsSphere(const glm::vec3& center, float radius) const
    {
        for (const auto& p : planes) {
            if (glm::dot(glm::vec3(p), center) + p.w < -radius) return false;
        }
        return true;
    }
};

} // namespace ve
