#include "ve/assets/MaterialAsset.hpp"

#include "ve/assets/AssetManager.hpp"
#include "ve/core/Log.hpp"
#include "ve/render/Decals.hpp"
#include "ve/render/Texture.hpp"
#include "ve/scene/Reflection.hpp"
#include "ve/scene/SceneSerializer.hpp"

namespace ve {

MaterialAsset MaterialAsset::fromJson(const Json& j)
{
    MaterialAsset m;
    if (!j.is_object()) return m;
    if (const TypeInfo* t = TypeRegistry::get().find("MaterialAsset")) SceneSerializer::applyFields(*t, &m, j);
    return m;
}

MaterialAsset::Json MaterialAsset::toJson() const
{
    Json j = Json::object();
    if (const TypeInfo* t = TypeRegistry::get().find("MaterialAsset")) j = SceneSerializer::serializeFields(*t, this);
    j["version"] = 1;
    return j;
}

void MaterialAsset::apply(StandardMaterial& mat, AssetManager& assets) const
{
    mat.setShader(assets.materialShader(mode, vertexShader, fragmentShader));
    mat.setBaseColor(baseColor)
        .setRoughness(roughness)
        .setMetallic(metallic)
        .setNormalStrength(normalStrength)
        .setAlphaCutoff(mode == StandardMaterial::Mode::Cutout ? alphaCutoff : 0.0f)
        .setEmissive(emissive, emissiveIntensity)
        .setUvScale(uvScale)
        .setVertexColorWeight(vertexColorWeight)
        .setDecalLayers(receiveDecals ? kDecalLayerAll : 0u)
        .setTranslucency(translucency, translucencyColor);

    // (Colour / data / normal is each image's own TextureSettings.)
    auto load = [&](const std::string& path) -> std::shared_ptr<Texture> {
        if (path.empty()) return nullptr;
        try {
            return assets.loadTexture(path);
        } catch (const std::exception& e) {
            log::warn("Material '{}': {}", mat.name, e.what());
            return nullptr;
        }
    };
    mat.setAlbedoTexture(load(albedo));
    mat.setNormalTexture(load(normal));
    mat.setMetallicRoughnessTexture(load(metallicRoughness));
    mat.setOcclusionTexture(load(occlusion));
    mat.setEmissiveTexture(load(emissiveTexture));
    mat.setTranslucencyTexture(load(translucencyTexture));

    // Extra inputs for custom shaders. Slots this file doesn't name are cleared,
    // so removing an entry (live edit) takes its texture / value away again.
    bool texSet[kMaterialTextureCount] = {};
    if (textures.is_array()) {
        for (const Json& e : textures) {
            const int slot = e.is_object() ? e.value("slot", -1) : -1;
            if (slot < 5 || slot >= int(StandardMaterial::kTranslucencyTexture)) {
                log::warn("Material '{}': extra texture slots are 5..{} (got {})", mat.name, StandardMaterial::kTranslucencyTexture - 1, slot);
                continue;
            }
            mat.setTexture(uint32_t(slot), load(e.value("path", std::string())));
            texSet[slot] = true;
        }
    }
    for (uint32_t s = 5; s < StandardMaterial::kTranslucencyTexture; ++s)
        if (!texSet[s]) mat.setTexture(s, nullptr);
    bool paramSet[kMaterialParamCount] = {};
    if (params.is_array()) {
        for (const Json& e : params) {
            const int slot = e.is_object() ? e.value("slot", -1) : -1;
            if (slot < 4 || slot >= int(StandardMaterial::kTranslucencyParam)) {
                log::warn("Material '{}': extra param slots are 4..{} (got {})", mat.name, StandardMaterial::kTranslucencyParam - 1, slot);
                continue;
            }
            glm::vec4 v(0.0f);
            if (const auto it = e.find("value"); it != e.end() && it->is_array())
                for (size_t i = 0; i < 4 && i < it->size(); ++i) v[int(i)] = (*it)[i].is_number() ? (*it)[i].get<float>() : 0.0f;
            mat.setVector(uint32_t(slot), v);
            paramSet[slot] = true;
        }
    }
    for (uint32_t s = 4; s < StandardMaterial::kTranslucencyParam; ++s)
        if (!paramSet[s]) mat.setVector(s, glm::vec4(0.0f));
}

void registerAssetTypes(TypeRegistry& r)
{
    r.add<MaterialAsset>("MaterialAsset", [](TypeBuilder<MaterialAsset>& t) {
        t.category("Assets");
        t.enumField("mode", &MaterialAsset::mode, {"Opaque", "Cutout", "Transparent"})
            .tooltip("Cutout: alpha test + double sided; Transparent: blended, no shadows");
        t.field("vertexShader", &MaterialAsset::vertexShader).tooltip("custom shader (empty = standard.vert)");
        t.field("fragmentShader", &MaterialAsset::fragmentShader).tooltip("custom shader with StandardMaterial's layout (empty = standard.frag)");
        t.field("baseColor", &MaterialAsset::baseColor).color();
        t.field("roughness", &MaterialAsset::roughness).range(0.0f, 1.0f).speed(0.01f);
        t.field("metallic", &MaterialAsset::metallic).range(0.0f, 1.0f).speed(0.01f);
        t.field("normalStrength", &MaterialAsset::normalStrength).range(0.0f, 4.0f).speed(0.01f);
        t.field("alphaCutoff", &MaterialAsset::alphaCutoff).range(0.0f, 1.0f).speed(0.01f).tooltip("Cutout mode");
        t.field("emissive", &MaterialAsset::emissive).color();
        t.field("emissiveIntensity", &MaterialAsset::emissiveIntensity).range(0.0f, 100.0f).speed(0.05f);
        t.field("uvScale", &MaterialAsset::uvScale);
        t.field("vertexColorWeight", &MaterialAsset::vertexColorWeight).range(0.0f, 1.0f).speed(0.01f)
            .tooltip("multiply albedo by the vertex colour (0 = ignore it)");
        t.field("receiveDecals", &MaterialAsset::receiveDecals);
        t.field("translucency", &MaterialAsset::translucency).range(0.0f, 1.0f).speed(0.01f)
            .tooltip("light through thin surfaces (leaves): the front reflects albedo x (1 - t), the back lets albedo x colour x t through");
        t.field("translucencyColor", &MaterialAsset::translucencyColor).color().tooltip("tint of the light passing through (linear)");
        const char* images = ".png .jpg .jpeg .tga";
        t.field("albedo", &MaterialAsset::albedo).assets(images).tooltip("sRGB colour, A = opacity");
        t.field("normal", &MaterialAsset::normal).assets(images).tooltip("tangent-space normal map (linear)");
        t.field("metallicRoughness", &MaterialAsset::metallicRoughness).assets(images).tooltip("G = roughness, B = metallic (linear)");
        t.field("occlusion", &MaterialAsset::occlusion).assets(images).tooltip("R = ambient occlusion (the ORM file again for a packed texture)");
        t.field("emissiveTexture", &MaterialAsset::emissiveTexture).assets(images);
        t.field("translucencyTexture", &MaterialAsset::translucencyTexture).assets(images).tooltip("R = where light gets through (linear)");
        t.property("textures", FieldKind::Json, [](const MaterialAsset& m) { return m.textures; },
                   [](MaterialAsset& m, const Json& j) { m.textures = j.is_array() ? j : Json::array(); })
            .tooltip(R"(custom shaders: [{"slot": 5, "path": "textures/x.png"}], slots 5-14)");
        t.property("params", FieldKind::Json, [](const MaterialAsset& m) { return m.params; },
                   [](MaterialAsset& m, const Json& j) { m.params = j.is_array() ? j : Json::array(); })
            .tooltip(R"(custom shaders: [{"slot": 4, "value": [1, 0, 0, 0]}], slots 4-10)");
    });
}

} // namespace ve
