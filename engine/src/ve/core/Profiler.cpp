#include "ve/core/Profiler.hpp"

#include <algorithm>
#include <thread>

namespace ve {

namespace {
std::thread::id g_mainThread = std::this_thread::get_id(); // the thread that loaded the engine
}

Profiler& Profiler::get()
{
    static Profiler p;
    return p;
}

void Profiler::beginFrame()
{
    g_mainThread = std::this_thread::get_id();
    m_stack.clear();
    for (Zone& z : m_zones) {
        z.frameMs = 0;
        z.calls = 0;
    }
}

void Profiler::endFrame()
{
    ++m_frames;
    m_windowMax.resize(m_zones.size(), 0.0);
    const bool newWindow = m_frames % 60 == 0;
    for (size_t i = 0; i < m_zones.size(); ++i) {
        Zone& z = m_zones[i];
        z.avgMs = z.avgMs * 0.95 + z.frameMs * 0.05; // ~1 s at 60 fps
        m_windowMax[i] = std::max(m_windowMax[i], z.frameMs);
        if (newWindow) {
            z.maxMs = m_windowMax[i];
            m_windowMax[i] = 0.0;
        }
    }
    // Drop zones that stopped running (e.g. loading-screen frames, a system that
    // was removed). A parent runs whenever its children do, so subtrees go together.
    if (newWindow) {
        std::vector<int> remap(m_zones.size(), -1);
        std::vector<Zone> kept;
        std::vector<double> keptMax;
        for (size_t i = 0; i < m_zones.size(); ++i) {
            if (m_frames - m_zones[i].lastFrame > 120) continue;
            remap[i] = int(kept.size());
            kept.push_back(m_zones[i]);
            keptMax.push_back(m_windowMax[i]);
        }
        if (kept.size() != m_zones.size()) {
            for (Zone& z : kept)
                if (z.parent >= 0) z.parent = remap[size_t(z.parent)];
            m_zones = std::move(kept);
            m_windowMax = std::move(keptMax);
        }
    }
}

void Profiler::reset()
{
    // Called between frames: no zone is open.
    m_zones.clear();
    m_windowMax.clear();
    m_stack.clear();
}

int Profiler::enter(const char* name)
{
    if (!enabled || std::this_thread::get_id() != g_mainThread) return -1;
    const int parent = m_stack.empty() ? -1 : m_stack.back();
    int index = -1;
    for (size_t i = 0; i < m_zones.size(); ++i)
        if (m_zones[i].parent == parent && m_zones[i].name == name) index = int(i);
    if (index < 0) {
        Zone z;
        z.name = name;
        z.parent = parent;
        z.depth = parent < 0 ? 0 : m_zones[size_t(parent)].depth + 1;
        // Keep children right after their parent (display order).
        size_t at = m_zones.size();
        if (parent >= 0) {
            at = size_t(parent) + 1;
            while (at < m_zones.size() && m_zones[at].depth > m_zones[size_t(parent)].depth) ++at;
            for (Zone& other : m_zones)
                if (other.parent >= int(at)) ++other.parent;
            for (int& s : m_stack)
                if (s >= int(at)) ++s;
        }
        m_zones.insert(m_zones.begin() + ptrdiff_t(at), z);
        if (at < m_windowMax.size()) m_windowMax.insert(m_windowMax.begin() + ptrdiff_t(at), 0.0);
        index = int(at);
    }
    m_stack.push_back(index);
    return index;
}

void Profiler::leave(int zone, double ms)
{
    if (zone < 0 || m_stack.empty()) return;
    const int top = m_stack.back();
    m_stack.pop_back();
    Zone& z = m_zones[size_t(top)];
    z.frameMs += ms;
    ++z.calls;
    z.lastFrame = m_frames;
}

Profiler::Scope::Scope(const char* name) : m_zone(Profiler::get().enter(name))
{
    if (m_zone >= 0) m_start = std::chrono::steady_clock::now();
}

Profiler::Scope::~Scope()
{
    if (m_zone < 0) return;
    Profiler::get().leave(m_zone, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_start).count());
}

} // namespace ve
