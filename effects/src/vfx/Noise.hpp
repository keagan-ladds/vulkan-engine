#pragma once

// Procedural, tileable noise used by the effects (clouds, fog...).

#include <cstdint>
#include <vector>

namespace vfx {

// size^3 RGBA8 volume, tileable in all axes:
//   R = Perlin-Worley (cloud base shape), G/B/A = inverted Worley FBM at
//   increasing frequencies (detail erosion). Generated on all CPU cores.
std::vector<uint8_t> generateCloudNoise(uint32_t size = 64);

// size^2 RGBA8 soft round sprite (white, alpha falloff) for particles.
std::vector<uint8_t> generateSoftSprite(uint32_t size = 64);
// size^2 RGBA8 cloudy puff sprite (smoke, dust).
std::vector<uint8_t> generateSmokeSprite(uint32_t size = 128);

} // namespace vfx
