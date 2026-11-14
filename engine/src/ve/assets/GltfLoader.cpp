// =============================================================================
// glTF 2.0 importer (via cgltf).
//
// glTF concepts -> engine concepts:
//   mesh (+ primitives)   -> Mesh with one Submesh per primitive
//   material              -> StandardMaterial (metallic-roughness PBR)
//   image/texture         -> Texture (sRGB for colour, linear for data)
//   node hierarchy        -> ModelNode tree (Scene::instantiate makes GameObjects)
// =============================================================================

#include "ve/assets/AssetManager.hpp"
#include "ve/assets/Model.hpp"
#include "ve/core/Log.hpp"
#include "ve/render/Mesh.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/render/Texture.hpp"

#include <cgltf.h>

#include <cstring>
#include <map>

namespace ve {

namespace {

struct GltfContext {
    Renderer& renderer;
    const cgltf_data* data;
    std::filesystem::path baseDir;
    Model& model;
    std::map<std::pair<const cgltf_image*, bool>, std::shared_ptr<Texture>> textureCache;
};

std::shared_ptr<Texture> loadImage(GltfContext& ctx, const cgltf_texture_view& view, bool srgb)
{
    if (!view.texture || !view.texture->image) return nullptr;
    const cgltf_image* image = view.texture->image;
    const auto key = std::make_pair(image, srgb);
    if (auto it = ctx.textureCache.find(key); it != ctx.textureCache.end()) return it->second;

    // glTF samplers: honour "nearest" filtering (Valheim-style pixel textures).
    SamplerType sampler = SamplerType::LinearRepeat;
    if (view.texture->sampler && view.texture->sampler->mag_filter == 9728 /* GL_NEAREST */) {
        sampler = SamplerType::NearestRepeat;
    }

    std::shared_ptr<Texture> tex;
    const std::string name = image->name ? image->name : (image->uri ? image->uri : "embedded");
    if (image->buffer_view) {
        // Embedded in a .glb or a buffer.
        const auto* bytes = static_cast<const uint8_t*>(image->buffer_view->buffer->data) + image->buffer_view->offset;
        tex = loadTextureMemory(ctx.renderer, bytes, image->buffer_view->size, srgb, true, sampler, name);
    } else if (image->uri && std::strncmp(image->uri, "data:", 5) == 0) {
        // data:image/png;base64,....
        const char* comma = std::strchr(image->uri, ',');
        if (!comma) return nullptr;
        const char* b64 = comma + 1;
        const size_t len = std::strlen(b64);
        size_t padding = 0;
        if (len >= 1 && b64[len - 1] == '=') ++padding;
        if (len >= 2 && b64[len - 2] == '=') ++padding;
        const size_t size = len / 4 * 3 - padding;
        void* decoded = nullptr;
        cgltf_options options{};
        if (cgltf_load_buffer_base64(&options, size, b64, &decoded) != cgltf_result_success) return nullptr;
        tex = loadTextureMemory(ctx.renderer, decoded, size, srgb, true, sampler, name);
        std::free(decoded);
    } else if (image->uri) {
        std::string uri = image->uri;
        cgltf_decode_uri(uri.data());
        uri.resize(std::strlen(uri.c_str()));
        tex = loadTextureFile(ctx.renderer, ctx.baseDir / uri, srgb, true, sampler);
    }
    if (tex) {
        ctx.textureCache[key] = tex;
        ctx.model.textures.push_back(tex);
    }
    return tex;
}

std::shared_ptr<Material> convertMaterial(GltfContext& ctx, const cgltf_material* m)
{
    using Mode = StandardMaterial::Mode;
    if (!m) return ctx.renderer.createStandardMaterial();

    Mode mode = Mode::Opaque;
    if (m->alpha_mode == cgltf_alpha_mode_blend) mode = Mode::Transparent;
    else if (m->alpha_mode == cgltf_alpha_mode_mask || m->double_sided) mode = Mode::Cutout; // cutout shader = no culling

    auto mat = ctx.renderer.createStandardMaterial(mode);
    mat->name = m->name ? m->name : "";
    mat->setAlphaCutoff(m->alpha_mode == cgltf_alpha_mode_mask ? m->alpha_cutoff : 0.0f);

    if (m->has_pbr_metallic_roughness) {
        const auto& pbr = m->pbr_metallic_roughness;
        mat->setBaseColor({pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2],
                           pbr.base_color_factor[3]});
        mat->setRoughness(pbr.roughness_factor);
        mat->setMetallic(pbr.metallic_factor);
        if (auto t = loadImage(ctx, pbr.base_color_texture, true)) mat->setAlbedoTexture(t);
        if (auto t = loadImage(ctx, pbr.metallic_roughness_texture, false)) mat->setMetallicRoughnessTexture(t);
    }
    if (auto t = loadImage(ctx, m->normal_texture, false)) {
        mat->setNormalTexture(t);
        mat->setNormalStrength(m->normal_texture.scale);
    }
    if (auto t = loadImage(ctx, m->occlusion_texture, false)) mat->setOcclusionTexture(t);
    const float emissiveStrength = m->has_emissive_strength ? m->emissive_strength.emissive_strength : 1.0f;
    mat->setEmissive({m->emissive_factor[0], m->emissive_factor[1], m->emissive_factor[2]}, emissiveStrength);
    if (auto t = loadImage(ctx, m->emissive_texture, true)) mat->setEmissiveTexture(t);
    return mat;
}

const cgltf_accessor* findAttribute(const cgltf_primitive& p, cgltf_attribute_type type, int index = 0)
{
    for (size_t i = 0; i < p.attributes_count; ++i) {
        if (p.attributes[i].type == type && p.attributes[i].index == index) return p.attributes[i].data;
    }
    return nullptr;
}

} // namespace

