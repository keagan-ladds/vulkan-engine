#pragma once

// =============================================================================
// Spline - a smooth curve through control points: roads, paths, fences,
// powerlines, rivers, patrol routes, camera rails.
//
//   auto& s = obj.addComponent<ve::Spline>();
//   s.setPoints({{{0, 0, 0}}, {{20, 0, 5}}, {{40, 0, 0}}});   // local to the object
//   const auto path = s.path();                  // world space, cached
//   float len = path->length();
//   auto mid = path->at(len * 0.5f);             // position, tangent, width at a distance
//   auto c = path->closest(somePoint, 10.0f);    // nearest point within 10 m (horizontal)
//
// The curve passes through every point (centripetal Catmull-Rom: no loops or
// overshoot however unevenly the points are spaced). Points are in the
// object's local space, so moving the object moves the whole spline. Each
// point also has a width factor that systems built on splines use (a road
// narrowing, a fence's clearing). The world-space path is sampled every
// ~0.5 m into a polyline with distances along it and a grid for fast
// closest-point queries; it is immutable and shared (terrain snapshots and
// worker threads hold it).
//
// Systems that follow a spline (SplineInstancer, TerrainSpline, ...) look for
// the Spline on their own object and rebuild when version() changes. The
// editor edits points in the viewport (docs/05-editor.md, "Splines").
// =============================================================================

#include "ve/scene/GameObject.hpp"

#include <nlohmann/json.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace ve {

using Json = nlohmann::json;

/// A control point of a Spline (in the object's local space).
struct SplinePoint {
    glm::vec3 position{0.0f}; ///< Where the curve passes (local space, metres).
    float width = 1.0f;       ///< Width factor here (1 = the user's nominal width), interpolated between points.
};

/// A spline sampled in world space: a polyline with distances along it.
/// Immutable once built (share it freely between threads).
class SplinePath {
public:
    /// A point on the path.
    struct Sample {
        glm::vec3 position{0.0f}; ///< World position.
        glm::vec3 tangent{0.0f, 0.0f, 1.0f}; ///< Unit direction of travel.
        float width = 1.0f;       ///< Interpolated width factor.
        float distance = 0.0f;    ///< Metres along the path from its start.
    };
    /// The point of the path nearest to a query point.
    struct Closest : Sample {
        float separation = 0.0f; ///< Distance from the query point to the path (m; horizontal for horizontal queries).
        float side = 0.0f;       ///< Signed horizontal offset: > 0 to the right of the direction of travel.
    };

    /// Samples the curve through `points` (local space, transformed by `toWorld`).
    /// @param step target distance between samples (m).
    static SplinePath build(const std::vector<SplinePoint>& points, bool closed, const glm::mat4& toWorld, float step = 0.5f);

    /// Whether the path has no length (fewer than two points).
    bool empty() const { return m_samples.size() < 2; }
    /// Total length (m).
    float length() const { return m_samples.empty() ? 0.0f : m_samples.back().distance; }
    /// Whether the end joins the start.
    bool closed() const { return m_closed; }
    /// The point `distance` metres along (clamped to the ends; wrapped when closed).
    Sample at(float distance) const;
    /// The nearest point of the path to `p`.
    /// @param maxSeparation only look this far (m); larger = slower. Returns
    ///        separation = +inf when nothing is that close.
    /// @param horizontal measure in the XZ plane only (terrain uses), else in 3D.
    Closest closest(const glm::vec3& p, float maxSeparation, bool horizontal = true) const;
    /// The sampled polyline (world space; closed paths repeat the first sample at the end).
    const std::vector<Sample>& samples() const { return m_samples; }
    /// Distance along the path of control point `i` (m; 0 for out-of-range indices).
    float pointDistance(size_t i) const { return i < m_pointDistances.size() ? m_pointDistances[i] : 0.0f; }
    /// Index of the control point that starts the segment containing `distance`.
    size_t segmentAt(float distance) const;
    /// World bounds of the samples.
    glm::vec3 boundsMin() const { return m_min; }
    /// World bounds of the samples.
    glm::vec3 boundsMax() const { return m_max; }

private:
    void buildGrid();
    std::vector<Sample> m_samples;
    std::vector<float> m_pointDistances;
    bool m_closed = false;
    glm::vec3 m_min{0.0f}, m_max{0.0f};
    float m_cell = 16.0f;                                   ///< grid cell size (m)
    std::unordered_map<uint64_t, std::vector<uint32_t>> m_grid; ///< XZ cell -> segments (index of their first sample)
};

/// A curve through control points on a GameObject (see the top of this file).
class Spline : public Component {
public:
    /// The control points (local space).
    const std::vector<SplinePoint>& points() const { return m_points; }
    /// Replaces every point.
    void setPoints(std::vector<SplinePoint> points);
    /// Moves / changes point `index` (ignored if out of range).
    void setPoint(size_t index, const SplinePoint& point);
    /// Inserts a point before `index` (index = size(): appends).
    void insertPoint(size_t index, const SplinePoint& point);
    /// Removes point `index`.
    void removePoint(size_t index);
    /// Whether the last point joins back to the first.
    bool closed() const { return m_closed; }
    /// Opens / closes the loop.
    void setClosed(bool closed);
    /// Reverses the direction (point order).
    void reverse();

    /// The world-space path (rebuilt when the points or the object's transform changed).
    std::shared_ptr<const SplinePath> path();
    /// Changes whenever the points, closed or the world transform change (poll it to rebuild what follows the spline).
    uint64_t version();

    /// Converts a world position to this object's local space (for placing points).
    glm::vec3 toLocal(const glm::vec3& world) const;
    /// Converts a local point position to world space.
    glm::vec3 toWorld(const glm::vec3& local) const;

    /// The points as JSON: [[x, y, z, width], ...] (scene files).
    Json pointsJson() const;
    /// Sets the points from pointsJson()'s format (also accepts [x, y, z]).
    void setPointsJson(const Json& json);

private:
    void changed() { ++m_version; }
    std::vector<SplinePoint> m_points;
    bool m_closed = false;
    uint64_t m_version = 1;
    uint64_t m_builtVersion = 0;
    glm::mat4 m_builtMatrix{0.0f};
    std::shared_ptr<const SplinePath> m_path;
};

} // namespace ve
