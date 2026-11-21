// =============================================================================
// Physics debug drawing: body wireframes (PhysicsWorld::drawDebug) and joints
// (Joint::drawDebug), as ve::debug lines. This is the physics module's only
// file that includes ve/debug (an allowed .cpp-level exception in
// tools/check_layering.py: debug::drawEngineChannels in turn calls these).
// (Jolt-backed: see JoltInternal.hpp for the module's layout and its rule.)
// =============================================================================

#include "ve/physics/JoltInternal.hpp"

#include "ve/debug/Debug.hpp"
#include "ve/scene/Scene.hpp"

#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseQuery.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/TransformedShape.h>

namespace ve {

using namespace physics_detail;

namespace {
// Triangles of a shape in its centre-of-mass space, cached in `cache`.
const std::vector<glm::vec3>& shapeTriangles(std::unordered_map<const JPH::Shape*, PhysicsWorld::Impl::ShapeTriangles>& cache,
                                             const JPH::Shape* shape)
{
    if (auto it = cache.find(shape); it != cache.end()) return it->second.tris;
    if (cache.size() > 4096) cache.clear();
    PhysicsWorld::Impl::ShapeTriangles& st = cache[shape];
    st.keep = shape;
    JPH::AllHitCollisionCollector<JPH::TransformedShapeCollector> leaves;
    shape->CollectTransformedShapes(JPH::AABox::sBiggest(), JPH::Vec3::sZero(), JPH::Quat::sIdentity(), JPH::Vec3::sReplicate(1.0f),
                                    JPH::SubShapeIDCreator(), leaves, {});
    constexpr int kBatch = JPH::Shape::cGetTrianglesMinTrianglesRequested * 4;
    JPH::Float3 verts[kBatch * 3];
    for (const JPH::TransformedShape& ts : leaves.mHits) {
        JPH::Shape::GetTrianglesContext ctx;
        ts.GetTrianglesStart(ctx, JPH::AABox::sBiggest(), JPH::RVec3::sZero());
        for (;;) {
            const int n = ts.GetTrianglesNext(ctx, kBatch, verts);
            if (n <= 0) break;
            for (int i = 0; i < n * 3; ++i) st.tris.emplace_back(verts[i].x, verts[i].y, verts[i].z);
        }
    }
    return st.tris;
}
} // namespace

void PhysicsWorld::drawDebug(const glm::vec3& center, float radius, bool showBodies, bool showTerrain, bool showCharacters, const glm::vec3& eye) const
{
    if (!m_impl->system) return;
    const JPH::AABox box(toJ(center) - JPH::Vec3::sReplicate(radius), toJ(center) + JPH::Vec3::sReplicate(radius));
    JPH::AllHitCollisionCollector<JPH::CollideShapeBodyCollector> found;
    m_impl->system->GetBroadPhaseQuery().CollideAABox(box, found);
    const JPH::BodyLockInterfaceLocking& locks = m_impl->system->GetBodyLockInterface();
    const float r2 = radius * radius;
    debug::Style style;
    style.thickness = 1.5f;
    for (const JPH::BodyID id : found.mHits) {
        JPH::BodyLockRead lock(locks, id);
        if (!lock.Succeeded()) continue;
        const JPH::Body& body = lock.GetBody();
        const JPH::Shape* shape = body.GetShape();
        const bool heightfield = shape->GetSubType() == JPH::EShapeSubType::HeightField;
        const bool character = (body.GetUserData() & 3u) == 2u;
        if (heightfield ? !showTerrain : character ? !showCharacters : !showBodies) continue;
        // Not the one we're inside (the camera's own character): its lines would cover the screen.
        if (!heightfield && body.GetWorldSpaceBounds().Contains(toJ(eye))) continue;
        glm::vec4 color = debug::kGrey;
        if (character) color = debug::kYellow;
        else if (body.IsSensor()) color = debug::kGreen;
        else if (body.IsKinematic()) color = debug::kBlue;
        else if (body.IsDynamic()) color = body.IsActive() ? debug::kOrange : glm::vec4(0.6f, 0.4f, 0.15f, 1.0f);
        if (heightfield) color = glm::vec4(0.55f, 0.75f, 0.45f, 0.6f);
        const JPH::RMat44 m = body.GetCenterOfMassTransform();
        const auto& tris = shapeTriangles(m_impl->debugTriangles, shape);
        for (size_t i = 0; i + 2 < tris.size(); i += 3) {
            glm::vec3 p[3];
            for (int k = 0; k < 3; ++k) {
                const JPH::RVec3 w = m * JPH::Vec3(tris[i + k].x, tris[i + k].y, tris[i + k].z);
                p[k] = glm::vec3(float(w.GetX()), float(w.GetY()), float(w.GetZ()));
            }
            if (heightfield) { // big: only near the centre
                const glm::vec3 c = (p[0] + p[1] + p[2]) / 3.0f - center;
                if (glm::dot(c, c) > r2) continue;
            }
            debug::line(p[0], p[1], color, style);
            debug::line(p[1], p[2], color, style);
            debug::line(p[2], p[0], color, style);
        }
    }
}

void Joint::drawDebug() const
{
    const glm::vec3 p = glm::vec3(transform().worldMatrix() * glm::vec4(anchor, 1.0f));
    const glm::vec4 color = m_broken ? debug::kRed : attached() ? debug::kMagenta : debug::kGrey;
    debug::cross(p, 0.25f, color);
    if (m_type == Type::Hinge || m_type == Type::Slider) {
        const glm::vec3 ax = glm::normalize(transform().rotation() * axis);
        debug::arrow(p - ax * 0.4f, p + ax * 0.4f, color);
    }
    if (GameObject* o = connectedObject()) {
        const glm::vec3 q = m_type == Type::Distance ? glm::vec3(o->transform().worldMatrix() * glm::vec4(connectedAnchor, 1.0f))
                                                     : o->transform().position();
        debug::line(p, q, color);
    } else if (m_type == Type::Distance) {
        debug::line(p, connectedAnchor, color);
    }
}

} // namespace ve
