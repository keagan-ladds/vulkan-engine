#pragma once
// =============================================================================
// DebugLineBuffer - the world-space lines DebugLinePass draws this frame
// (engine internal). render owns the buffer and its layout (the pass's vertex
// format); ve::debug (ve/debug/Debug.hpp) is what fills it: it keeps its line
// list here directly, with its own expiry times beside it, so render never
// includes the debug module.
// =============================================================================

#include <glm/glm.hpp>

#include <vector>

namespace ve {

struct DebugLine {
    glm::vec4 a;     ///< xyz, w = thickness (pixels)
    glm::vec4 b;     ///< xyz, w = 1: depth tested
    glm::vec4 color; ///< sRGB
};

class DebugLineBuffer {
public:
    /// Process-wide, like ve::debug's state: lines can be drawn before the
    /// Engine (and its renderer) exists, and last across scene changes.
    static DebugLineBuffer& get();

    std::vector<DebugLine> lines;
};

} // namespace ve
