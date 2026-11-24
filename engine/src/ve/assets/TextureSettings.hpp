#pragma once

// =============================================================================
// TextureSettings - how an image file becomes a texture, decided once per file
// (not by whoever loads it).
//
// Each image may have a settings file next to it, named after it plus
// ".texture" (concrete_n.png -> concrete_n.png.texture):
//
//   { "type": "Normal", "maxSize": 1024 }
//
// Fields (all optional):
//   type      "Color" (sRGB colour, alpha kept), "Data" (linear: roughness,
//             AO, masks, packed ORM, LUTs...) or "Normal" (tangent-space normal
//             map: only X/Y are stored, Z is rebuilt in the shader)
//   mipmaps   true          (false: UI images, LUTs, pixel-exact data)
//   compress  true          (false: keep 8-bit RGBA when cooking)
//   maxSize   0             (> 0: halve the image until neither side exceeds it)
//   filter    "Linear" or "Nearest" (pixel art)
//   wrap      "Repeat" or "Clamp"
//
// Without a settings file the type comes from the file name's last word
// (separated by _ - . or space; a size suffix like "_2k" is skipped):
//   normal, nrm, nor, norm, n                                       -> Normal
//   rough, roughness, metal, metallic, metalness, ao, occlusion, orm,
//   arm, rma, mr, mask, height, disp, displacement, detail, noise,
//   data, lut, opacity, gloss, spec, specular                       -> Data
//   anything else (albedo, basecolor, diffuse, color...)            -> Color
// Everything else keeps its default.
//
// The cooker (TextureCook.hpp) turns these into GPU formats: Color -> BC7
// sRGB, Normal -> BC5 (X/Y), Data -> BC4 when the image is greyscale (seen as
// R,R,R,1 like the source), else BC7.
// =============================================================================

#include "ve/gfx/Samplers.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace ve {

/// What an image holds: colour (sRGB, BC7), data (linear: masks, roughness;
/// BC4 / BC7) or a tangent-space normal map (BC5, Z rebuilt in shaders).
enum class TextureType { Color, Data, Normal };
/// Texture filtering: smooth, or pixel art.
enum class TextureFilter { Linear, Nearest };
/// What happens past the 0..1 UV range.
enum class TextureWrap { Repeat, Clamp };

/// How an image file loads and cooks: decided by the file (`<image>.texture`,
/// else its name), not by the code that loads it (see the top of this file).
struct TextureSettings {
    TextureType type = TextureType::Color;        ///< Colour, data or normal map.
    bool mipmaps = true;                          ///< Build the mip chain (off: UI images, pixel art).
    bool compress = true;                         ///< Block-compress when cooked (off: RGBA8).
    uint32_t maxSize = 0;                         ///< Downscale so neither side exceeds this (pixels); 0 = keep.
    TextureFilter filter = TextureFilter::Linear; ///< Sampler filtering.
    TextureWrap wrap = TextureWrap::Repeat;       ///< Sampler addressing.

    /// Whether the texture is sampled as sRGB (colour images only).
    bool srgb() const { return type == TextureType::Color; }
    /// The bindless sampler for filter + wrap.
    SamplerType sampler() const;

    /// The settings of an image file: its .texture file if there is one
    /// (fields it leaves out: from the name / defaults), else by name.
    static TextureSettings forImage(const std::filesystem::path& imageFile);
    /// The type the file name suggests (the rule above).
    static TextureType typeFromName(const std::filesystem::path& imageFile);
    /// <image>.texture
    static std::filesystem::path settingsFile(const std::filesystem::path& imageFile);

    nlohmann::json toJson() const;           ///< The settings as a .texture file's JSON.
    void applyJson(const nlohmann::json& j); ///< Sets the fields `j` has (others keep their values).

    /// Changes when the cooked result would differ (type, mipmaps, compress,
    /// maxSize; not filter / wrap, which only pick the sampler).
    uint64_t cookHash() const;
};

const char* toString(TextureType type);

} // namespace ve
