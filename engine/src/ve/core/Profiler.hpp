#pragma once

// =============================================================================
// Profiler - CPU timing zones on the main thread (F1 overlay "CPU zones",
// VE_BENCH output). GPU passes are timed separately (render graph).
//
//   void Foliage::update(float dt) {
//       VE_PROFILE("Foliage");          // until the end of the scope
//       ...
//   }
//
// Zones nest (a zone opened inside another is its child). Each frame's
// timings are averaged over ~1 s for display. Zones on other threads are
// ignored. Cost: two clock reads and a short search per zone.
// =============================================================================

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace ve {

class Profiler {
public:
    struct Zone {
        std::string name;
        int parent = -1;     // index into zones(), -1 = top level
        int depth = 0;
        double frameMs = 0;  // this frame (all calls)
        double avgMs = 0;    // smoothed over ~1 s
        double maxMs = 0;    // over the last ~1 s
        uint32_t calls = 0;  // this frame
        uint64_t lastFrame = 0; // last frame it ran (zones idle for ~2 s are dropped)
    };

    static Profiler& get();
    void beginFrame();
    void endFrame();
    void reset(); // forget all zones (e.g. after a loading screen)
    const std::vector<Zone>& zones() const { return m_zones; }
    bool enabled = true;

    class Scope {
    public:
        explicit Scope(const char* name);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        int m_zone = -1;
        std::chrono::steady_clock::time_point m_start;
    };

private:
    int enter(const char* name);
    void leave(int zone, double ms);

    std::vector<Zone> m_zones;
    std::vector<int> m_stack;
    std::vector<double> m_windowMax;
    uint64_t m_frames = 0;
};

#define VE_PROFILE_CAT2(a, b) a##b
#define VE_PROFILE_CAT(a, b) VE_PROFILE_CAT2(a, b)
#define VE_PROFILE(name) ::ve::Profiler::Scope VE_PROFILE_CAT(veProfileScope, __LINE__)(name)

} // namespace ve
