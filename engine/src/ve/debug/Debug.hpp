#pragma once
// =============================================================================
// Debug drawing: lines, shapes and labels in the 3D world, from anywhere.
//
//   ve::debug::line(a, b, ve::debug::kRed);
//   ve::debug::sphere(target, 0.5f, ve::debug::kYellow, {.duration = 2.0f});
//   ve::debug::path(agentCorners, ve::debug::kCyan);
//   ve::debug::text(head + up, "Wander", ve::debug::kWhite);
//
// Drawn on top of the finished image (after tonemapping and TAA, under the
// UI): crisp lines of any thickness, depth-tested against the scene, with the
// hidden parts faint. duration 0 = this frame only (call it every frame);
// > 0 = that many seconds. Not in photos.
//
// Channels group what engine systems (and games) can draw, switched on in
// the F1 overlay's "Debug draw" section or from code:
//
//   if (ve::debug::enabled("AI targets")) ve::debug::sphere(...);   // registers it (off)
//   ve::debug::setChannel("Physics colliders", true);
//
// Engine channels: "Physics colliders", "Physics terrain", "Character
// controllers", "Navmesh", "Nav agents" (paths, destinations).
// =============================================================================

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ve::debug {

// Colours are sRGB (like the UI's).
inline constexpr glm::vec4 kWhite{1.0f, 1.0f, 1.0f, 1.0f};
inline constexpr glm::vec4 kRed{1.0f, 0.25f, 0.2f, 1.0f};
inline constexpr glm::vec4 kGreen{0.3f, 1.0f, 0.35f, 1.0f};
inline constexpr glm::vec4 kBlue{0.3f, 0.55f, 1.0f, 1.0f};
inline constexpr glm::vec4 kCyan{0.2f, 0.9f, 1.0f, 1.0f};
inline constexpr glm::vec4 kYellow{1.0f, 0.9f, 0.2f, 1.0f};
inline constexpr glm::vec4 kOrange{1.0f, 0.6f, 0.15f, 1.0f};
inline constexpr glm::vec4 kMagenta{1.0f, 0.3f, 0.9f, 1.0f};
inline constexpr glm::vec4 kGrey{0.65f, 0.65f, 0.65f, 1.0f};

struct Style {
    float duration = 0.0f;  // seconds; 0 = this frame
    float thickness = 2.0f; // pixels
    bool depthTest = true;  // false: always on top
};

// --- Shapes ----------------------------------------------------------------------------
void line(const glm::vec3& a, const glm::vec3& b, const glm::vec4& color, const Style& style = {});
void ray(const glm::vec3& origin, const glm::vec3& direction, const glm::vec4& color, const Style& style = {}); // origin + direction
void arrow(const glm::vec3& from, const glm::vec3& to, const glm::vec4& color, const Style& style = {});
void path(std::span<const glm::vec3> points, const glm::vec4& color, const Style& style = {});
void cross(const glm::vec3& p, float size, const glm::vec4& color, const Style& style = {});
void axes(const glm::vec3& position, const glm::quat& rotation, float size = 1.0f, const Style& style = {}); // x red, y green, z blue
void box(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation, const glm::vec4& color, const Style& style = {});
void aabb(const glm::vec3& min, const glm::vec3& max, const glm::vec4& color, const Style& style = {});
void circle(const glm::vec3& center, const glm::vec3& normal, float radius, const glm::vec4& color, const Style& style = {}, int segments = 32);
void sphere(const glm::vec3& center, float radius, const glm::vec4& color, const Style& style = {});
void capsule(const glm::vec3& a, const glm::vec3& b, float radius, const glm::vec4& color, const Style& style = {}); // segment a-b
void cylinder(const glm::vec3& base, const glm::vec3& top, float radius, const glm::vec4& color, const Style& style = {});
void text(const glm::vec3& position, const std::string& text, const glm::vec4& color = kWhite, const Style& style = {});
void clear(); // everything, including lines that would have lasted

// --- Channels ----------------------------------------------------------------------------
// (std::less<>: looked up by string_view, so enabled("literal") doesn't build a std::string each call.)
using ChannelMap = std::map<std::string, bool, std::less<>>;
bool enabled(std::string_view channel); // registers the channel (off) the first time it's asked about
void setChannel(const std::string& channel, bool on);
const ChannelMap& channels();

// --- Engine internal -----------------------------------------------------------------------
struct Line {
    glm::vec4 a;     // xyz, w = thickness
    glm::vec4 b;     // xyz, w = 1: depth tested
    glm::vec4 color; // sRGB
};
struct Label {
    glm::vec3 position;
    std::string text;
    glm::vec4 color;
};
const std::vector<Line>& lines();
const std::vector<Label>& labels();
void drawEngineChannels(); // colliders, navmesh, agents (once per frame, before rendering)
void drawLabels();         // into ImGui's background list (in the GUI phase)
void endFrame(double now); // drops what expired
size_t lineCount();

} // namespace ve::debug
