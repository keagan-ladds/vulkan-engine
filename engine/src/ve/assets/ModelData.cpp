#include "ve/assets/ModelData.hpp"

#include "ve/assets/AssetManager.hpp"
#include "ve/assets/TextureSettings.hpp"
#include "ve/core/FileSystem.hpp"
#include "ve/core/Log.hpp"
#include "ve/render/Material.hpp"
#include "ve/render/Renderer.hpp"
#include "ve/render/Skinning.hpp"

#include <cgltf.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace ve {

// -----------------------------------------------------------------------------
// ModelSettings
// -----------------------------------------------------------------------------

std::filesystem::path ModelSettings::settingsFile(const std::filesystem::path& modelFile)
{
    std::filesystem::path f = modelFile;
    f += ".model";
    return f;
}

ModelSettings ModelSettings::forModel(const std::filesystem::path& modelFile)
{
    ModelSettings s;
    const auto bytes = vfs::read(settingsFile(modelFile));
    if (!bytes) return s;
    const nlohmann::json j = nlohmann::json::parse(bytes->begin(), bytes->end(), nullptr, false);
    if (!j.is_object()) {
        log::warnOnce("Model settings " + settingsFile(modelFile).string() + ": not a JSON object (using defaults)");
        return s;
    }
    s.applyJson(j);
    return s;
}

nlohmann::json ModelSettings::toJson() const
{
    nlohmann::json j = {{"autoLods", autoLods},
                        {"lodRatio", lodRatio},
                        {"lodMaxError", lodMaxError},
                        {"lodFaceted", lodFaceted},
                        {"impostor", impostor},
                        {"impostorFrames", impostorFrames},
                        {"impostorResolution", impostorResolution},
                        {"impostorAlphaCutoff", impostorAlphaCutoff},
                        {"impostorBackground", {impostorBackground.r, impostorBackground.g, impostorBackground.b}},
                        {"materials", nlohmann::json::object()}};
    for (const auto& [slot, file] : materials) j["materials"][slot] = file;
    return j;
}

