#include "ve/world/LodModel.hpp"

#include "ve/assets/AssetManager.hpp"
#include "ve/assets/Model.hpp"
#include "ve/core/Strings.hpp"
#include "ve/render/Impostor.hpp"
#include "ve/render/Material.hpp"
#include "ve/render/Renderer.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <stdexcept>

namespace ve {

namespace {

std::string lower(std::string s)
{
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Drops Blender's duplicate suffix (".001").
std::string stripDuplicate(const std::string& name)
{
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot + 1 == name.size()) return name;
    for (size_t i = dot + 1; i < name.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) return name;
    return name.substr(0, dot);
}

std::string trimSeparator(std::string base)
{
    while (!base.empty() && (base.back() == '_' || base.back() == '-' || base.back() == '.' || base.back() == ' '))
        base.pop_back();
    return base;
}

bool isCollider(const std::string& nodeName, std::string* base)
{
    const std::string name = stripDuplicate(nodeName);
    const std::string l = lower(name);
    if (l.rfind("ucx_", 0) == 0) {
        if (base) *base = name.substr(4);
        return true;
    }
    for (const char* suffix : {"collider", "collision", "col"}) {
        const size_t n = std::char_traits<char>::length(suffix);
        if (l.size() >= n && l.compare(l.size() - n, n, suffix) == 0) {
            const std::string b = name.substr(0, name.size() - n);
            if (!b.empty() && std::isalnum(static_cast<unsigned char>(b.back()))) continue; // "Atoll" isn't a collider
            if (base) *base = trimSeparator(b);
            return true;
        }
    }
    return false;
}

struct Piece {
    int mesh;
    glm::mat4 transform;
};

void appendPiece(const Model& model, const Piece& piece, LodGeometry& out)
{
    const MeshData& src = model.meshData[size_t(piece.mesh)];
    const auto& mesh = model.meshes[size_t(piece.mesh)];
    const auto& mats = model.meshMaterials[size_t(piece.mesh)];
    const auto firstIndex = static_cast<uint32_t>(out.data.indices.size());
    out.data.append(src, piece.transform);
    auto slotFor = [&](uint32_t slot) {
        const std::shared_ptr<Material> m = slot < mats.size() ? mats[slot] : nullptr;
        const auto it = std::find(out.materials.begin(), out.materials.end(), m);
        if (it != out.materials.end()) return static_cast<uint32_t>(it - out.materials.begin());
        out.materials.push_back(m);
        return static_cast<uint32_t>(out.materials.size() - 1);
    };
    for (const Submesh& sm : mesh->submeshes()) {
        Submesh s = sm;
        s.firstIndex += firstIndex;
        s.materialSlot = slotFor(sm.materialSlot);
        out.submeshes.push_back(s);
    }
}

} // namespace

int parseLodSuffix(const std::string& nodeName, std::string* base)
{
    const std::string name = stripDuplicate(nodeName);
    size_t i = name.size();
    while (i > 0 && std::isdigit(static_cast<unsigned char>(name[i - 1]))) --i;
    if (i == name.size() || i < 3) return -1;
    if (lower(name.substr(i - 3, 3)) != "lod") return -1;
    const std::string b = name.substr(0, i - 3);
    if (!b.empty() && std::isalnum(static_cast<unsigned char>(b.back()))) return -1; // "FloodLOD1"? keep it strict
    if (base) *base = trimSeparator(b);
    return std::stoi(name.substr(i));
}

uint32_t LodModel::levelCount() const
{
    size_t n = 0;
    for (const auto& v : variants) n = std::max(n, v.levels.size());
    return static_cast<uint32_t>(n);
}

LodModel lodModelFrom(const Model& model)
{
    // World matrices + parents.
    const size_t n = model.nodes.size();
    std::vector<glm::mat4> world(n, glm::mat4(1.0f));
    std::vector<int> parent(n, -1);
    std::function<void(int, const glm::mat4&)> walk = [&](int i, const glm::mat4& p) {
        world[size_t(i)] = p * model.nodes[size_t(i)].local;
        for (int c : model.nodes[size_t(i)].children) {
            parent[size_t(c)] = i;
            walk(c, world[size_t(i)]);
        }
    };
    for (int r : model.roots) walk(r, glm::mat4(1.0f));

    // Meshes in a subtree, relative to the subtree root's position (rotation/scale kept).
    auto gather = [&](int root, std::vector<Piece>& out) {
        const glm::vec3 origin(world[size_t(root)][3]);
        std::function<void(int)> rec = [&](int i) {
            const ModelNode& node = model.nodes[size_t(i)];
            if (i != root && (isCollider(node.name, nullptr) || parseLodSuffix(node.name) >= 0)) return; // own role
            if (node.mesh >= 0) {
                glm::mat4 m = world[size_t(i)];
                m[3] -= glm::vec4(origin, 0.0f);
                out.push_back({node.mesh, m});
            }
            for (int c : node.children) rec(c);
        };
        rec(root);
    };

    LodModel result;
    std::vector<std::pair<std::string, glm::vec3>> origins; // variant name -> position of its first level
    auto noteOrigin = [&](const std::string& name, int node) {
        for (const auto& o : origins)
            if (o.first == name) return;
        origins.push_back({name, glm::vec3(world[size_t(node)][3])});
    };
    auto variantFor = [&](const std::string& name) -> LodVariant& {
        for (auto& v : result.variants)
            if (v.name == name) return v;
        result.variants.push_back({name, {}, {}});
        return result.variants.back();
    };

    // LOD-named nodes and colliders.
    struct Level {
        std::string base;
        int level;
        int node;
    };
    std::vector<Level> levels;
    std::vector<std::pair<std::string, int>> colliders;
    for (size_t i = 0; i < n; ++i) {
        std::string base;
        if (isCollider(model.nodes[i].name, &base)) colliders.push_back({base, int(i)});
        else if (const int l = parseLodSuffix(model.nodes[i].name, &base); l >= 0) levels.push_back({base, l, int(i)});
    }
    if (!levels.empty()) {
        std::stable_sort(levels.begin(), levels.end(), [](const Level& a, const Level& b) { return a.level < b.level; });
        // Keep variants in file order of their first level.
        std::vector<std::string> order;
        for (size_t i = 0; i < n; ++i) {
            std::string base;
            if (!isCollider(model.nodes[i].name, nullptr) && parseLodSuffix(model.nodes[i].name, &base) >= 0 &&
                std::find(order.begin(), order.end(), base) == order.end())
                order.push_back(base);
        }
        for (const std::string& base : order) variantFor(base);
        for (const Level& l : levels) {
            std::vector<Piece> pieces;
            gather(l.node, pieces);
            if (pieces.empty()) continue;
            LodGeometry g;
            for (const Piece& p : pieces) appendPiece(model, p, g);
            variantFor(l.base).levels.push_back(std::move(g));
            noteOrigin(l.base, l.node);
        }
    } else {
        for (int r : model.roots) {
            if (isCollider(model.nodes[size_t(r)].name, nullptr)) continue;
            std::vector<Piece> pieces;
            gather(r, pieces);
            if (pieces.empty()) continue;
            LodGeometry g;
            for (const Piece& p : pieces) appendPiece(model, p, g);
            const std::string name = stripDuplicate(model.nodes[size_t(r)].name);
            variantFor(name).levels.push_back(std::move(g));
            noteOrigin(name, r);
        }
    }
    std::erase_if(result.variants, [](const LodVariant& v) { return v.levels.empty(); });

    // Colliders: matched by name, or shared ("COL") by every variant.
    for (const auto& [base, node] : colliders) {
        std::vector<Piece> pieces;
        gather(node, pieces);
        // Collider positions are kept relative to their variant's first level.
        for (auto& v : result.variants) {
            if (!base.empty() && lower(base) != lower(v.name)) continue;
            glm::vec3 origin(world[size_t(node)][3]); // shared "COL": centred like the variant
            if (!base.empty())
                for (const auto& o : origins)
                    if (o.first == v.name) origin = o.second;
            // Pieces are relative to the collider node: move them relative to the variant.
            const glm::vec3 shift = glm::vec3(world[size_t(node)][3]) - origin;
            for (const Piece& p : pieces) {
                glm::mat4 t = p.transform;
                t[3] += glm::vec4(shift, 0.0f);
                v.collider.append(model.meshData[size_t(p.mesh)], t);
            }
        }
    }
    return result;
}

LodModel loadLodModel(AssetManager& assets, const std::string& paths)
{
    const std::vector<std::string> files = str::split(paths, ';');
    if (files.empty()) throw std::runtime_error("no model path");

    LodModel result;
    for (size_t k = 0; k < files.size(); ++k) {
        auto model = assets.loadModel(files[k]);
        LodModel part = lodModelFrom(*model);
        if (part.variants.empty()) throw std::runtime_error(files[k] + ": no meshes");
        if (k == 0) {
            result = std::move(part);
            continue;
        }
        // File k = one more level for every variant.
        for (size_t i = 0; i < result.variants.size(); ++i) {
            LodVariant& src = part.variants[std::min(i, part.variants.size() - 1)];
            if (!src.levels.empty()) result.variants[i].levels.push_back(src.levels.front());
        }
    }
    return result;
}

std::vector<InstanceLod> buildLodChain(Renderer& renderer, LodChainDesc desc)
{
    std::vector<InstanceLod> chain;
    if (desc.levels.empty()) return chain;
    // Simplify from the unoptimized level 0 (before optimize() reorders it).
    if (!desc.simplify.empty()) {
        const LodGeometry base = desc.levels.front(); // a copy: push_back below may reallocate
        for (const SimplifyOptions& options : desc.simplify) {
            if (desc.levels.size() >= desc.maxLevels) break;
            LodGeometry g;
            g.data = base.data.simplified(options);
            g.materials = base.materials;
            desc.levels.push_back(std::move(g));
        }
    }
    auto withSwitch = [&](InstanceLod l) {
        if (chain.size() < desc.switches.size()) {
            l.screenSize = desc.switches[chain.size()].screenSize;
            l.maxDistance = desc.switches[chain.size()].maxDistance;
        }
        return l;
    };
    for (size_t i = 0; i < desc.levels.size() && i < desc.maxLevels; ++i) {
        LodGeometry& g = desc.levels[i];
        g.data.optimize(); // vertex cache / fetch order: faster to draw, same shape
        InstanceLod l;
        l.mesh = Mesh::create(renderer, g.data, g.submeshes.size() > 1 ? g.submeshes : std::vector<Submesh>{},
                              desc.name + " LOD" + std::to_string(i));
        l.materials = g.materials;
        chain.push_back(withSwitch(std::move(l)));
    }
    if (desc.impostor) chain.push_back(withSwitch(Impostor::bake(renderer, *chain.front().mesh, chain.front().materials)->lod()));
    return chain;
}

} // namespace ve
