// =============================================================================
// PhysicsWorld queries: raycasts, shape casts (sphere / box / any collider)
// and overlaps, filtered by QueryFilter.
// (Jolt-backed: see JoltInternal.hpp for the module's layout and its rule.)
// =============================================================================

#include "ve/physics/JoltInternal.hpp"

#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>

namespace ve {

using namespace physics_detail;

namespace {

// Query filter by layer (QueryFilter's statics / dynamics / characters / triggers).
class QueryLayerFilter final : public JPH::ObjectLayerFilter {
public:
    explicit QueryLayerFilter(const QueryFilter& f) : m_f(f) {}
    bool ShouldCollide(JPH::ObjectLayer layer) const override
    {
        if (!((m_f.layers >> gameLayerOf(layer)) & 1u)) return false;
        switch (kindOf(layer)) {
        case Layers::NonMoving: return m_f.statics;
        case Layers::Moving: return m_f.dynamics;
        case Layers::Character: return m_f.characters;
        case Layers::Sensor: return m_f.triggers;
        default: return false;
        }
    }

private:
    const QueryFilter& m_f;
};

// Filters a query's bodies: the ignored body / character.
class QueryBodyFilter final : public JPH::BodyFilter {
public:
    explicit QueryBodyFilter(const QueryFilter& f, JPH::BodyID character) : m_f(f), m_character(character) {}
    bool ShouldCollide(const JPH::BodyID& id) const override
    {
        if (m_f.ignore && m_f.ignore->created() && id == JPH::BodyID(m_f.ignore->bodyId())) return false;
        return m_character.IsInvalid() || id != m_character;
    }

private:
    const QueryFilter& m_f;
    JPH::BodyID m_character;
};

JPH::BodyID characterBody(const QueryFilter& f)
{
    return f.ignoreCharacter ? JPH::BodyID(f.ignoreCharacter->innerBodyId()) : JPH::BodyID();
}

// Fills body / gameObject / userValue (and the normal, when asked) from a body.
void fillHit(const JPH::PhysicsSystem& system, JPH::BodyID id, const JPH::SubShapeID& subShape, RaycastHit& hit,
             bool surfaceNormal)
{
    JPH::BodyLockRead lock(system.GetBodyLockInterface(), id);
    if (!lock.Succeeded()) return;
    const JPH::Body& body = lock.GetBody();
    if (surfaceNormal) hit.normal = toG(body.GetWorldSpaceSurfaceNormal(subShape, toJ(hit.point)));
    const BodyOwner o = ownerOf(body.GetUserData());
    hit.body = o.body;
    hit.gameObject = o.gameObject;
    hit.userValue = o.userValue;
    hit.layer = gameLayerOf(body.GetObjectLayer());
    hit.surface = surfaceOf(body);
}

} // namespace

std::optional<RaycastHit> PhysicsWorld::raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                                                const Rigidbody* ignore, bool hitCharacters) const
{
    QueryFilter f;
    f.ignore = ignore;
    f.characters = hitCharacters;
    return raycast(origin, direction, maxDistance, f);
}

std::optional<RaycastHit> PhysicsWorld::raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                                                const QueryFilter& filter) const
{
    if (!m_impl->system || maxDistance <= 0.0f || glm::dot(direction, direction) < 1e-12f) return std::nullopt;
    const glm::vec3 dir = glm::normalize(direction);
    const JPH::RRayCast ray{toJ(origin), toJ(dir * maxDistance)};
    JPH::RayCastResult hit;
    QueryLayerFilter layerFilter(filter);
    QueryBodyFilter bodyFilter(filter, characterBody(filter));
    if (!m_impl->system->GetNarrowPhaseQuery().CastRay(ray, hit, {}, layerFilter, bodyFilter)) return std::nullopt;

    RaycastHit result;
    result.distance = hit.mFraction * maxDistance;
    result.point = origin + dir * result.distance;
    fillHit(*m_impl->system, hit.mBodyID, hit.mSubShapeID2, result, true);
    return result;
}

std::optional<RaycastHit> PhysicsWorld::sphereCast(const glm::vec3& origin, float radius, const glm::vec3& direction,
                                                   float maxDistance, const QueryFilter& filter) const
{
    return shapeCast(Collider::sphere(std::max(radius, 1e-3f)), origin, glm::quat(1, 0, 0, 0), direction, maxDistance, filter);
}