void ModelSettings::applyJson(const nlohmann::json& j)
{
    autoLods = std::clamp(j.value("autoLods", autoLods), 0, 3);
    lodRatio = std::clamp(j.value("lodRatio", lodRatio), 0.01f, 1.0f);
    lodMaxError = std::max(j.value("lodMaxError", lodMaxError), 0.0f);
    lodFaceted = j.value("lodFaceted", lodFaceted);
    impostor = j.value("impostor", impostor);
    impostorFrames = std::clamp(j.value("impostorFrames", impostorFrames), 2u, 32u);
    impostorResolution = std::clamp(j.value("impostorResolution", impostorResolution), 16u, 512u);
    impostorAlphaCutoff = std::clamp(j.value("impostorAlphaCutoff", impostorAlphaCutoff), 0.0f, 1.0f);
    if (auto it = j.find("impostorBackground"); it != j.end() && it->is_array() && it->size() == 3)
        impostorBackground = {(*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>()};
    if (auto it = j.find("materials"); it != j.end() && it->is_object())
        for (auto m = it->begin(); m != it->end(); ++m)
            if (m.value().is_string()) materials[m.key()] = m.value().get<std::string>();
}

uint64_t ModelSettings::cookHash() const
{
    const std::string s = toJson().dump();
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

// -----------------------------------------------------------------------------
// Binary form
// -----------------------------------------------------------------------------

namespace {

void writeTexture(BinaryWriter& w, const ModelTextureRef& t)
{
    w.putString(t.uri);
    w.put<int32_t>(t.embedded);
    w.put<uint8_t>(t.nearest ? 1 : 0);
}

ModelTextureRef readTexture(BinaryReader& r)
{
    ModelTextureRef t;
    t.uri = r.getString();
    t.embedded = r.get<int32_t>();
    t.nearest = r.get<uint8_t>() != 0;
    return t;
}

void writeSubmeshes(BinaryWriter& w, const std::vector<Submesh>& submeshes)
{
    w.put<uint64_t>(submeshes.size());
    for (const Submesh& s : submeshes) {
        w.put(s.firstIndex);
        w.put(s.indexCount);
        w.put(s.materialSlot);
        w.put(s.bounds.min);
        w.put(s.bounds.max);
    }
}

std::vector<Submesh> readSubmeshes(BinaryReader& r)
{
    std::vector<Submesh> out(std::min<uint64_t>(r.get<uint64_t>(), r.remaining() / 36 + 1));
    for (Submesh& s : out) {
        s.firstIndex = r.get<uint32_t>();
        s.indexCount = r.get<uint32_t>();
        s.materialSlot = r.get<uint32_t>();
        s.bounds.min = r.get<glm::vec3>();
        s.bounds.max = r.get<glm::vec3>();
    }
    return out;
}

template <class T, class F>
void writeList(BinaryWriter& w, const std::vector<T>& list, F&& each)
{
    w.put<uint64_t>(list.size());
    for (const T& item : list) each(item);
}

template <class T, class F>
std::vector<T> readList(BinaryReader& r, F&& each)
{
    const uint64_t n = r.get<uint64_t>();
    std::vector<T> out;
    for (uint64_t i = 0; i < n && r.ok(); ++i) out.push_back(each());
    return out;
}

} // namespace

void ModelData::write(BinaryWriter& w) const
{
    w.putString(name);
    writeList(w, meshes, [&](const ModelMeshData& m) {
        w.putString(m.name);
        m.data.write(w);
        writeSubmeshes(w, m.submeshes);
        w.putVector(m.materials);
        w.putVector(m.joints);
        w.putVector(m.weights);
        w.putVector(m.jointRadius);
    });
    writeList(w, materials, [&](const ModelMaterialData& m) {
        w.putString(m.name);
        w.putString(m.extras);
        w.putString(m.file);
        w.put<int32_t>(m.mode);
        w.put(m.alphaCutoff);
        w.put(m.baseColor);
        w.put(m.roughness);
        w.put(m.metallic);
        w.put(m.normalScale);
        w.put(m.emissive);
        w.put(m.emissiveStrength);
        for (const ModelTextureRef* t : {&m.albedo, &m.metallicRoughness, &m.normal, &m.occlusion, &m.emissiveTexture}) writeTexture(w, *t);
    });
    writeList(w, images, [&](const ModelImageData& i) {
        w.putString(i.name);
        w.putVector(i.bytes);
    });
    writeList(w, nodes, [&](const ModelNode& n) {
        w.putString(n.name);
        w.put(n.local);
        w.put<int32_t>(n.mesh);
        w.put<int32_t>(n.skin);
        w.putVector(n.children);
        w.putString(n.extras);
    });
    w.putVector(roots);
    writeList(w, skins, [&](const ModelSkin& s) {
        w.putString(s.name);
        w.putVector(s.joints);
        w.putVector(s.inverseBind);
    });
    writeList(w, animations, [&](const AnimationClip& a) {
        w.putString(a.name);
        w.put(a.duration);
        writeList(w, a.channels, [&](const AnimationChannel& c) {
            w.put<int32_t>(c.node);
            w.put<int32_t>(int32_t(c.path));
            w.put<int32_t>(int32_t(c.interpolation));
            w.putVector(c.times);
            w.putVector(c.values);
        });
    });
    writeList(w, lods, [&](const ModelVariantLods& v) {
        w.putString(v.variant);
        writeList(w, v.generated, [&](const ModelLodLevelData& l) {
            l.data.write(w);
            writeSubmeshes(w, l.submeshes);
        });
        const ModelImpostorData& i = v.impostor;
        w.put(i.albedoFormat);
        w.put(i.normalFormat);
        w.put(i.size);
        w.putVector(i.levelOffsets);
        w.putVector(i.albedo);
        w.putVector(i.normal);
        w.put(i.centre);
        w.put(i.radius);
        w.put(i.frames);
        w.put(i.alphaCutoff);
    });
    writeList(w, dependencies, [&](const std::string& d) { w.putString(d); });
}

bool ModelData::read(BinaryReader& r)
{
    name = r.getString();
    meshes = readList<ModelMeshData>(r, [&] {
        ModelMeshData m;
        m.name = r.getString();
        m.data.read(r);
        m.submeshes = readSubmeshes(r);
        m.materials = r.getVector<int>();
        m.joints = r.getVector<glm::uvec4>();
        m.weights = r.getVector<glm::vec4>();
        m.jointRadius = r.getVector<float>();
        return m;
    });
    materials = readList<ModelMaterialData>(r, [&] {
        ModelMaterialData m;
        m.name = r.getString();
        m.extras = r.getString();
        m.file = r.getString();
        m.mode = r.get<int32_t>();
        m.alphaCutoff = r.get<float>();
        m.baseColor = r.get<glm::vec4>();
        m.roughness = r.get<float>();
        m.metallic = r.get<float>();
        m.normalScale = r.get<float>();
        m.emissive = r.get<glm::vec3>();
        m.emissiveStrength = r.get<float>();
        for (ModelTextureRef* t : {&m.albedo, &m.metallicRoughness, &m.normal, &m.occlusion, &m.emissiveTexture}) *t = readTexture(r);
        return m;
    });
    images = readList<ModelImageData>(r, [&] {
        ModelImageData i;
        i.name = r.getString();
        i.bytes = r.getVector<uint8_t>();
        return i;
    });
    nodes = readList<ModelNode>(r, [&] {
        ModelNode n;
        n.name = r.getString();
        n.local = r.get<glm::mat4>();
        n.mesh = r.get<int32_t>();
        n.skin = r.get<int32_t>();
        n.children = r.getVector<int>();
        n.extras = r.getString();
        return n;
    });
    roots = r.getVector<int>();
    skins = readList<ModelSkin>(r, [&] {
        ModelSkin s;
        s.name = r.getString();
        s.joints = r.getVector<int>();
        s.inverseBind = r.getVector<glm::mat4>();
        return s;
    });
    animations = readList<AnimationClip>(r, [&] {
        AnimationClip a;
        a.name = r.getString();
        a.duration = r.get<float>();
        a.channels = readList<AnimationChannel>(r, [&] {
            AnimationChannel c;
            c.node = r.get<int32_t>();
            c.path = AnimationChannel::Path(r.get<int32_t>());
            c.interpolation = AnimationChannel::Interpolation(r.get<int32_t>());
            c.times = r.getVector<float>();
            c.values = r.getVector<glm::vec4>();
            return c;
        });
        return a;
    });
    lods = readList<ModelVariantLods>(r, [&] {
        ModelVariantLods v;
        v.variant = r.getString();
        v.generated = readList<ModelLodLevelData>(r, [&] {
            ModelLodLevelData l;
            l.data.read(r);
            l.submeshes = readSubmeshes(r);
            return l;
        });
        ModelImpostorData& i = v.impostor;
        i.albedoFormat = r.get<uint32_t>();
        i.normalFormat = r.get<uint32_t>();
        i.size = r.get<uint32_t>();
        i.levelOffsets = r.getVector<uint64_t>();
        i.albedo = r.getVector<uint8_t>();
        i.normal = r.getVector<uint8_t>();
        i.centre = r.get<glm::vec3>();
        i.radius = r.get<float>();
        i.frames = r.get<uint32_t>();
        i.alphaCutoff = r.get<float>();
        return v;
    });
    dependencies = readList<std::string>(r, [&] { return r.getString(); });
    return r.ok();
}

// -----------------------------------------------------------------------------
// glTF -> ModelData
// -----------------------------------------------------------------------------

namespace {

const cgltf_accessor* findAttribute(const cgltf_primitive& p, cgltf_attribute_type type, int index = 0)
{
    for (size_t i = 0; i < p.attributes_count; ++i) {
        if (p.attributes[i].type == type && p.attributes[i].index == index) return p.attributes[i].data;
    }
    return nullptr;
}

// cgltf file callbacks through the virtual file system (external .bin
// buffers of a .gltf, also inside a packaged game's archive).
cgltf_result vfsReadFile(const cgltf_memory_options*, const cgltf_file_options*, const char* path, cgltf_size* size, void** data)
{
    auto bytes = vfs::read(std::filesystem::path(path));
    if (!bytes) return cgltf_result_file_not_found;
    void* copy = std::malloc(std::max<size_t>(bytes->size(), 1));
    if (!copy) return cgltf_result_out_of_memory;
    std::memcpy(copy, bytes->data(), bytes->size());
    *size = bytes->size();
    *data = copy;
    return cgltf_result_success;
}

void vfsReleaseFile(const cgltf_memory_options*, const cgltf_file_options*, void* data)
{
    std::free(data);
}

std::string decodedUri(const char* uri)
{
    std::string u = uri;
    cgltf_decode_uri(u.data());
    u.resize(std::strlen(u.c_str()));
    return u;
}

} // namespace

ModelData parseGltf(const std::filesystem::path& file)
{
    cgltf_options options{};
    options.file.read = vfsReadFile;
    options.file.release = vfsReleaseFile;
    cgltf_data* data = nullptr;
    const std::string path = file.string();
    // The file's bytes stay alive until cgltf_free (a .glb's binary chunk points into them).
    const auto bytes = vfs::read(file);
    if (!bytes) throw std::runtime_error("Can't read " + path);
    if (cgltf_parse(&options, bytes->data(), bytes->size(), &data) != cgltf_result_success)
        throw std::runtime_error("Failed to parse glTF: " + path);
    const std::unique_ptr<cgltf_data, void (*)(cgltf_data*)> guard(data, cgltf_free); // freed on every way out
    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success)
        throw std::runtime_error("Failed to load glTF buffers: " + path);

    ModelData out;
    out.name = file.stem().string();
    for (size_t i = 0; i < data->buffers_count; ++i) {
        const char* uri = data->buffers[i].uri;
        if (uri && std::strncmp(uri, "data:", 5) != 0) out.dependencies.push_back(decodedUri(uri));
    }

    // --- Images: embedded ones are copied in, files stay references ----------------
    std::vector<int> imageIndex(data->images_count, -1);
    auto textureRef = [&](const cgltf_texture_view& view) {
        ModelTextureRef t;
        if (!view.texture || !view.texture->image) return t;
        const cgltf_image* image = view.texture->image;
        t.nearest = view.texture->sampler && view.texture->sampler->mag_filter == 9728; // GL_NEAREST
        const size_t ii = size_t(image - data->images);
        if (image->buffer_view || (image->uri && std::strncmp(image->uri, "data:", 5) == 0)) {
            if (imageIndex[ii] < 0) {
                ModelImageData img;
                img.name = image->name ? image->name : "embedded";
                if (image->buffer_view) {
                    const auto* p = static_cast<const uint8_t*>(image->buffer_view->buffer->data) + image->buffer_view->offset;
                    img.bytes.assign(p, p + image->buffer_view->size);
                } else {
                    const char* comma = std::strchr(image->uri, ',');
                    if (!comma) return t;
                    const char* b64 = comma + 1;
                    const size_t len = std::strlen(b64);
                    size_t padding = 0;
                    if (len >= 1 && b64[len - 1] == '=') ++padding;
                    if (len >= 2 && b64[len - 2] == '=') ++padding;
                    const size_t size = len / 4 * 3 - padding;
                    void* decoded = nullptr;
                    cgltf_options o{};
                    if (cgltf_load_buffer_base64(&o, size, b64, &decoded) != cgltf_result_success) return t;
                    img.bytes.assign(static_cast<uint8_t*>(decoded), static_cast<uint8_t*>(decoded) + size);
                    std::free(decoded);
                }
                imageIndex[ii] = int(out.images.size());
                out.images.push_back(std::move(img));
            }
            t.embedded = imageIndex[ii];
        } else if (image->uri) {
            t.uri = decodedUri(image->uri);
        }
        return t;
    };

    // --- Materials ------------------------------------------------------------------------
    for (size_t i = 0; i < data->materials_count; ++i) {
        const cgltf_material& m = data->materials[i];
        ModelMaterialData md;
        md.name = m.name ? m.name : "";
        if (m.extras.data) md.extras = m.extras.data;
        md.mode = m.alpha_mode == cgltf_alpha_mode_blend ? 2 : (m.alpha_mode == cgltf_alpha_mode_mask || m.double_sided) ? 1 : 0;
        md.alphaCutoff = m.alpha_mode == cgltf_alpha_mode_mask ? m.alpha_cutoff : 0.0f;
        if (m.has_pbr_metallic_roughness) {
            const auto& pbr = m.pbr_metallic_roughness;
            md.baseColor = {pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2], pbr.base_color_factor[3]};
            md.roughness = pbr.roughness_factor;
            md.metallic = pbr.metallic_factor;
            md.albedo = textureRef(pbr.base_color_texture);
            md.metallicRoughness = textureRef(pbr.metallic_roughness_texture);
        }
        md.normal = textureRef(m.normal_texture);
        md.normalScale = m.normal_texture.texture ? m.normal_texture.scale : 1.0f;
        md.occlusion = textureRef(m.occlusion_texture);
        md.emissive = {m.emissive_factor[0], m.emissive_factor[1], m.emissive_factor[2]};
        md.emissiveStrength = m.has_emissive_strength ? m.emissive_strength.emissive_strength : 1.0f;
        md.emissiveTexture = textureRef(m.emissive_texture);
        out.materials.push_back(std::move(md));
    }

    // --- Meshes -----------------------------------------------------------------------------
    for (size_t mi = 0; mi < data->meshes_count; ++mi) {
        const cgltf_mesh& gm = data->meshes[mi];
        ModelMeshData mesh;
        mesh.name = gm.name ? gm.name : out.name;
        MeshData& md = mesh.data;
        bool needTangents = false;
        bool skinned = false;

        for (size_t pi = 0; pi < gm.primitives_count; ++pi) {
            const cgltf_primitive& prim = gm.primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) continue;
            const cgltf_accessor* pos = findAttribute(prim, cgltf_attribute_type_position);
            if (!pos) continue;
            const cgltf_accessor* nrm = findAttribute(prim, cgltf_attribute_type_normal);
            const cgltf_accessor* uv = findAttribute(prim, cgltf_attribute_type_texcoord);
            const cgltf_accessor* tan = findAttribute(prim, cgltf_attribute_type_tangent);
            const cgltf_accessor* col = findAttribute(prim, cgltf_attribute_type_color);
            const cgltf_accessor* joints = findAttribute(prim, cgltf_attribute_type_joints);
            const cgltf_accessor* weights = findAttribute(prim, cgltf_attribute_type_weights);

            const auto baseVertex = static_cast<uint32_t>(md.vertices.size());
            if (joints && weights) {
                skinned = true;
                mesh.joints.resize(baseVertex, glm::uvec4(0));
                mesh.weights.resize(baseVertex, glm::vec4(1, 0, 0, 0));
                for (size_t v = 0; v < pos->count; ++v) {
                    cgltf_uint j[4] = {0, 0, 0, 0};
                    float w[4] = {1, 0, 0, 0};
                    cgltf_accessor_read_uint(joints, v, j, 4);
                    cgltf_accessor_read_float(weights, v, w, 4);
                    const float sum = w[0] + w[1] + w[2] + w[3];
                    mesh.joints.push_back({j[0], j[1], j[2], j[3]});
                    mesh.weights.push_back(sum > 0.0f ? glm::vec4(w[0], w[1], w[2], w[3]) / sum : glm::vec4(1, 0, 0, 0));
                }
            } else if (skinned) {
                mesh.joints.resize(baseVertex + pos->count, glm::uvec4(0));
                mesh.weights.resize(baseVertex + pos->count, glm::vec4(1, 0, 0, 0));
            }
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
                for (size_t i = 0; i < prim.indices->count; ++i)
                    md.indices.push_back(baseVertex + static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i)));
            } else {
                for (uint32_t i = 0; i < pos->count; ++i) md.indices.push_back(baseVertex + i);
            }
            sm.indexCount = static_cast<uint32_t>(md.indices.size()) - sm.firstIndex;
            sm.materialSlot = static_cast<uint32_t>(mesh.materials.size());
            mesh.materials.push_back(prim.material ? int(prim.material - data->materials) : -1);
            mesh.submeshes.push_back(sm);
            // Tangents (normal mapping) whenever the file has UVs but none: the
            // material may get a normal map from elsewhere (a material file).
            if (uv && !tan) needTangents = true;
            if (!nrm) {
                // Flat normals for this primitive's triangles.
                MeshData part;
                part.vertices.assign(md.vertices.begin() + baseVertex, md.vertices.end());
                for (uint32_t i = sm.firstIndex; i < sm.firstIndex + sm.indexCount; ++i) part.indices.push_back(md.indices[i] - baseVertex);
                part.computeNormals();
                std::copy(part.vertices.begin(), part.vertices.end(), md.vertices.begin() + baseVertex);
            }
        }
        if (!md.indices.empty() && needTangents) md.computeTangents();
        if (skinned) {
            mesh.joints.resize(md.vertices.size(), glm::uvec4(0));
            mesh.weights.resize(md.vertices.size(), glm::vec4(1, 0, 0, 0));
        }
        out.meshes.push_back(std::move(mesh));
    }

    // --- Nodes -------------------------------------------------------------------------------
    out.nodes.resize(data->nodes_count);
    for (size_t ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& gn = data->nodes[ni];
        ModelNode& node = out.nodes[ni];
        node.name = gn.name ? gn.name : "";
        cgltf_node_transform_local(&gn, &node.local[0][0]);
        node.mesh = gn.mesh ? static_cast<int>(gn.mesh - data->meshes) : -1;
        if (node.mesh >= 0 && out.meshes[size_t(node.mesh)].empty()) node.mesh = -1;
        node.skin = gn.skin && node.mesh >= 0 && out.meshes[size_t(node.mesh)].skinned() ? static_cast<int>(gn.skin - data->skins) : -1;
        for (size_t c = 0; c < gn.children_count; ++c) node.children.push_back(static_cast<int>(gn.children[c] - data->nodes));
        if (gn.extras.data) node.extras = gn.extras.data; // marker nodes read their fields from it
    }
    const cgltf_scene* scene = data->scene ? data->scene : (data->scenes_count ? &data->scenes[0] : nullptr);
    if (scene) {
        for (size_t i = 0; i < scene->nodes_count; ++i) out.roots.push_back(static_cast<int>(scene->nodes[i] - data->nodes));
    } else {
        for (size_t ni = 0; ni < data->nodes_count; ++ni)
            if (!data->nodes[ni].parent) out.roots.push_back(static_cast<int>(ni));
    }

    // --- Skins -------------------------------------------------------------------------------
    for (size_t si = 0; si < data->skins_count; ++si) {
        const cgltf_skin& gs = data->skins[si];
        ModelSkin skin;
        skin.name = gs.name ? gs.name : "";
        for (size_t j = 0; j < gs.joints_count; ++j) {
            skin.joints.push_back(static_cast<int>(gs.joints[j] - data->nodes));
            glm::mat4 ibm(1.0f);
            if (gs.inverse_bind_matrices) cgltf_accessor_read_float(gs.inverse_bind_matrices, j, &ibm[0][0], 16);
            skin.inverseBind.push_back(ibm);
        }
        out.skins.push_back(std::move(skin));
    }
    // Bounds padding per joint: how far its vertices reach from it (in the bind pose).
    for (const ModelNode& node : out.nodes) {
        if (node.skin < 0) continue;
        ModelMeshData& m = out.meshes[size_t(node.mesh)];
        const ModelSkin& skin = out.skins[size_t(node.skin)];
        m.jointRadius.assign(skin.joints.size(), 0.0f);
        for (size_t v = 0; v < m.data.vertices.size(); ++v) {
            for (int k = 0; k < 4; ++k) {
                const uint32_t j = m.joints[v][k];
                if (m.weights[v][k] <= 0.0f || j >= skin.joints.size()) continue;
                const glm::vec3 local = glm::vec3(skin.inverseBind[j] * glm::vec4(m.data.vertices[v].position, 1.0f));
                m.jointRadius[j] = std::max(m.jointRadius[j], glm::length(local));
            }
        }
    }

    // --- Animations --------------------------------------------------------------------------
    for (size_t ai = 0; ai < data->animations_count; ++ai) {
        const cgltf_animation& ga = data->animations[ai];
        AnimationClip clip;
        clip.name = ga.name ? ga.name : "Animation " + std::to_string(ai);
        for (size_t ci = 0; ci < ga.channels_count; ++ci) {
            const cgltf_animation_channel& gc = ga.channels[ci];
            if (!gc.target_node || !gc.sampler) continue;
            AnimationChannel ch;
            ch.node = static_cast<int>(gc.target_node - data->nodes);
            if (gc.target_path == cgltf_animation_path_type_translation) ch.path = AnimationChannel::Path::Translation;
            else if (gc.target_path == cgltf_animation_path_type_rotation) ch.path = AnimationChannel::Path::Rotation;
            else if (gc.target_path == cgltf_animation_path_type_scale) ch.path = AnimationChannel::Path::Scale;
            else continue; // morph weights: not supported
            const cgltf_animation_sampler& gsmp = *gc.sampler;
            ch.interpolation = gsmp.interpolation == cgltf_interpolation_type_step           ? AnimationChannel::Interpolation::Step
                               : gsmp.interpolation == cgltf_interpolation_type_cubic_spline ? AnimationChannel::Interpolation::CubicSpline
                                                                                             : AnimationChannel::Interpolation::Linear;
            ch.times.resize(gsmp.input->count);
            for (size_t k = 0; k < gsmp.input->count; ++k) cgltf_accessor_read_float(gsmp.input, k, &ch.times[k], 1);
            const int comps = ch.path == AnimationChannel::Path::Rotation ? 4 : 3;
            ch.values.resize(gsmp.output->count, glm::vec4(0.0f));
            for (size_t k = 0; k < gsmp.output->count; ++k) cgltf_accessor_read_float(gsmp.output, k, &ch.values[k].x, comps);
            if (!ch.times.empty()) clip.duration = std::max(clip.duration, ch.times.back());
            clip.channels.push_back(std::move(ch));
        }
        out.animations.push_back(std::move(clip));
    }
    return out;
}

