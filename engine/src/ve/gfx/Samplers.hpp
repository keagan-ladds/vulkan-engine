#pragma once

// =============================================================================
// Samplers - a fixed set of commonly needed samplers, created once.
// A sampler describes *how* a texture is read (filtering, wrapping, compare).
// =============================================================================

#include "ve/gfx/Vk.hpp"

#include <array>

namespace ve {

class Device;

enum class SamplerType : uint32_t {
    LinearRepeat,   // default for material textures (with anisotropy + mips)
    LinearClamp,    // render targets / post-processing
    NearestRepeat,  // pixel-art / Valheim-style crunchy textures
    NearestClamp,   // exact texel fetches
    ShadowCompare,  // depth comparison (hardware PCF) for shadow maps
    Count
};

class Samplers {
public:
    void init(const Device& device);
    void shutdown(const Device& device);
    VkSampler get(SamplerType type) const { return m_samplers[static_cast<size_t>(type)]; }

private:
    std::array<VkSampler, static_cast<size_t>(SamplerType::Count)> m_samplers{};
};

} // namespace ve
