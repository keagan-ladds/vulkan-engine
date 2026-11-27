#include "ve/scene/Spline.hpp"

#include "ve/scene/Transform.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ve {

namespace {

// Centripetal Catmull-Rom (Barry-Goldman form): the curve between p1 and p2,
// with p0 / p3 as neighbours; knots spaced by sqrt(distance) so uneven point
// spacing never makes loops or cusps.
glm::vec3 catmullRom(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3, float u)
{
    auto knot = [](const glm::vec3& a, const glm::vec3& b) { return std::max(std::sqrt(glm::length(b - a)), 1e-4f); };
    const float t0 = 0.0f, t1 = t0 + knot(p0, p1), t2 = t1 + knot(p1, p2), t3 = t2 + knot(p2, p3);
    const float t = t1 + (t2 - t1) * u;
    const glm::vec3 a1 = (t1 - t) / (t1 - t0) * p0 + (t - t0) / (t1 - t0) * p1;
    const glm::vec3 a2 = (t2 - t) / (t2 - t1) * p1 + (t - t1) / (t2 - t1) * p2;
    const glm::vec3 a3 = (t3 - t) / (t3 - t2) * p2 + (t - t2) / (t3 - t2) * p3;
    const glm::vec3 b1 = (t2 - t) / (t2 - t0) * a1 + (t - t0) / (t2 - t0) * a2;
    const glm::vec3 b2 = (t3 - t) / (t3 - t1) * a2 + (t - t1) / (t3 - t1) * a3;
    return (t2 - t) / (t2 - t1) * b1 + (t - t1) / (t2 - t1) * b2;
}

uint64_t cellKey(int x, int z)
{
    return (uint64_t(uint32_t(x)) << 32) | uint64_t(uint32_t(z));
}

glm::vec3 rightOf(const glm::vec3& tangent)
{
    // Y up: right = tangent x up (forward -Z -> right +X).
    const glm::vec3 r(-tangent.z, 0.0f, tangent.x);
    const float l = glm::length(r);
    return l > 1e-6f ? r / l : glm::vec3(1.0f, 0.0f, 0.0f);
}

} // namespace

// --- SplinePath ------------------------------------------------------------------

SplinePath SplinePath::build(const std::vector<SplinePoint>& points, bool closed, const glm::mat4& toWorld, float step)
{
    SplinePath path;
    path.m_closed = closed && points.size() >= 3;
    const size_t n = points.size();
    if (n == 0) return path;
    std::vector<glm::vec3> w(n);
    for (size_t i = 0; i < n; ++i) w[i] = glm::vec3(toWorld * glm::vec4(points[i].position, 1.0f));
    step = std::max(step, 0.01f);

    auto addSample = [&](const glm::vec3& p, float width) {
        Sample s;
        s.position = p;
        s.width = width;
        if (!path.m_samples.empty()) {
            const float d = glm::length(p - path.m_samples.back().position);
            if (d < 1e-5f) return; // (duplicate points)
            s.distance = path.m_samples.back().distance + d;
        }
        path.m_samples.push_back(s);
    };

    if (n == 1) {
        addSample(w[0], points[0].width);
    } else {
        const size_t segments = path.m_closed ? n : n - 1;
        auto at = [&](long i) -> glm::vec3 {
            if (path.m_closed) return w[size_t((i % long(n) + long(n)) % long(n))];
            if (i < 0) return 2.0f * w[0] - w[1];                   // reflected end tangents
            if (i >= long(n)) return 2.0f * w[n - 1] - w[n - 2];
            return w[size_t(i)];
        };
        for (size_t s = 0; s < segments; ++s) {
            const long i = long(s);
            const glm::vec3 p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
            const float w1 = points[s].width, w2 = points[(s + 1) % n].width;
            path.m_pointDistances.push_back(path.m_samples.empty() ? 0.0f : path.m_samples.back().distance);
            // Enough samples for the chord (curves bulge: a little extra).
            const int count = std::max(2, int(std::ceil(glm::length(p2 - p1) * 1.15f / step)));
            for (int k = 0; k < count; ++k) {
                const float u = float(k) / float(count);
                addSample(catmullRom(p0, p1, p2, p3, u), w1 + (w2 - w1) * u);
            }
        }
        addSample(path.m_closed ? w[0] : w[n - 1], path.m_closed ? points[0].width : points[n - 1].width);
        if (!path.m_closed) path.m_pointDistances.push_back(path.m_samples.back().distance);
    }

    // Tangents from neighbours (closed paths wrap).
    const size_t m = path.m_samples.size();
    for (size_t i = 0; i < m; ++i) {
        size_t a = i > 0 ? i - 1 : (path.m_closed && m > 2 ? m - 2 : 0);
        size_t b = i + 1 < m ? i + 1 : (path.m_closed && m > 2 ? 1 : m - 1);
        glm::vec3 d = path.m_samples[b].position - path.m_samples[a].position;
        const float l = glm::length(d);
        path.m_samples[i].tangent = l > 1e-6f ? d / l : (i > 0 ? path.m_samples[i - 1].tangent : glm::vec3(0, 0, 1));
    }
    path.m_min = path.m_max = path.m_samples.front().position;
    path.m_maxWidth = 0.0f;
    for (const Sample& s : path.m_samples) {
        path.m_maxWidth = std::max(path.m_maxWidth, s.width);
        path.m_min = glm::min(path.m_min, s.position);
        path.m_max = glm::max(path.m_max, s.position);
    }
    path.buildGrid();
    return path;
}