std::optional<RaycastHit> PhysicsWorld::boxCast(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation,
                                                const glm::vec3& direction, float maxDistance, const QueryFilter& filter) const
{
    return shapeCast(Collider::box(glm::max(halfExtents, glm::vec3(1e-3f))), center, rotation, direction, maxDistance, filter);
}

std::optional<RaycastHit> PhysicsWorld::shapeCast(const Collider& shape, const glm::vec3& position, const glm::quat& rotation,
                                                  const glm::vec3& direction, float maxDistance, const QueryFilter& filter) const
{
    if (!m_impl->system || !shape.valid() || maxDistance < 0.0f || glm::dot(direction, direction) < 1e-12f) return std::nullopt;
    const glm::vec3 dir = glm::normalize(direction);
    const JPH::RMat44 start = JPH::RMat44::sRotationTranslation(toJ(rotation), toJ(position));
    const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(shape.impl->shape, JPH::Vec3::sReplicate(1.0f), start,
                                                                      toJ(dir * maxDistance));
    JPH::ShapeCastSettings settings;
    settings.mReturnDeepestPoint = true; // starts inside: report the way out
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::IgnoreBackFaces;
    settings.mBackFaceModeConvex = JPH::EBackFaceMode::CollideWithBackFaces;
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    QueryLayerFilter layerFilter(filter);
    QueryBodyFilter bodyFilter(filter, characterBody(filter));
    m_impl->system->GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(), collector, {}, layerFilter, bodyFilter);
    if (!collector.HadHit()) return std::nullopt;

    const JPH::ShapeCastResult& h = collector.mHit;
    RaycastHit result;
    result.distance = h.mFraction * maxDistance;
    result.point = toG(h.mContactPointOn2);
    // The penetration axis points from the cast shape into what it hit.
    const glm::vec3 axis = toG(h.mPenetrationAxis);
    result.normal = glm::dot(axis, axis) > 1e-12f ? -glm::normalize(axis) : -dir;
    fillHit(*m_impl->system, h.mBodyID2, h.mSubShapeID2, result, false);
    return result;
}

std::vector<RaycastHit> PhysicsWorld::overlapSphere(const glm::vec3& center, float radius, const QueryFilter& filter) const
{
    return overlap(Collider::sphere(std::max(radius, 1e-3f)), center, glm::quat(1, 0, 0, 0), filter);
}

std::vector<RaycastHit> PhysicsWorld::overlapBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation,
                                                 const QueryFilter& filter) const
{
    return overlap(Collider::box(glm::max(halfExtents, glm::vec3(1e-3f))), center, rotation, filter);
}

std::vector<RaycastHit> PhysicsWorld::overlap(const Collider& shape, const glm::vec3& position, const glm::quat& rotation,
                                              const QueryFilter& filter) const
{
    std::vector<RaycastHit> hits;
    if (!m_impl->system || !shape.valid()) return hits;
    const JPH::Shape& s = *shape.impl->shape;
    // CollideShape wants the centre-of-mass transform.
    const JPH::RMat44 com = JPH::RMat44::sRotationTranslation(toJ(rotation), toJ(position)).PreTranslated(s.GetCenterOfMass());
    JPH::CollideShapeSettings settings;
    settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    QueryLayerFilter layerFilter(filter);
    QueryBodyFilter bodyFilter(filter, characterBody(filter));
    m_impl->system->GetNarrowPhaseQuery().CollideShape(&s, JPH::Vec3::sReplicate(1.0f), com, settings, JPH::RVec3::sZero(),
                                                       collector, {}, layerFilter, bodyFilter);
    // One hit per body: the deepest contact.
    std::unordered_map<uint32_t, size_t> byBody;
    std::vector<float> depth;
    for (const JPH::CollideShapeResult& r : collector.mHits) {
        const uint32_t id = r.mBodyID2.GetIndexAndSequenceNumber();
        auto [it, inserted] = byBody.try_emplace(id, hits.size());
        if (!inserted && r.mPenetrationDepth <= depth[it->second]) continue;
        RaycastHit h;
        h.point = toG(r.mContactPointOn2);
        const glm::vec3 axis = toG(r.mPenetrationAxis);
        h.normal = glm::dot(axis, axis) > 1e-12f ? -glm::normalize(axis) : glm::vec3(0, 1, 0);
        fillHit(*m_impl->system, r.mBodyID2, r.mSubShapeID2, h, false);
        if (inserted) {
            hits.push_back(h);
            depth.push_back(r.mPenetrationDepth);
        } else {
            hits[it->second] = h;
            depth[it->second] = r.mPenetrationDepth;
        }
    }
    return hits;
}

} // namespace ve
