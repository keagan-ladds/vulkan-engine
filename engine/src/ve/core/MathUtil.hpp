#pragma once

// =============================================================================
// MathUtil - small helpers for the engine's conventions (right-handed, +Y up,
// forward = -Z, yaw 0 looks north / -Z, positive yaw turns left / west).
//
//   const float yaw = ve::math::yawOf(target - eye);          // radians
//   player.setPose(eye, glm::degrees(yaw), glm::degrees(ve::math::pitchOf(d)));
//   const glm::vec3 fwd = ve::math::flat(camera.forward());   // walk direction
// =============================================================================

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace ve::math {

/// Yaw (radians, about +Y) that turns -Z towards `dir`; ignores dir.y.
inline float yawOf(const glm::vec3& dir) { return std::atan2(-dir.x, -dir.z); }

/// Pitch (radians, positive looks up) of `dir`.
inline float pitchOf(const glm::vec3& dir) { return std::atan2(dir.y, glm::length(glm::vec2(dir.x, dir.z))); }

/// Rotation about +Y by `yaw` radians.
inline glm::quat yawRotation(float yaw) { return glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f)); }

/// `v` projected onto the ground plane and normalised; `fallback` when v is (almost) vertical.
inline glm::vec3 flat(const glm::vec3& v, const glm::vec3& fallback = glm::vec3(0.0f, 0.0f, -1.0f))
{
    const glm::vec3 f(v.x, 0.0f, v.z);
    const float len = glm::length(f);
    return len > 1e-5f ? f / len : fallback;
}

} // namespace ve::math
