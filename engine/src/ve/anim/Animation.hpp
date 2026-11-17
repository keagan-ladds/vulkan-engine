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
class Animator : public Component {
public:
    std::string clip;     // played on start (empty = the first clip)
    float speed = 1.0f;   // playback rate of everything
    bool playOnStart = true;

    void start() override;
    void update(float dt) override;

    // Crossfades to `name` over `fade` seconds (0 = cut). Unknown names are ignored (warned once).
    void play(const std::string& name, float fade = 0.2f, bool loop = true, float clipSpeed = 1.0f);
    void stop(float fade = 0.2f);
    const std::string& current() const; // the clip fading in / playing ("" = none)
    float time() const;                 // seconds into it
    bool finished() const;              // a non-looping current clip reached its end
    std::vector<std::string> clipNames() const;

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
    void apply();

    std::vector<std::shared_ptr<const AnimationClip>> m_clips;
    std::vector<uint64_t> m_nodes;   // object ids per model node (0 = none)
    std::vector<Rest> m_rest;        // per model node
    std::vector<int> m_animated;     // model nodes any clip animates
    std::vector<State> m_states;     // the last one is the current clip
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
