#pragma once

// =============================================================================
// Skeletal animation (F2).
//
//   auto model = assets().loadModel("models/villager.gltf"); // mesh, skin, clips
//   GameObject& v = scene.instantiate(*model);                // joints become child objects
//   auto* anim = v.getComponent<ve::Animator>();             // added for animated models
//   anim->play("Walk", 0.25f);                                // crossfade over 0.25 s
//
// The skeleton is the model's node objects: an Animator writes their local
// transforms, so anything parented to a joint (a spear in a hand) follows.
// A SkinnedMeshRenderer (a MeshRenderer) turns those joints into skin
// matrices every frame; the vertices are skinned on the GPU (SkinningSystem)
// and drawn by every regular pipeline (colour, shadows, picking).
// Clips: glTF translation / rotation / scale channels with step, linear or
// cubic-spline keys. Several clips can play at once with weights (crossfades).
// =============================================================================

#include "ve/render/Skinning.hpp"
#include "ve/scene/Components.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ve {

struct AnimationChannel {
    enum class Path { Translation, Rotation, Scale };
    enum class Interpolation { Step, Linear, CubicSpline };
    int node = -1; // the model node it animates
    Path path = Path::Translation;
    Interpolation interpolation = Interpolation::Linear;
    std::vector<float> times;
    // xyz (rotation: quaternion xyzw). Cubic spline: in-tangent, value, out-tangent per key.
    std::vector<glm::vec4> values;
    glm::vec4 sample(float time) const;
};

class AnimationClip {
public:
    std::string name;
    float duration = 0.0f;
    std::vector<AnimationChannel> channels;
};

// Plays clips on a model's node objects. Scene::instantiate adds one (with
// the model's clips) to the root of every animated model.
//
// Layers: layer 0 is the whole body. More layers play their own clips over
// it, on part of the skeleton (a joint and everything below it), replacing
// (Override) or adding to (Additive) what's under them, with a weight that
// fades:
//   int upper = anim->addLayer("Arms", "UpperArm.R");   // the right arm down
//   anim->play(upper, "Wave");  ...  anim->setLayerWeight(upper, 0.0f, 0.4f);
// Events: names at times in a clip, fired when its playback passes them
// (while the clip is the current one on its layer, not while fading out):
//   anim->addEvent("Walk", 0.25f, "step");  anim->onEvent = [](auto& e, auto&) { ... };
class Animator : public Component {
public:
    enum class LayerBlend { Override, Additive };

    std::string clip;     // played on start (empty = the first clip)
    float speed = 1.0f;   // playback rate of everything
    bool playOnStart = true;
    // (event, clip): an event a playing clip passed (addEvent).
    std::function<void(const std::string& event, const std::string& clip)> onEvent;

    void start() override;
    void update(float dt) override;

    // Crossfades to `name` over `fade` seconds (0 = cut) on layer 0. Unknown names are ignored (warned once).
    void play(const std::string& name, float fade = 0.2f, bool loop = true, float clipSpeed = 1.0f);
    void stop(float fade = 0.2f);
    const std::string& current() const; // the clip fading in / playing on layer 0 ("" = none)
    float time() const;                 // seconds into it
    bool finished() const;              // a non-looping current clip reached its end
    std::vector<std::string> clipNames() const;

    // --- Layers -------------------------------------------------------------------------
    // maskJoint: the joint (node name) it affects, with everything below it; "" = all.
    // Returns the layer's index (an existing one if the name is taken).
    int addLayer(const std::string& name, const std::string& maskJoint = "", LayerBlend blend = LayerBlend::Override);
    int layerIndex(const std::string& name) const; // -1 if none
    int layerCount() const { return int(m_layers.size()); }
    void play(int layer, const std::string& name, float fade = 0.2f, bool loop = true, float clipSpeed = 1.0f);
    void stop(int layer, float fade);
    void setLayerWeight(int layer, float weight, float fade = 0.2f);
    float layerWeight(int layer) const;
    const std::string& current(int layer) const;
    float time(int layer) const;
    bool finished(int layer) const;

    // --- Events ---------------------------------------------------------------------------
    void addEvent(const std::string& clip, float time, const std::string& name);
    void clearEvents() { m_events.clear(); }

    // Does any clip animate this node object? (IK resets the ones none does.)
    bool animates(const GameObject& node) const;

    // Scene::instantiate: the clips and the object of every model node (null = not instantiated).
    void bind(std::vector<std::shared_ptr<const AnimationClip>> clips, const std::vector<GameObject*>& nodes);

private:
    struct State {
        std::shared_ptr<const AnimationClip> clip;
        float time = 0.0f, speed = 1.0f, weight = 0.0f, fadeRate = 0.0f; // fadeRate: weight per second (+in / -out)
        bool loop = true;
    };
    struct Rest {
        glm::vec3 t;
        glm::quat r;
        glm::vec3 s;
    };
    struct Pose {
        glm::vec3 t{0.0f}, s{0.0f};
        glm::vec4 r{0.0f};
        float w = 0.0f;
    };
    struct Layer {
        std::string name;
        std::vector<char> mask; // per model node (empty = every node)
        LayerBlend blend = LayerBlend::Override;
        float weight = 1.0f, target = 1.0f, rate = 0.0f;
        std::vector<State> states; // the last one is the current clip
    };
    struct Event {
        std::string clip, name;
        float time;
    };
    void apply();
    // The weighted pose of a layer's clips (normalised), per animated node in `nodes`; returns the total weight.
    float samplePose(const Layer& layer, std::vector<Pose>& poses) const;
    std::shared_ptr<const AnimationClip> find(const std::string& name) const;