// -----------------------------------------------------------------------------
// ModelData -> Model (GPU)
// -----------------------------------------------------------------------------

namespace {

struct BuildContext {
    Renderer& renderer;
    const ModelData& data;
    AssetManager* assets;
    std::filesystem::path folder; // the model's folder: texture URIs are relative to it
    Model& model;
    std::map<std::pair<int, bool>, std::shared_ptr<Texture>> embeddedCache;
};

std::shared_ptr<Texture> loadTexture(BuildContext& ctx, const ModelTextureRef& ref, TextureType role)
{
    if (!ref.valid()) return nullptr;
    const bool srgb = role == TextureType::Color; // (embedded images: the role decides; files: their settings)
    std::shared_ptr<Texture> tex;
    if (ref.embedded >= 0 && size_t(ref.embedded) < ctx.data.images.size()) {
        const auto key = std::make_pair(ref.embedded, srgb);
        if (auto it = ctx.embeddedCache.find(key); it != ctx.embeddedCache.end()) return it->second;
        const ModelImageData& img = ctx.data.images[size_t(ref.embedded)];
        tex = loadTextureMemory(ctx.renderer, img.bytes.data(), img.bytes.size(), srgb, true,
                                ref.nearest ? SamplerType::NearestRepeat : SamplerType::LinearRepeat, img.name);
        ctx.embeddedCache[key] = tex;
    } else {
        // Through the asset cache when there is one: every model pointing at
        // this file shares one texture (and its cooked copy is used).
        const std::filesystem::path file = (ctx.folder / ref.uri).lexically_normal();
        if (ctx.assets) {
            tex = ctx.assets->loadTexture(file);
            const TextureType type = ctx.assets->textureSettings(file).type;
            if (type != role)
                log::warnOnce(std::string("Texture ") + file.generic_string() + " is used as a " +
                              (role == TextureType::Normal ? "normal map" : role == TextureType::Data ? "data texture" : "colour texture") +
                              " by " + ctx.model.name + " but loads as " + toString(type) + ": add " +
                              TextureSettings::settingsFile(file).generic_string() + " with {\"type\": \"" + toString(role) +
                              "\"} or rename the image (e.g. *_normal, *_orm)");
        } else {
            tex = loadTextureFile(ctx.renderer, file, srgb, true, ref.nearest ? SamplerType::NearestRepeat : SamplerType::LinearRepeat);
        }
    }
    if (tex) ctx.model.textures.push_back(tex);
    return tex;
}

std::shared_ptr<Material> buildMaterial(BuildContext& ctx, const ModelMaterialData& m, const ModelSettings& settings,
                                        std::vector<std::filesystem::path>* dependencies)
{
    using Mode = StandardMaterial::Mode;
    if (ctx.assets) {
        // 1. The .model file's remap (decided when cooked, or from the settings),
        // 2. the "material" custom property / a file named like it.
        std::string remap = m.file;
        if (auto it = settings.materials.find(m.name); remap.empty() && it != settings.materials.end()) remap = it->second;
        if (!remap.empty()) {
            try {
                auto mat = ctx.assets->loadMaterial(remap);
                if (dependencies) dependencies->push_back(remap);
                return mat;
            } catch (const std::exception& e) {
                log::warnOnce(ctx.model.name + ": material '" + m.name + "' -> " + remap + ": " + e.what());
            }
        }
        if (auto shared = ctx.assets->materialForGltf(m.name, m.extras)) {
            if (dependencies) dependencies->push_back(ctx.assets->materialFileForGltf(m.name, m.extras));
            return shared;
        }
    }
    auto mat = ctx.renderer.createStandardMaterial(m.mode == 2 ? Mode::Transparent : m.mode == 1 ? Mode::Cutout : Mode::Opaque);
    mat->name = m.name;
    mat->setAlphaCutoff(m.alphaCutoff);
    mat->setBaseColor(m.baseColor);
    mat->setRoughness(m.roughness);
    mat->setMetallic(m.metallic);
    if (auto t = loadTexture(ctx, m.albedo, TextureType::Color)) mat->setAlbedoTexture(t);
    if (auto t = loadTexture(ctx, m.metallicRoughness, TextureType::Data)) mat->setMetallicRoughnessTexture(t);
    if (auto t = loadTexture(ctx, m.normal, TextureType::Normal)) {
        mat->setNormalTexture(t);
        mat->setNormalStrength(m.normalScale);
    }
    if (auto t = loadTexture(ctx, m.occlusion, TextureType::Data)) mat->setOcclusionTexture(t);
    mat->setEmissive(m.emissive, m.emissiveStrength);
    if (auto t = loadTexture(ctx, m.emissiveTexture, TextureType::Color)) mat->setEmissiveTexture(t);
    return mat;
}

} // namespace