std::shared_ptr<Model> loadGltf(Renderer& renderer, const std::filesystem::path& file)
{
    cgltf_options options{};
    cgltf_data* data = nullptr;
    const std::string path = file.string();
    if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success) {
        throw std::runtime_error("Failed to parse glTF: " + path);
    }
    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) {
        cgltf_free(data);
        throw std::runtime_error("Failed to load glTF buffers: " + path);
    }

    auto model = std::make_shared<Model>();
    model->name = file.stem().string();
    GltfContext ctx{renderer, data, file.parent_path(), *model, {}};

    // Materials are shared between meshes in glTF; convert each once.
    std::vector<std::shared_ptr<Material>> materials(data->materials_count);
    for (size_t i = 0; i < data->materials_count; ++i) materials[i] = convertMaterial(ctx, &data->materials[i]);
    std::shared_ptr<Material> defaultMaterial;

    // --- Meshes ----------------------------------------------------------------
    for (size_t mi = 0; mi < data->meshes_count; ++mi) {
        const cgltf_mesh& gm = data->meshes[mi];
        MeshData md;
        std::vector<Submesh> submeshes;
        std::vector<std::shared_ptr<Material>> slots;
        bool needTangents = false;

        for (size_t pi = 0; pi < gm.primitives_count; ++pi) {
            const cgltf_primitive& prim = gm.primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) continue;
            const cgltf_accessor* pos = findAttribute(prim, cgltf_attribute_type_position);
            if (!pos) continue;
            const cgltf_accessor* nrm = findAttribute(prim, cgltf_attribute_type_normal);
            const cgltf_accessor* uv = findAttribute(prim, cgltf_attribute_type_texcoord);
            const cgltf_accessor* tan = findAttribute(prim, cgltf_attribute_type_tangent);
            const cgltf_accessor* col = findAttribute(prim, cgltf_attribute_type_color);

            const auto baseVertex = static_cast<uint32_t>(md.vertices.size());
            for (size_t v = 0; v < pos->count; ++v) {
                Vertex vert;
                cgltf_accessor_read_float(pos, v, &vert.position.x, 3);
                if (nrm) cgltf_accessor_read_float(nrm, v, &vert.normal.x, 3);
                if (uv) cgltf_accessor_read_float(uv, v, &vert.uv.x, 2);
                if (tan) cgltf_accessor_read_float(tan, v, &vert.tangent.x, 4);
                if (col) {
                    float c[4] = {1, 1, 1, 1};
                    cgltf_accessor_read_float(col, v, c, cgltf_num_components(col->type));
                    vert.color = {c[0], c[1], c[2], c[3]};
                }
                md.vertices.push_back(vert);
            }

            Submesh sm;
            sm.firstIndex = static_cast<uint32_t>(md.indices.size());
            if (prim.indices) {
                for (size_t i = 0; i < prim.indices->count; ++i) {
                    md.indices.push_back(baseVertex + static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i)));
                }
            } else {
                for (uint32_t i = 0; i < pos->count; ++i) md.indices.push_back(baseVertex + i);
            }
            sm.indexCount = static_cast<uint32_t>(md.indices.size()) - sm.firstIndex;
            sm.materialSlot = static_cast<uint32_t>(slots.size());

            std::shared_ptr<Material> mat;
            if (prim.material) {
                mat = materials[static_cast<size_t>(prim.material - data->materials)];
                if (prim.material->normal_texture.texture && !tan) needTangents = true;
            } else {
                if (!defaultMaterial) defaultMaterial = renderer.createStandardMaterial();
                mat = defaultMaterial;
            }
            slots.push_back(mat);
            submeshes.push_back(sm);

            if (!nrm) {
                // Flat normals for this primitive's triangles.
                MeshData part;
                part.vertices.assign(md.vertices.begin() + baseVertex, md.vertices.end());
                for (uint32_t i = sm.firstIndex; i < sm.firstIndex + sm.indexCount; ++i) {
                    part.indices.push_back(md.indices[i] - baseVertex);
                }
                part.computeNormals();
                std::copy(part.vertices.begin(), part.vertices.end(), md.vertices.begin() + baseVertex);
            }
        }
        if (md.indices.empty()) {
            model->meshes.push_back(nullptr);
            model->meshMaterials.push_back({});
            model->meshData.emplace_back();
            continue;
        }
        if (needTangents) md.computeTangents();
        model->meshData.push_back(md);
        model->meshes.push_back(Mesh::create(renderer, md, submeshes, gm.name ? gm.name : model->name));
        model->meshMaterials.push_back(std::move(slots));
    }

    // --- Nodes -----------------------------------------------------------------
    model->nodes.resize(data->nodes_count);
    for (size_t ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& gn = data->nodes[ni];
        ModelNode& node = model->nodes[ni];
        node.name = gn.name ? gn.name : "";
        cgltf_node_transform_local(&gn, &node.local[0][0]);
        node.mesh = gn.mesh ? static_cast<int>(gn.mesh - data->meshes) : -1;
        if (node.mesh >= 0 && !model->meshes[node.mesh]) node.mesh = -1;
        for (size_t c = 0; c < gn.children_count; ++c) {
            node.children.push_back(static_cast<int>(gn.children[c] - data->nodes));
        }
    }
    const cgltf_scene* scene = data->scene ? data->scene : (data->scenes_count ? &data->scenes[0] : nullptr);
    if (scene) {
        for (size_t i = 0; i < scene->nodes_count; ++i) model->roots.push_back(static_cast<int>(scene->nodes[i] - data->nodes));
    } else {
        for (size_t ni = 0; ni < data->nodes_count; ++ni) {
            if (!data->nodes[ni].parent) model->roots.push_back(static_cast<int>(ni));
        }
    }

    cgltf_free(data);
    return model;
}

} // namespace ve
