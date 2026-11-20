// =============================================================================
// Collider: Jolt shapes (box, sphere, capsule, cylinder, convex hull, mesh,
// heightfield, offset) and their triangles.
// (Jolt-backed: see JoltInternal.hpp for the module's layout and its rule.)
// =============================================================================

#include "ve/physics/JoltInternal.hpp"

#include "ve/render/Mesh.hpp"

#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/TransformedShape.h>

#include <glm/gtx/matrix_decompose.hpp>

#include <stdexcept>

namespace ve {

using namespace physics_detail;

namespace {

Collider makeCollider(const JPH::ShapeSettings::ShapeResult& result, const char* what)
{
    if (result.HasError()) {
        throw std::runtime_error(std::string("Collider::") + what + ": " + result.GetError().c_str());
    }
    auto impl = std::make_shared<Collider::Impl>();
    impl->shape = result.Get();
    Collider c;
    c.impl = impl;
    return c;
}

void requireJolt(const char* what)
{
    if (!g_joltInitialized) {
        throw std::runtime_error(std::string("Collider::") + what + " called before the Engine (physics) was created");
    }
}

} // namespace

namespace physics_detail {

JPH::RefConst<JPH::Shape> scaledShape(const Collider& collider, const glm::vec3& scale)
{
    JPH::RefConst<JPH::Shape> shape = collider.impl->shape;
    if (glm::any(glm::greaterThan(glm::abs(scale - 1.0f), glm::vec3(1e-4f)))) {
        shape = new JPH::ScaledShape(shape, shape->MakeScaleValid(toJ(scale)));
    }
    return shape;
}

} // namespace physics_detail

Collider Collider::box(const glm::vec3& halfExtents)
{
    requireJolt("box");
    // Convex radius rounds the corners slightly (faster, more stable collisions)
    // and must be smaller than the half extents.
    const float radius = std::min(JPH::cDefaultConvexRadius, glm::min(halfExtents.x, glm::min(halfExtents.y, halfExtents.z)) * 0.5f);
    return makeCollider(JPH::BoxShapeSettings(toJ(halfExtents), radius).Create(), "box");
}

Collider Collider::sphere(float radius)
{
    requireJolt("sphere");
    return makeCollider(JPH::SphereShapeSettings(radius).Create(), "sphere");
}

Collider Collider::capsule(float halfHeight, float radius)
{
    requireJolt("capsule");
    return makeCollider(JPH::CapsuleShapeSettings(halfHeight, radius).Create(), "capsule");
}

Collider Collider::cylinder(float halfHeight, float radius)
{
    requireJolt("cylinder");
    const float convex = std::min(JPH::cDefaultConvexRadius, std::min(halfHeight, radius) * 0.5f);
    return makeCollider(JPH::CylinderShapeSettings(halfHeight, radius, convex).Create(), "cylinder");
}

Collider Collider::convexHull(const std::vector<glm::vec3>& points)
{
    requireJolt("convexHull");
    JPH::Array<JPH::Vec3> pts;
    pts.reserve(points.size());
    for (const auto& p : points) pts.push_back(toJ(p));
    return makeCollider(JPH::ConvexHullShapeSettings(pts).Create(), "convexHull");
}

Collider Collider::convexHull(const MeshData& mesh)
{
    std::vector<glm::vec3> pts;
    pts.reserve(mesh.vertices.size());
    for (const auto& v : mesh.vertices) pts.push_back(v.position);
    return convexHull(pts);
}

Collider Collider::mesh(const MeshData& mesh)
{
    requireJolt("mesh");
    JPH::VertexList verts;
    verts.reserve(mesh.vertices.size());
    for (const auto& v : mesh.vertices) verts.push_back(JPH::Float3(v.position.x, v.position.y, v.position.z));
    JPH::IndexedTriangleList tris;
    tris.reserve(mesh.indices.size() / 3);
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        tris.push_back(JPH::IndexedTriangle(mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2]));
    }
    return makeCollider(JPH::MeshShapeSettings(verts, tris).Create(), "mesh");
}

Collider Collider::heightfield(const std::vector<float>& heights, uint32_t n, float cellSize, const glm::vec3& origin)
{
    requireJolt("heightfield");
    if (heights.size() < size_t(n) * n || n < 4) throw std::runtime_error("Collider::heightfield: not enough samples");
    // Jolt wants a sample count that's a multiple of its block size (2);
    // pad by repeating the last row/column if needed.
    const uint32_t padded = (n % 2 == 0) ? n : n + 1;
    std::vector<float> samples(size_t(padded) * padded);
    for (uint32_t z = 0; z < padded; ++z) {
        for (uint32_t x = 0; x < padded; ++x) {
            samples[size_t(z) * padded + x] = heights[size_t(std::min(z, n - 1)) * n + std::min(x, n - 1)];
        }
    }
    JPH::HeightFieldShapeSettings settings(samples.data(), toJ(origin), JPH::Vec3(cellSize, 1.0f, cellSize), padded);
    return makeCollider(settings.Create(), "heightfield");
}

Collider Collider::offset(const glm::vec3& translation, const glm::quat& rotation) const
{
    if (!impl) return *this;
    return makeCollider(JPH::RotatedTranslatedShapeSettings(toJ(translation), toJ(rotation), impl->shape).Create(), "offset");
}

std::vector<glm::vec3> Collider::triangles(const glm::mat4& transform) const
{
    std::vector<glm::vec3> out;
    if (!impl) return out;
    glm::vec3 scale{1.0f}, translation{0.0f}, skew{0.0f};
    glm::vec4 perspective{0.0f, 0.0f, 0.0f, 1.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    // A degenerate transform (zero scale) has no triangles to give.
    if (!glm::decompose(transform, scale, rotation, translation, skew, perspective)) return out;
    const JPH::Shape& shape = *impl->shape;
    const JPH::Quat rot = toJ(rotation);
    const JPH::Vec3 com = toJ(translation) + rot * (toJ(scale) * shape.GetCenterOfMass());
    // Offset / compound shapes: triangulate their leaf shapes.
    JPH::AllHitCollisionCollector<JPH::TransformedShapeCollector> leaves;
    shape.CollectTransformedShapes(JPH::AABox::sBiggest(), com, rot, toJ(scale), JPH::SubShapeIDCreator(), leaves, {});
    constexpr int kBatch = JPH::Shape::cGetTrianglesMinTrianglesRequested * 4;
    JPH::Float3 verts[kBatch * 3];
    for (const JPH::TransformedShape& ts : leaves.mHits) {
        JPH::Shape::GetTrianglesContext ctx;
        ts.GetTrianglesStart(ctx, JPH::AABox::sBiggest(), JPH::RVec3::sZero());
        for (;;) {
            const int n = ts.GetTrianglesNext(ctx, kBatch, verts);
            if (n <= 0) break;
            for (int i = 0; i < n * 3; ++i) out.emplace_back(verts[i].x, verts[i].y, verts[i].z);
        }
    }
    return out;
}

} // namespace ve
