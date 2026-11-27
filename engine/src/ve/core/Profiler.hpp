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

/// CPU time per named zone (VE_PROFILE scopes), nested, averaged over about a
/// second: shown in the F1 overlay's "CPU zones" table and in VE_BENCH output.
/// Main thread only.
class Profiler {
public:
    /// One zone's timings.
    struct Zone {
        std::string name;  ///< The name given to VE_PROFILE.
        int parent = -1;     ///< index into zones(), -1 = top level
        int depth = 0;  ///< Nesting depth (0 = top level).
        double frameMs = 0;  ///< this frame (all calls)
        double avgMs = 0;    ///< smoothed over ~1 s
        double maxMs = 0;    ///< over the last ~1 s
        uint32_t calls = 0;  ///< this frame
        uint64_t lastFrame = 0; ///< last frame it ran (zones idle for ~2 s are dropped)
    };

    /// The profiler (one per process).
    static Profiler& get();
    /// Engine: starts a frame's measurements.
    void beginFrame();
    /// Engine: ends the frame (updates averages, drops idle zones).
    void endFrame();
    void reset(); ///< forget all zones (e.g. after a loading screen)
    /// Every zone seen recently, parents before their children.
    const std::vector<Zone>& zones() const { return m_zones; }
    bool enabled = true;  ///< Off: VE_PROFILE scopes cost (almost) nothing and record nothing.

    /// Times its lifetime into a zone; use the VE_PROFILE("name") macro rather than this directly.
    class Scope {
    public:
        /// Enters the zone `name` (a string literal: kept by pointer).
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
/// Times the rest of the enclosing block as the zone `name` (a string literal):
///     void Foliage::update(float dt) { VE_PROFILE("Foliage rules"); ... }
#define VE_PROFILE(name) ::ve::Profiler::Scope VE_PROFILE_CAT(veProfileScope, __LINE__)(name)

} // namespace ve