void SplinePath::buildGrid()
{
    m_grid.clear();
    for (uint32_t i = 0; i + 1 < m_samples.size(); ++i) {
        const glm::vec3 a = m_samples[i].position, b = m_samples[i + 1].position;
        const int x0 = int(std::floor(std::min(a.x, b.x) / m_cell)), x1 = int(std::floor(std::max(a.x, b.x) / m_cell));
        const int z0 = int(std::floor(std::min(a.z, b.z) / m_cell)), z1 = int(std::floor(std::max(a.z, b.z) / m_cell));
        for (int x = x0; x <= x1; ++x)
            for (int z = z0; z <= z1; ++z) m_grid[cellKey(x, z)].push_back(i);
    }
}

size_t SplinePath::segmentAt(float distance) const
{
    if (m_pointDistances.empty()) return 0;
    auto it = std::upper_bound(m_pointDistances.begin(), m_pointDistances.end(), distance);
    const size_t i = it == m_pointDistances.begin() ? 0 : size_t(it - m_pointDistances.begin()) - 1;
    const size_t segments = m_closed ? m_pointDistances.size() : m_pointDistances.size() - 1;
    return std::min(i, segments > 0 ? segments - 1 : 0);
}

SplinePath::Sample SplinePath::at(float distance) const
{
    if (m_samples.empty()) return {};
    if (m_samples.size() == 1) return m_samples[0];
    const float len = length();
    if (m_closed && len > 0.0f) {
        distance = std::fmod(distance, len);
        if (distance < 0.0f) distance += len;
    } else {
        distance = std::clamp(distance, 0.0f, len);
    }
    auto it = std::upper_bound(m_samples.begin(), m_samples.end(), distance, [](float d, const Sample& s) { return d < s.distance; });
    const size_t i1 = std::min(size_t(it - m_samples.begin()), m_samples.size() - 1);
    const size_t i0 = i1 > 0 ? i1 - 1 : 0;
    const Sample &a = m_samples[i0], &b = m_samples[i1];
    const float span = b.distance - a.distance;
    const float u = span > 1e-6f ? (distance - a.distance) / span : 0.0f;
    Sample s;
    s.position = glm::mix(a.position, b.position, u);
    const glm::vec3 t = glm::mix(a.tangent, b.tangent, u);
    s.tangent = glm::length(t) > 1e-6f ? glm::normalize(t) : a.tangent;
    s.width = a.width + (b.width - a.width) * u;
    s.distance = distance;
    return s;
}

