#pragma once

// Small deterministic CPU noise helpers for procedural world generation
// (terrain, biomes, scattering). Same results on every platform.

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>

namespace ve::noise {

inline float hash2(int x, int y, uint32_t seed = 0)
{
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFFu) / 16777216.0f;
}

// Smooth value noise in [0, 1].
inline float valueNoise(glm::vec2 p, uint32_t seed = 0)
{
    const glm::vec2 i = glm::floor(p);
    glm::vec2 f = p - i;
    f = f * f * (3.0f - 2.0f * f);
    const int x = static_cast<int>(i.x), y = static_cast<int>(i.y);
    const float a = hash2(x, y, seed), b = hash2(x + 1, y, seed);
    const float c = hash2(x, y + 1, seed), d = hash2(x + 1, y + 1, seed);
    return glm::mix(glm::mix(a, b, f.x), glm::mix(c, d, f.x), f.y);
}

// Fractal Brownian motion: several octaves of noise, in roughly [0, 1].
inline float fbm(glm::vec2 p, int octaves, uint32_t seed = 0)
{
    float v = 0.0f, amp = 0.5f, norm = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        v += valueNoise(p, seed + static_cast<uint32_t>(o) * 17u) * amp;
        norm += amp;
        amp *= 0.5f;
        p = p * 2.03f + glm::vec2(17.1f, 9.7f);
    }
    return v / norm;
}

// Simple deterministic random number generator for scattering.
class Rng {
public:
    explicit Rng(uint32_t seed) : m_state(seed * 747796405u + 2891336453u) {}
    float next()
    {
        m_state = m_state * 747796405u + 2891336453u;
        uint32_t w = ((m_state >> ((m_state >> 28u) + 4u)) ^ m_state) * 277803737u;
        w = (w >> 22u) ^ w;
        return static_cast<float>(w & 0xFFFFFFu) / 16777216.0f;
    }
    float range(float lo, float hi) { return lo + (hi - lo) * next(); }

private:
    uint32_t m_state;
};

} // namespace ve::noise