    std::vector<std::shared_ptr<const AnimationClip>> m_clips;
    std::vector<uint64_t> m_nodes;   // object ids per model node (0 = none)
    std::vector<std::string> m_nodeNames;
    std::vector<int> m_parents;      // per model node (-1 = root)
    std::vector<Rest> m_rest;        // per model node
    std::vector<int> m_animated;     // model nodes any clip animates
    std::vector<Layer> m_layers{Layer{"Base"}};
    std::vector<Event> m_events;
    std::vector<Pose> m_base, m_scratch; // per node, reused every frame
};

// Two-bone inverse kinematics (legs, arms) after the animation: bends the
// upper / middle joints so the end joint reaches `target`, with the middle
// joint (knee, elbow) bending towards `pole`. Put it on the model's root
// (next to the Animator); joints are found by name below it.
//   auto& ik = villager.addComponent<ve::TwoBoneIK>("UpperArm.R", "LowerArm.R", "Hand.R");
//   ik.target = door.handle(); ik.weight = 1.0f;
class TwoBoneIK : public Component {
public:
    TwoBoneIK() = default;
    TwoBoneIK(std::string upper, std::string middle, std::string end)
        : upperJoint(std::move(upper)), middleJoint(std::move(middle)), endJoint(std::move(end)) {}
    std::string upperJoint, middleJoint, endJoint;
    glm::vec3 target{0.0f};          // world position for the end joint
    glm::vec3 pole{0.0f, 0.0f, 1.0f}; // which way the middle joint bends, in the model's space (+Z = forward for glTF models)
    float weight = 1.0f;             // 0 = the animation as it is
    bool automatic = true;           // solve in lateUpdate (false: whoever drives it calls solve())

    void lateUpdate(float dt) override;
    // The end joint's position before IK this frame (what the animation wanted).
    glm::vec3 animatedEnd() const { return m_animatedEnd; }
    bool solve(); // (lateUpdate does it; call again after moving the target in a later lateUpdate)

private:
    bool resolve();
    uint64_t m_upper = 0, m_middle = 0, m_end = 0;
    glm::quat m_restUpper{1, 0, 0, 0}, m_restMiddle{1, 0, 0, 0}; // for joints no clip animates (no accumulation)
    bool m_resetUpper = false, m_resetMiddle = false;
    glm::vec3 m_animatedEnd{0.0f};
};

// Feet on uneven ground: raycasts under each foot, moves the foot targets of
// two TwoBoneIKs onto the ground and lowers the hips by as much as the lower
// foot needs. Put it next to the Animator with the legs' joint names.
class FootIK : public Component {
public:
    std::string hipsJoint = "Hips";
    std::string leftUpper = "UpperLeg.L", leftMiddle = "LowerLeg.L", leftFoot = "Foot.L";
    std::string rightUpper = "UpperLeg.R", rightMiddle = "LowerLeg.R", rightFoot = "Foot.R";
    float maxStep = 0.45f;      // furthest a foot reaches down / up
    float smoothing = 12.0f;    // 1/s
    float weight = 1.0f;
    uint32_t layers = 0xFFFFFFFFu; // what counts as ground (PhysicsWorld::layerMask)

    void start() override;
    void lateUpdate(float dt) override;
    float hipsOffset() const { return m_hips; }
    glm::vec3 footTarget(int side) const { return m_targets[side]; } // 0 left, 1 right

private:
    TwoBoneIK* m_legs[2] = {nullptr, nullptr};
    uint64_t m_hipsId = 0;
    float m_hips = 0.0f;
    float m_offset[2] = {0.0f, 0.0f};
    glm::vec3 m_targets[2]{};
};

// A mesh deformed by joints (GPU skinned). Set up by Scene::instantiate for
// glTF meshes with a skin.
class SkinnedMeshRenderer : public MeshRenderer {
public:
    void bind(std::shared_ptr<const SkinnedMeshSource> source, const std::vector<GameObject*>& joints, std::vector<glm::mat4> inverseBind);
    void onDestroy() override;
    ~SkinnedMeshRenderer() override;

    // Renderer, every frame: joint matrices (in this object's space) for the
    // skinning pass, and bounds that follow the joints.
    void prepareSkinning(SkinningSystem& system);
    size_t jointCount() const { return m_joints.size(); }

private:
    void release();
    std::shared_ptr<const SkinnedMeshSource> m_source;
    std::vector<uint64_t> m_joints; // object ids
    std::vector<glm::mat4> m_inverseBind;
    std::vector<glm::mat4> m_matrices; // scratch
    SkinningSystem::Instance m_instance;
    SkinningSystem* m_system = nullptr;
};

} // namespace ve
