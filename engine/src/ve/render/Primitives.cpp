#include "ve/render/Primitives.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <map>

namespace ve::primitives {

MeshData cube(const glm::vec3& size)
{
    MeshData m;
    const glm::vec3 h = size * 0.5f;
    // Each face: normal, and two axes spanning it (u, v) such that u x v = normal (CCW).
    struct Face {
        glm::vec3 n, u, v;
    };
    const Face faces[6] = {
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},  {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},  {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},   {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
    };
    for (const Face& f : faces) {
        const auto base = static_cast<uint32_t>(m.vertices.size());
        const glm::vec2 uvs[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
        const glm::vec2 corners[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (int i = 0; i < 4; ++i) {
            Vertex v;
            v.position = (f.n + f.u * corners[i].x + f.v * corners[i].y) * h;
            v.normal = f.n;
            v.uv = uvs[i];
            v.tangent = glm::vec4(f.u, 1.0f);
            m.vertices.push_back(v);
        }
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    return m;
}

MeshData plane(float sizeX, float sizeZ, uint32_t segX, uint32_t segZ)
{
    MeshData m;
    for (uint32_t z = 0; z <= segZ; ++z) {
        for (uint32_t x = 0; x <= segX; ++x) {
            Vertex v;
            const float fx = static_cast<float>(x) / static_cast<float>(segX);
            const float fz = static_cast<float>(z) / static_cast<float>(segZ);
            v.position = {(fx - 0.5f) * sizeX, 0.0f, (fz - 0.5f) * sizeZ};
            v.normal = {0, 1, 0};
            v.uv = {fx, fz};
            v.tangent = {1, 0, 0, 1};
            m.vertices.push_back(v);
        }
    }
    const uint32_t row = segX + 1;
    for (uint32_t z = 0; z < segZ; ++z) {
        for (uint32_t x = 0; x < segX; ++x) {
            const uint32_t i = z * row + x;
            // CCW when viewed from above (+Y).
            m.indices.insert(m.indices.end(), {i, i + row, i + 1, i + 1, i + row, i + row + 1});
        }
    }
    return m;
}

MeshData sphere(float radius, uint32_t segments, uint32_t rings)
{
    MeshData m;
    for (uint32_t r = 0; r <= rings; ++r) {
        const float v = static_cast<float>(r) / static_cast<float>(rings);
        const float phi = v * glm::pi<float>();
        for (uint32_t s = 0; s <= segments; ++s) {
            const float u = static_cast<float>(s) / static_cast<float>(segments);
            const float theta = u * glm::two_pi<float>();
            Vertex vert;
            vert.normal = {std::sin(phi) * std::cos(theta), std::cos(phi), -std::sin(phi) * std::sin(theta)};
            vert.position = vert.normal * radius;
            vert.uv = {u, v};
            m.vertices.push_back(vert);
        }
    }
    const uint32_t row = segments + 1;
    for (uint32_t r = 0; r < rings; ++r) {
        for (uint32_t s = 0; s < segments; ++s) {
            const uint32_t i = r * row + s;
            m.indices.insert(m.indices.end(), {i, i + row, i + 1, i + 1, i + row, i + row + 1});
        }
    }
    m.computeTangents();
    return m;
}

MeshData icosphere(float radius, uint32_t subdivisions)
{
    const float t = (1.0f + std::sqrt(5.0f)) / 2.0f;
    std::vector<glm::vec3> pos = {
        {-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
        {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1},
    };
    std::vector<uint32_t> idx = {0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4,
                                 11, 10, 2, 10, 7, 6, 7, 1, 8, 3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8,
                                 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1};
    for (auto& p : pos) p = glm::normalize(p);

    for (uint32_t s = 0; s < subdivisions; ++s) {
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> cache;
        auto midpoint = [&](uint32_t a, uint32_t b) {
            const auto key = std::minmax(a, b);
            if (auto it = cache.find(key); it != cache.end()) return it->second;
            pos.push_back(glm::normalize((pos[a] + pos[b]) * 0.5f));
            const auto i = static_cast<uint32_t>(pos.size() - 1);
            cache[key] = i;
            return i;
        };
        std::vector<uint32_t> next;
        for (size_t i = 0; i < idx.size(); i += 3) {
            const uint32_t a = idx[i], b = idx[i + 1], c = idx[i + 2];
            const uint32_t ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
            next.insert(next.end(), {a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca});
        }
        idx = std::move(next);
    }

    MeshData m;
    for (const auto& p : pos) {
        Vertex v;
        v.position = p * radius;
        v.normal = p;
        v.uv = {0.5f + std::atan2(p.z, p.x) / glm::two_pi<float>(), 0.5f - std::asin(p.y) / glm::pi<float>()};
        m.vertices.push_back(v);
    }
    m.indices = std::move(idx);
    return m;
}

MeshData cylinder(float rBottom, float rTop, float height, uint32_t segments, bool caps)
{
    MeshData m;
    const float slope = (rBottom - rTop) / height; // for cone normals
    for (uint32_t s = 0; s <= segments; ++s) {
        const float u = static_cast<float>(s) / static_cast<float>(segments);
        const float a = u * glm::two_pi<float>();
        const glm::vec3 dir(std::cos(a), 0.0f, -std::sin(a));
        const glm::vec3 n = glm::normalize(glm::vec3(dir.x, slope, dir.z));
        Vertex bottom, top;
        bottom.position = dir * rBottom;
        top.position = dir * rTop + glm::vec3(0, height, 0);
        bottom.normal = top.normal = n;
        bottom.uv = {u, 1.0f};
        top.uv = {u, 0.0f};
        m.vertices.push_back(bottom);
        m.vertices.push_back(top);
    }
    for (uint32_t s = 0; s < segments; ++s) {
        const uint32_t i = s * 2;
        m.indices.insert(m.indices.end(), {i, i + 2, i + 1, i + 1, i + 2, i + 3});
    }
    if (caps) {
        auto cap = [&](float y, float r, bool up) {
            if (r <= 0.0f) return;
            const auto center = static_cast<uint32_t>(m.vertices.size());
            Vertex c;
            c.position = {0, y, 0};
            c.normal = {0, up ? 1.0f : -1.0f, 0};
            c.uv = {0.5f, 0.5f};
            m.vertices.push_back(c);
            for (uint32_t s = 0; s <= segments; ++s) {
                const float a = static_cast<float>(s) / static_cast<float>(segments) * glm::two_pi<float>();
                Vertex v = c;
                v.position = {std::cos(a) * r, y, -std::sin(a) * r};
                v.uv = {0.5f + std::cos(a) * 0.5f, 0.5f + std::sin(a) * 0.5f};
                m.vertices.push_back(v);
            }
            for (uint32_t s = 0; s < segments; ++s) {
                const uint32_t a = center + 1 + s, b = a + 1;
                if (up) m.indices.insert(m.indices.end(), {center, a, b});
                else m.indices.insert(m.indices.end(), {center, b, a});
            }
        };
        cap(0.0f, rBottom, false);
        cap(height, rTop, true);
    }
    m.computeTangents();
    return m;
}

MeshData quad(float width, float height)
{
    MeshData m;
    const float w = width * 0.5f;
    const glm::vec3 p[4] = {{-w, 0, 0}, {w, 0, 0}, {w, height, 0}, {-w, height, 0}};
    const glm::vec2 uv[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    for (int i = 0; i < 4; ++i) {
        Vertex v;
        v.position = p[i];
        v.normal = {0, 0, 1};
        v.uv = uv[i];
        m.vertices.push_back(v);
    }
    m.indices = {0, 1, 2, 0, 2, 3};
    return m;
}

} // namespace ve::primitives
