#include "vfx/Noise.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <thread>

namespace vfx {

namespace {

// Integer hash -> [0,1). Periodic inputs make the noise tile.
uint32_t hash3(int x, int y, int z, uint32_t seed)
{
    uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u ^
                 static_cast<uint32_t>(z) * 83492791u ^ seed * 2654435761u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

float hashf(int x, int y, int z, uint32_t seed)
{
    return static_cast<float>(hash3(x, y, z, seed) & 0xFFFFFFu) / 16777216.0f;
}

int wrap(int v, int period)
{
    return ((v % period) + period) % period;
}

glm::vec3 gradient(int x, int y, int z, int period)
{
    x = wrap(x, period);
    y = wrap(y, period);
    z = wrap(z, period);
    const float a = hashf(x, y, z, 1) * 6.2831853f;
    const float b = hashf(x, y, z, 2) * 2.0f - 1.0f;
    const float r = std::sqrt(1.0f - b * b);
    return {r * std::cos(a), b, r * std::sin(a)};
}

// Tileable gradient (Perlin) noise, roughly in [-1, 1].
float perlin(const glm::vec3& p, int period)
{
    const glm::ivec3 i = glm::ivec3(glm::floor(p));
    const glm::vec3 f = p - glm::floor(p);
    const glm::vec3 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f); // quintic fade
    float n[8];
    int k = 0;
    for (int z = 0; z <= 1; ++z) {
        for (int y = 0; y <= 1; ++y) {
            for (int x = 0; x <= 1; ++x) {
                n[k++] = glm::dot(gradient(i.x + x, i.y + y, i.z + z, period), f - glm::vec3(x, y, z));
            }
        }
    }
    const float x00 = glm::mix(n[0], n[1], u.x), x10 = glm::mix(n[2], n[3], u.x);
    const float x01 = glm::mix(n[4], n[5], u.x), x11 = glm::mix(n[6], n[7], u.x);
    return glm::mix(glm::mix(x00, x10, u.y), glm::mix(x01, x11, u.y), u.z) * 1.6f;
}

// Tileable Worley noise: distance to the nearest feature point, inverted so
// cell centres are bright (puffy). `cells` = cells per tile.
float worley(const glm::vec3& p01, int cells, uint32_t seed)
{
    const glm::vec3 p = p01 * static_cast<float>(cells);
    const glm::ivec3 c = glm::ivec3(glm::floor(p));
    float best = 1e9f;
    for (int z = -1; z <= 1; ++z) {
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                const glm::ivec3 n = c + glm::ivec3(x, y, z);
                const int hx = wrap(n.x, cells), hy = wrap(n.y, cells), hz = wrap(n.z, cells);
                const glm::vec3 feature = glm::vec3(n) + glm::vec3(hashf(hx, hy, hz, seed), hashf(hx, hy, hz, seed + 7),
                                                                   hashf(hx, hy, hz, seed + 13));
                const glm::vec3 d = feature - p;
                best = std::min(best, glm::dot(d, d));
            }
        }
    }
    return 1.0f - std::clamp(std::sqrt(best), 0.0f, 1.0f);
}

float worleyFbm(const glm::vec3& p, int cells, uint32_t seed)
{
    return worley(p, cells, seed) * 0.625f + worley(p, cells * 2, seed + 31) * 0.25f +
           worley(p, cells * 4, seed + 57) * 0.125f;
}

float remap(float v, float lo, float hi, float nlo, float nhi)
{
    return nlo + (v - lo) / std::max(hi - lo, 1e-5f) * (nhi - nlo);
}

uint8_t toByte(float v)
{
    return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

} // namespace

std::vector<uint8_t> generateCloudNoise(uint32_t size)
{
    std::vector<uint8_t> data(size_t(size) * size * size * 4);
    auto slice = [&](uint32_t z0, uint32_t z1) {
        for (uint32_t z = z0; z < z1; ++z) {
            for (uint32_t y = 0; y < size; ++y) {
                for (uint32_t x = 0; x < size; ++x) {
                    const glm::vec3 p = (glm::vec3(x, y, z) + 0.5f) / static_cast<float>(size);
                    // Perlin FBM, 4 octaves, base frequency 4 (tiles exactly).
                    float pn = 0.0f, amp = 1.0f, norm = 0.0f;
                    for (int o = 0; o < 4; ++o) {
                        const int freq = 4 << o;
                        pn += perlin(p * static_cast<float>(freq), freq) * amp;
                        norm += amp;
                        amp *= 0.5f;
                    }
                    pn = pn / norm * 0.5f + 0.5f;
                    const float w = worleyFbm(p, 4, 100);
                    const float perlinWorley = remap(pn, 0.0f, 1.0f, w, 1.0f);
                    uint8_t* px = &data[((size_t(z) * size + y) * size + x) * 4];
                    px[0] = toByte(remap(perlinWorley, 0.25f, 1.0f, 0.0f, 1.0f));
                    px[1] = toByte(worleyFbm(p, 4, 200));
                    px[2] = toByte(worleyFbm(p, 8, 300));
                    px[3] = toByte(worleyFbm(p, 16, 400));
                }
            }
        }
    };
    const uint32_t threads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    const uint32_t per = (size + threads - 1) / threads;
    for (uint32_t t = 0; t < threads; ++t) {
        const uint32_t z0 = t * per, z1 = std::min(size, z0 + per);
        if (z0 < z1) pool.emplace_back(slice, z0, z1);
    }
    for (auto& th : pool) th.join();
    return data;
}

std::vector<uint8_t> generateSoftSprite(uint32_t size)
{
    std::vector<uint8_t> data(size_t(size) * size * 4);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const glm::vec2 d = (glm::vec2(x, y) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
            const float r = glm::length(d);
            const float a = std::pow(std::clamp(1.0f - r, 0.0f, 1.0f), 1.5f);
            uint8_t* px = &data[(size_t(y) * size + x) * 4];
            px[0] = px[1] = px[2] = 255;
            px[3] = toByte(a);
        }
    }
    return data;
}

std::vector<uint8_t> generateSmokeSprite(uint32_t size)
{
    std::vector<uint8_t> data(size_t(size) * size * 4);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const glm::vec2 uv = (glm::vec2(x, y) + 0.5f) / static_cast<float>(size);
            const glm::vec2 d = uv * 2.0f - 1.0f;
            const float r = glm::length(d);
            float n = 0.0f, amp = 0.5f;
            for (int o = 0; o < 4; ++o) {
                const int f = 4 << o;
                n += perlin(glm::vec3(uv * static_cast<float>(f), 0.5f), f) * amp;
                amp *= 0.5f;
            }
            const float a = std::clamp((1.0f - r) * 1.4f + n * 0.6f - 0.2f, 0.0f, 1.0f) *
                            std::clamp(1.0f - r, 0.0f, 1.0f);
            uint8_t* px = &data[(size_t(y) * size + x) * 4];
            const uint8_t shade = toByte(0.85f + n * 0.3f);
            px[0] = px[1] = px[2] = shade;
            px[3] = toByte(a);
        }
    }
    return data;
}

} // namespace vfx
