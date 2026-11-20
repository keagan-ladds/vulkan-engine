// =============================================================================
// Reflection (scene files, editor) for the physics module's components:
// registerPhysicsTypes, which the Engine calls at startup. No Jolt here.
// =============================================================================

#include "ve/physics/Physics.hpp"

#include "ve/scene/Reflection.hpp"

namespace ve {

namespace {
template <class T>
void jointFields(TypeBuilder<T>& t, bool axis, bool limits, bool motor, bool distance)
{
    t.category("Physics");
    t.field("connectedTo", &Joint::connectedTo).tooltip("the other body's object name (empty = the world)");
    t.field("anchor", &Joint::anchor).tooltip("the pivot, in this object's space");
    if (axis) t.field("axis", &Joint::axis).tooltip("in this object's space");
    if (limits) {
        t.field("useLimits", &Joint::useLimits);
        t.field("limitMin", &Joint::limitMin).tooltip("hinge: degrees, slider: metres (<= 0)");
        t.field("limitMax", &Joint::limitMax).tooltip("hinge: degrees, slider: metres (>= 0)");
        t.field("friction", &Joint::friction);
    }
    if (distance) {
        t.field("connectedAnchor", &Joint::connectedAnchor).tooltip("in the other's space (world point if none)");
        t.field("minDistance", &Joint::minDistance).tooltip("< 0: the distance when created (0 = a rope)");
        t.field("maxDistance", &Joint::maxDistance).tooltip("< 0: the distance when created");
    }
    if (limits || distance) {
        t.field("springFrequency", &Joint::springFrequency).range(0.0f, 30.0f).tooltip("Hz; 0 = rigid limits");
        t.field("springDamping", &Joint::springDamping).range(0.0f, 2.0f);
    }
    if (motor) {
        t.enumField("motor", &Joint::motor, {"Off", "Velocity", "Position"});
        t.field("motorTarget", &Joint::motorTarget).tooltip("velocity: deg/s or m/s; position: degrees or metres");
        t.field("motorMaxForce", &Joint::motorMaxForce);
        t.field("motorFrequency", &Joint::motorFrequency).range(0.1f, 30.0f);
    }
    t.field("breakForce", &Joint::breakForce).tooltip("N; 0 = never breaks");
    t.field("breakTorque", &Joint::breakTorque).tooltip("N m; 0 = never breaks");
}
} // namespace

void registerPhysicsTypes(TypeRegistry& r)
{
    r.add<FixedJoint>("FixedJoint", [](TypeBuilder<FixedJoint>& t) { jointFields(t, false, false, false, false); });
    r.add<HingeJoint>("HingeJoint", [](TypeBuilder<HingeJoint>& t) { jointFields(t, true, true, true, false); });
    r.add<SliderJoint>("SliderJoint", [](TypeBuilder<SliderJoint>& t) { jointFields(t, true, true, true, false); });
    r.add<DistanceJoint>("DistanceJoint", [](TypeBuilder<DistanceJoint>& t) { jointFields(t, false, false, false, true); });
    r.add<Rigidbody>("Rigidbody", [](TypeBuilder<Rigidbody>& t) {
        t.category("Physics");
        t.enumField("type", &Rigidbody::type, {"Static", "Kinematic", "Dynamic"});
        t.enumField("shape", &Rigidbody::shape, {"Custom (code)", "Box", "Sphere", "Capsule", "Cylinder"});
        t.field("shapeSize", &Rigidbody::shapeSize).range(0.01f, 1000.0f).tooltip("Box: full size. Sphere: x = diameter. Capsule/Cylinder: x = diameter, y = height");
        t.field("shapeOffset", &Rigidbody::shapeOffset);
        t.field("mass", &Rigidbody::mass).range(0.0f, 100000.0f).tooltip("kg; 0 = from volume");
        t.field("friction", &Rigidbody::friction).range(0.0f, 2.0f).speed(0.01f);
        t.field("restitution", &Rigidbody::restitution).range(0.0f, 1.0f).speed(0.01f);
        t.field("linearDamping", &Rigidbody::linearDamping).range(0.0f, 10.0f).speed(0.01f);
        t.field("angularDamping", &Rigidbody::angularDamping).range(0.0f, 10.0f).speed(0.01f);
        t.field("continuousCollision", &Rigidbody::continuousCollision);
        t.field("isTrigger", &Rigidbody::isTrigger).tooltip("Trigger volume: no collision, sends onTriggerEnter/Exit");
        t.field("layer", &Rigidbody::layer).tooltip("collision layer name (PhysicsWorld::addLayer); Default if empty");
        t.field("surface", &Rigidbody::surface).tooltip("physics material surface: Wood, Stone... (hits, contacts, footsteps)");
        t.onChanged([](Rigidbody& rb) { rb.rebuild(); });
    });
}

} // namespace ve
