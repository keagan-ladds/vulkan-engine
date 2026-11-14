#pragma once

// =============================================================================
// Time - frame timing, owned by the Engine and readable from anywhere via
// Engine::get().time() (or GameObject::time()).
//
// Two clocks exist, like in Unity:
//  - update clock:  deltaTime varies per frame (rendering rate).
//  - fixed clock:   fixedDeltaTime is constant (default 1/50 s). fixedUpdate()
//                   is called 0..N times per frame to "catch up", which keeps
//                   physics-like simulation deterministic and frame-rate independent.
// =============================================================================

#include <cstdint>

namespace ve {

struct Time {
    float deltaTime = 0.0f;         // scaled seconds since last frame (use for gameplay)
    float unscaledDeltaTime = 0.0f; // real seconds since last frame (use for UI, debug cameras)
    float time = 0.0f;              // scaled seconds since start
    float realTime = 0.0f;          // real seconds since start
    float timeScale = 1.0f;         // 0 = paused, 0.5 = slow motion
    float fixedDeltaTime = 1.0f / 50.0f;
    // How far we are between the last fixed step and the next one (0..1).
    // Useful to interpolate fixed-step positions for smooth rendering.
    float fixedAlpha = 0.0f;
    uint64_t frameCount = 0;
};

} // namespace ve
