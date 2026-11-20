#include "ve/debug/Debug.hpp"

#include "ve/core/Engine.hpp"
#include "ve/core/Time.hpp"
#include "ve/nav/Navigation.hpp"
#include "ve/physics/Physics.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/render/ViewProjector.hpp"
#include "ve/scene/Scene.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace ve::debug {

namespace {
struct State {
    std::vector<Line> lines;
    std::vector<double> lineExpiry; // < 0: this frame
    std::vector<Label> labels;
    std::vector<double> labelExpiry;
    ChannelMap channels{
        {"Physics colliders", false}, {"Physics terrain", false}, {"Character controllers", false},
        {"Navmesh", false},           {"Nav agents", false},        {"Physics joints", false},
    };
};
State& state()
{
    static State s;
    return s;
}

double expiry(const Style& style)
{
    if (style.duration <= 0.0f || !Engine::exists()) return -1.0;
    return Engine::get().time().realTime + double(style.duration);
}

// Two unit vectors perpendicular to n (and each other).
void basis(const glm::vec3& n, glm::vec3& u, glm::vec3& v)
{
    const glm::vec3 a = std::abs(n.y) < 0.95f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    u = glm::normalize(glm::cross(n, a));
    v = glm::cross(n, u);
}

void arc(const glm::vec3& c, const glm::vec3& u, const glm::vec3& v, float r, float from, float to, int segments,
         const glm::vec4& color, const Style& style)
{
    glm::vec3 prev = c + r * (std::cos(from) * u + std::sin(from) * v);
    for (int i = 1; i <= segments; ++i) {
        const float a = from + (to - from) * float(i) / float(segments);
        const glm::vec3 p = c + r * (std::cos(a) * u + std::sin(a) * v);
        line(prev, p, color, style);
        prev = p;
    }
}
} // namespace

// -----------------------------------------------------------------------------
// Shapes
// -----------------------------------------------------------------------------

void line(const glm::vec3& a, const glm::vec3& b, const glm::vec4& color, const Style& style)
{
    State& s = state();
    if (s.lines.size() >= 400000) return; // (something is drawing every frame with a duration)
    s.lines.push_back({glm::vec4(a, style.thickness), glm::vec4(b, style.depthTest ? 1.0f : 0.0f), color});
    s.lineExpiry.push_back(expiry(style));
}

void ray(const glm::vec3& origin, const glm::vec3& direction, const glm::vec4& color, const Style& style)
{
    line(origin, origin + direction, color, style);
}

void arrow(const glm::vec3& from, const glm::vec3& to, const glm::vec4& color, const Style& style)
{
    line(from, to, color, style);
    const glm::vec3 d = to - from;
    const float len = glm::length(d);
    if (len < 1e-4f) return;
    const glm::vec3 n = d / len;
    glm::vec3 u, v;
    basis(n, u, v);
    const float head = std::min(0.3f, len * 0.25f);
    for (const glm::vec3& side : {u, -u, v, -v}) line(to, to - n * head + side * head * 0.4f, color, style);
}

void path(std::span<const glm::vec3> points, const glm::vec4& color, const Style& style)
{
    for (size_t i = 1; i < points.size(); ++i) line(points[i - 1], points[i], color, style);
}

void cross(const glm::vec3& p, float size, const glm::vec4& color, const Style& style)
{
    const float h = size * 0.5f;
    line(p - glm::vec3(h, 0, 0), p + glm::vec3(h, 0, 0), color, style);
    line(p - glm::vec3(0, h, 0), p + glm::vec3(0, h, 0), color, style);
    line(p - glm::vec3(0, 0, h), p + glm::vec3(0, 0, h), color, style);
}

void axes(const glm::vec3& position, const glm::quat& rotation, float size, const Style& style)
{
    line(position, position + rotation * glm::vec3(size, 0, 0), kRed, style);
    line(position, position + rotation * glm::vec3(0, size, 0), kGreen, style);
    line(position, position + rotation * glm::vec3(0, 0, size), kBlue, style);
}

void box(const glm::vec3& center, const glm::vec3& h, const glm::quat& rotation, const glm::vec4& color, const Style& style)
{
    glm::vec3 c[8];
    for (int i = 0; i < 8; ++i)
        c[i] = center + rotation * glm::vec3(i & 1 ? h.x : -h.x, i & 2 ? h.y : -h.y, i & 4 ? h.z : -h.z);
    static constexpr int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : kEdges) line(c[e[0]], c[e[1]], color, style);
}

void aabb(const glm::vec3& min, const glm::vec3& max, const glm::vec4& color, const Style& style)
{
    box((min + max) * 0.5f, (max - min) * 0.5f, glm::quat(1, 0, 0, 0), color, style);
}

void circle(const glm::vec3& center, const glm::vec3& normal, float radius, const glm::vec4& color, const Style& style, int segments)
{
    glm::vec3 u, v;
    basis(glm::normalize(normal), u, v);
    arc(center, u, v, radius, 0.0f, 6.2831853f, segments, color, style);
}

void sphere(const glm::vec3& center, float radius, const glm::vec4& color, const Style& style)
{
    circle(center, {1, 0, 0}, radius, color, style, 24);
    circle(center, {0, 1, 0}, radius, color, style, 24);
    circle(center, {0, 0, 1}, radius, color, style, 24);
}

