#pragma once

// Scripted smoke test (SAVANNA_AUTOTEST=1): picks up a sunstone, throws rocks,
// streams the world, chops, picks, saves and loads, builds, picks up stones
// (a ProxySet: promotion, demotion, save / load), then walks the UI,
// logging what happened ("[autotest] ..."). Exercises gameplay code paths
// (deferred destroy, one-shot audio, weather transitions...) without input.

#include <ve/scene/GameObject.hpp>
#include <ve/scene/ObjectRef.hpp>
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

class Villager;

// What the autotest saved (checked after the reload).
struct AutoTestSaveExpect {
    int collected = 0, wood = 0, berries = 0, stones = 0;
    glm::vec3 player{0.0f};
    uint64_t tree = 0;
    std::vector<std::pair<std::string, glm::vec3>> pieces; // building test: name, position
    uint64_t pickupGone = 0, pickupKept = 0; // pickups test: the entry picked up, the one marked
    int pickupStones = 0;
    size_t pickupEntries = 0;
};

class AutoTest : public ve::GameObject {
public:
    static void startAt(int step) { if (s_resume == 0) s_resume = step; } // (a reload resumes where it was)
    explicit AutoTest(const ve::Terrain& terrain) : GameObject("AutoTest"), m_terrainRef(&terrain), m_step(s_resume) { s_resume = 0; }
    void update(float dt) override;

    // Loading a save rebuilds the level (and this test object): the new one resumes here.
    static inline int s_resume = 0;
    static inline AutoTestSaveExpect s_expect;
    // Building test helpers.
    void buildStep(int piece, const glm::vec3& aim, float yawDegrees);
    int foliageAt(const glm::vec3& p, float radius) const;
    int m_foliageOnSite = 0, m_foliageDuring = 0;
    glm::vec3 m_exclusionAt{0.0f};
    ve::ObjectRef<> m_exclusionObject;
    uint32_t m_frames = 0, m_stepFrame = 0; // frames: steps that need the preview to settle
    glm::vec3 m_site{0.0f};
    std::vector<ve::ObjectRef<>> m_built;
    bool m_buildShot = false;
    void uiSteps(const std::function<void(const std::string&)>& log);
    void physicsSteps(const std::function<void(const std::string&)>& log);
    void pickupSteps(const std::function<void(const std::string&)>& log);
    // Stands the player next to a point on the ground, looking at it.
    void faceGround(const glm::vec3& target, float distance);
    bool m_pickupLogged = false;
    ve::ObjectRef<> m_physGhost, m_physSolid, m_physPlatform, m_physProbe;
    bool m_physOnStone = false;
    ve::ObjectRef<Villager> m_animVillager;
    std::vector<ve::ObjectRef<>> m_jointObjects;
    ve::ObjectRef<> m_ropeEnd;
    glm::vec3 m_ropeTop{0.0f};
    float m_weldBroke = 0.0f;
    bool m_jointsLogged = false;
    glm::vec3 m_lookAt{0.0f}, m_pointAt{0.0f};
    int m_animSteps = 0;

private:
    glm::vec3 groundAt(glm::vec2 p) const;
    void shot(const char* name);
    std::set<std::string> m_shotTaken;
    // The level's terrain (if something deleted it, the test fails loudly).
    ve::ObjectRef<const ve::Terrain> m_terrainRef;
    const ve::Terrain& terrain() const;
    ve::FoliageInstanceId m_tree, m_bush; // interactive foliage checks
    std::vector<glm::vec3> m_crateStart;
    bool m_throwPending = false;
    uint64_t m_briefDecal = 0; // a spawned decal with a 0.3 s lifetime
    bool m_inTrigger = false;
    glm::vec3 m_triggerCenter{0.0f}, m_triggerFeet{0.0f};
    ve::ObjectRef<> m_destroyProbe; // (it destroys itself into something else)
    glm::vec3 m_padStart{0.0f};
    float m_followStart = 0.0f;
    ve::ObjectRef<Villager> m_follower;
    float m_t = 0.0f;
    int m_step;
};

} // namespace savanna