std::shared_ptr<Model> buildModel(Renderer& renderer, const ModelData& data, AssetManager* assets, const std::filesystem::path& modelPath,
                                  const ModelSettings& settings, std::vector<std::filesystem::path>* dependencies)
{
    auto model = std::make_shared<Model>();
    model->name = data.name;
    BuildContext ctx{renderer, data, assets, modelPath.parent_path(), *model, {}};

    for (const ModelMaterialData& m : data.materials) model->materials.push_back(buildMaterial(ctx, m, settings, dependencies));
    std::shared_ptr<Material> defaultMaterial;

    for (const ModelMeshData& m : data.meshes) {
        if (m.empty()) {
            model->meshes.push_back(nullptr);
            model->meshMaterials.push_back({});
            model->meshMaterialIds.push_back({});
            model->meshData.emplace_back();
            model->skinnedMeshes.push_back(nullptr);
            continue;
        }
        std::vector<std::shared_ptr<Material>> slots;
        for (int id : m.materials) {
            if (id >= 0 && size_t(id) < model->materials.size()) {
                slots.push_back(model->materials[size_t(id)]);
            } else {
                if (!defaultMaterial) defaultMaterial = renderer.createStandardMaterial();
                slots.push_back(defaultMaterial);
            }
        }
        model->meshes.push_back(Mesh::create(renderer, m.data, m.submeshes, m.name));
        std::shared_ptr<SkinnedMeshSource> skin;
        if (m.skinned()) {
            skin = std::make_shared<SkinnedMeshSource>();
            skin->data = m.data;
            skin->submeshes = m.submeshes;
            skin->joints = m.joints;
            skin->weights = m.weights;
            skin->jointRadius = m.jointRadius;
            skin->name = m.name;
        }
        model->meshData.push_back(m.data);
        model->meshMaterials.push_back(std::move(slots));
        model->meshMaterialIds.push_back(m.materials);
        model->skinnedMeshes.push_back(std::move(skin));
    }
    model->nodes = data.nodes;
    model->roots = data.roots;
    model->skins = data.skins;
    for (const AnimationClip& a : data.animations) model->animations.push_back(std::make_shared<const AnimationClip>(a));
    if (!data.lods.empty()) model->cookedLods = std::make_shared<const std::vector<ModelVariantLods>>(data.lods);
    return model;
}

} // namespace ve