void capsule(const glm::vec3& a, const glm::vec3& b, float radius, const glm::vec4& color, const Style& style)
{
    glm::vec3 axis = b - a;
    const float len = glm::length(axis);
    axis = len > 1e-5f ? axis / len : glm::vec3(0, 1, 0);
    glm::vec3 u, v;
    basis(axis, u, v);
    circle(a, axis, radius, color, style, 20);
    circle(b, axis, radius, color, style, 20);
    for (const glm::vec3& side : {u, -u, v, -v}) line(a + side * radius, b + side * radius, color, style);
    constexpr float pi = 3.14159265f;
    // The two caps: half circles in both planes through the axis.
    arc(b, u, axis, radius, 0.0f, pi, 10, color, style);
    arc(b, v, axis, radius, 0.0f, pi, 10, color, style);
    arc(a, u, -axis, radius, 0.0f, pi, 10, color, style);
    arc(a, v, -axis, radius, 0.0f, pi, 10, color, style);
}

void cylinder(const glm::vec3& base, const glm::vec3& top, float radius, const glm::vec4& color, const Style& style)
{
    glm::vec3 axis = top - base;
    const float len = glm::length(axis);
    axis = len > 1e-5f ? axis / len : glm::vec3(0, 1, 0);
    glm::vec3 u, v;
    basis(axis, u, v);
    circle(base, axis, radius, color, style, 20);
    circle(top, axis, radius, color, style, 20);
    for (const glm::vec3& side : {u, -u, v, -v}) line(base + side * radius, top + side * radius, color, style);
}

void text(const glm::vec3& position, const std::string& str, const glm::vec4& color, const Style& style)
{
    State& s = state();
    if (s.labels.size() >= 5000) return;
    s.labels.push_back({position, str, color});
    s.labelExpiry.push_back(expiry(style));
}

void clear()
{
    State& s = state();
    s.lines.clear();
    s.lineExpiry.clear();
    s.labels.clear();
    s.labelExpiry.clear();
}

// -----------------------------------------------------------------------------
// Channels
// -----------------------------------------------------------------------------

bool enabled(std::string_view channel)
{
    ChannelMap& channels = state().channels;
    if (auto it = channels.find(channel); it != channels.end()) return it->second;
    channels.emplace(std::string(channel), false);
    return false;
}

void setChannel(const std::string& channel, bool on)
{
    state().channels[channel] = on;
}

const ChannelMap& channels()
{
    return state().channels;
}

// -----------------------------------------------------------------------------
// Engine internal
// -----------------------------------------------------------------------------

const std::vector<Line>& lines()
{
    return state().lines;
}

const std::vector<Label>& labels()
{
    return state().labels;
}

size_t lineCount()
{
    return state().lines.size();
}

void drawEngineChannels()
{
    if (!Engine::exists()) return;
    Engine& e = Engine::get();
    if (e.loading()) return;
    Scene& scene = e.scene();
    const glm::vec3 eye = e.renderer().frameData().camera.position;
    if (enabled("Physics colliders") || enabled("Physics terrain") || enabled("Character controllers"))
        e.physics().drawDebug(eye, 40.0f, enabled("Physics colliders"), enabled("Physics terrain"), enabled("Character controllers"), eye);
    if (enabled("Physics joints")) scene.forEach<Joint>([](Joint& j) { j.drawDebug(); });
    if (enabled("Navmesh"))
        scene.forEach<NavMeshSurface>([&](NavMeshSurface& nav) { nav.drawDebugLines(eye, 50.0f); });
    if (enabled("Nav agents")) scene.forEach<NavAgent>([&](NavAgent& a) { a.drawDebug(); });
}

void drawLabels()
{
    State& s = state();
    if (s.labels.empty() || !Engine::exists()) return;
    Engine& e = Engine::get();
    if (e.renderer().photoCaptureActive()) return;
    const ImGuiIO& io = ImGui::GetIO();
    const ViewProjector view = ViewProjector::from(e.renderer().frameData().camera, {io.DisplaySize.x, io.DisplaySize.y});
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    for (const Label& l : s.labels) {
        const auto p = view.project(l.position);
        if (!p) continue;
        const ImVec2 size = ImGui::CalcTextSize(l.text.c_str());
        const ImVec2 at(p->x - size.x * 0.5f, p->y - size.y * 0.5f);
        const ImU32 c = ImGui::ColorConvertFloat4ToU32({l.color.r, l.color.g, l.color.b, l.color.a});
        list->AddRectFilled({at.x - 3, at.y - 1}, {at.x + size.x + 3, at.y + size.y + 1}, IM_COL32(0, 0, 0, 140), 3.0f);
        list->AddText(at, c, l.text.c_str());
    }
}

void endFrame(double now)
{
    State& s = state();
    size_t w = 0;
    for (size_t i = 0; i < s.lines.size(); ++i) {
        if (s.lineExpiry[i] < 0.0 || s.lineExpiry[i] <= now) continue;
        s.lines[w] = s.lines[i];
        s.lineExpiry[w++] = s.lineExpiry[i];
    }
    s.lines.resize(w);
    s.lineExpiry.resize(w);
    w = 0;
    for (size_t i = 0; i < s.labels.size(); ++i) {
        if (s.labelExpiry[i] < 0.0 || s.labelExpiry[i] <= now) continue;
        s.labels[w] = std::move(s.labels[i]);
        s.labelExpiry[w++] = s.labelExpiry[i];
    }
    s.labels.resize(w);
    s.labelExpiry.resize(w);
}

} // namespace ve::debug
