#pragma once

// Scripted smoke test (SAVANNA_AUTOTEST=1): picks up a sunstone, throws rocks,
// streams the world, chops, picks, saves and loads, builds, then walks the UI,
// logging what happened ("[autotest] ..."). Exercises gameplay code paths
// (deferred destroy, one-shot audio, weather transitions...) without input.

#include <ve/scene/GameObject.hpp>
#include <ve/world/Foliage.hpp>

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ve {
class Terrain;
}

namespace savanna {

// What the autotest saved (checked after the reload).
struct AutoTestSaveExpect {
    int collected = 0, wood = 0, berries = 0, stones = 0;
    glm::vec3 player{0.0f};
    uint64_t tree = 0;
    std::vector<std::pair<std::string, glm::vec3>> pieces; // building test: name, position
};

class AutoTest : public ve::GameObject {
public:
    static void startAt(int step) { if (s_resume == 0) s_resume = step; } // (a reload resumes where it was)
    explicit AutoTest(const ve::Terrain& terrain) : GameObject("AutoTest"), m_terrain(&terrain), m_step(s_resume) { s_resume = 0; }
    void update(float dt) override;

    // Loading a save rebuilds the level (and this test object): the new one resumes here.
    static inline int s_resume = 0;
    static inline AutoTestSaveExpect s_expect;
    // Building test helpers.
    void buildStep(int piece, const glm::vec3& aim, float yawDegrees);
    int foliageAt(const glm::vec3& p, float radius) const;
    int m_foliageOnSite = 0, m_foliageDuring = 0;
    glm::vec3 m_exclusionAt{0.0f};
    uint64_t m_exclusionObject = 0;
    uint32_t m_frames = 0, m_stepFrame = 0; // frames: steps that need the preview to settle
    glm::vec3 m_site{0.0f};
    std::vector<uint64_t> m_built;
    bool m_buildShot = false;
    void uiSteps(const std::function<void(const std::string&)>& log);
    void physicsSteps(const std::function<void(const std::string&)>& log);
    uint64_t m_physGhost = 0, m_physSolid = 0, m_physPlatform = 0, m_physProbe = 0;
    bool m_physOnStone = false;
    uint64_t m_animVillager = 0;
    std::vector<uint64_t> m_jointObjects;
    uint64_t m_ropeEnd = 0;
    glm::vec3 m_ropeTop{0.0f};
    float m_weldBroke = 0.0f;
    bool m_jointsLogged = false;
    glm::vec3 m_lookAt{0.0f}, m_pointAt{0.0f};
    int m_animSteps = 0;

private:
    glm::vec3 groundAt(glm::vec2 p) const;
    void shot(const char* name);
    std::set<std::string> m_shotTaken;
    const ve::Terrain* m_terrain;
    ve::FoliageInstanceId m_tree, m_bush; // interactive foliage checks
    std::vector<glm::vec3> m_crateStart;
    bool m_throwPending = false;
    bool m_inTrigger = false;
    glm::vec3 m_triggerCenter{0.0f}, m_triggerFeet{0.0f};
    uint64_t m_destroyProbe = 0; // (an id: it destroys itself into something else)
    glm::vec3 m_padStart{0.0f};
    float m_followStart = 0.0f;
    uint64_t m_follower = 0;
    float m_t = 0.0f;
    int m_step;
};

} // namespace savanna