SplinePath::Closest SplinePath::closest(const glm::vec3& p, float maxSeparation, bool horizontal) const
{
    Closest best;
    best.separation = std::numeric_limits<float>::infinity();
    if (m_samples.empty()) return best;
    auto consider = [&](uint32_t i) {
        const Sample &a = m_samples[i], &b = m_samples[i + 1];
        glm::vec3 ab = b.position - a.position, ap = p - a.position;
        if (horizontal) {
            ab.y = 0.0f;
            ap.y = 0.0f;
        }
        const float ll = glm::dot(ab, ab);
        const float u = ll > 1e-12f ? std::clamp(glm::dot(ap, ab) / ll, 0.0f, 1.0f) : 0.0f;
        const glm::vec3 d = ap - ab * u;
        const float sep = glm::length(d);
        if (sep >= best.separation) return;
        best.separation = sep;
        best.position = glm::mix(a.position, b.position, u);
        best.tangent = glm::normalize(glm::mix(a.tangent, b.tangent, u) + glm::vec3(1e-7f));
        best.width = a.width + (b.width - a.width) * u;
        best.distance = a.distance + (b.distance - a.distance) * u;
        best.side = glm::dot(glm::vec3(p.x - best.position.x, 0.0f, p.z - best.position.z), rightOf(best.tangent));
    };
    if (m_samples.size() == 1) {
        const glm::vec3 d = p - m_samples[0].position;
        best.separation = horizontal ? glm::length(glm::vec2(d.x, d.z)) : glm::length(d);
        static_cast<Sample&>(best) = m_samples[0];
    } else if (!std::isfinite(maxSeparation) || maxSeparation > m_cell * 64.0f) {
        for (uint32_t i = 0; i + 1 < m_samples.size(); ++i) consider(i);
    } else {
        const int x0 = int(std::floor((p.x - maxSeparation) / m_cell)), x1 = int(std::floor((p.x + maxSeparation) / m_cell));
        const int z0 = int(std::floor((p.z - maxSeparation) / m_cell)), z1 = int(std::floor((p.z + maxSeparation) / m_cell));
        for (int x = x0; x <= x1; ++x)
            for (int z = z0; z <= z1; ++z)
                if (auto it = m_grid.find(cellKey(x, z)); it != m_grid.end())
                    for (uint32_t i : it->second) consider(i);
    }
    if (best.separation > maxSeparation) best.separation = std::numeric_limits<float>::infinity();
    return best;
}

// --- Spline ------------------------------------------------------------------------

void Spline::setPoints(std::vector<SplinePoint> points)
{
    m_points = std::move(points);
    changed();
}

void Spline::setPoint(size_t index, const SplinePoint& point)
{
    if (index >= m_points.size()) return;
    m_points[index] = point;
    changed();
}

void Spline::insertPoint(size_t index, const SplinePoint& point)
{
    m_points.insert(m_points.begin() + long(std::min(index, m_points.size())), point);
    changed();
}

void Spline::removePoint(size_t index)
{
    if (index >= m_points.size()) return;
    m_points.erase(m_points.begin() + long(index));
    changed();
}

void Spline::setClosed(bool closed)
{
    if (closed == m_closed) return;
    m_closed = closed;
    changed();
}

void Spline::reverse()
{
    std::reverse(m_points.begin(), m_points.end());
    changed();
}

uint64_t Spline::version()
{
    // A moved object moves its spline: count it as a change.
    if (m_builtMatrix != gameObject().transform().worldMatrix() && m_builtVersion == m_version) changed();
    return m_version;
}

std::shared_ptr<const SplinePath> Spline::path()
{
    const glm::mat4& world = gameObject().transform().worldMatrix();
    if (!m_path || m_builtVersion != m_version || m_builtMatrix != world) {
        if (m_builtMatrix != world && m_builtVersion == m_version) changed();
        m_path = std::make_shared<const SplinePath>(SplinePath::build(m_points, m_closed, world));
        m_builtVersion = m_version;
        m_builtMatrix = world;
    }
    return m_path;
}

glm::vec3 Spline::toLocal(const glm::vec3& world) const
{
    return glm::vec3(glm::inverse(gameObject().transform().worldMatrix()) * glm::vec4(world, 1.0f));
}

glm::vec3 Spline::toWorld(const glm::vec3& local) const
{
    return glm::vec3(gameObject().transform().worldMatrix() * glm::vec4(local, 1.0f));
}

Json Spline::pointsJson() const
{
    Json a = Json::array();
    for (const SplinePoint& p : m_points) a.push_back({p.position.x, p.position.y, p.position.z, p.width});
    return a;
}

void Spline::setPointsJson(const Json& json)
{
    std::vector<SplinePoint> pts;
    if (json.is_array()) {
        for (const Json& j : json) {
            if (!j.is_array() || j.size() < 3) continue;
            SplinePoint p;
            p.position = {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
            if (j.size() > 3) p.width = j[3].get<float>();
            pts.push_back(p);
        }
    }
    setPoints(std::move(pts));
}

} // namespace ve
