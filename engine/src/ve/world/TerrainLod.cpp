#include "ve/world/TerrainLod.hpp"

#include "ve/core/Log.hpp"
#include "ve/render/Renderer.hpp"

#include <algorithm>
#include <cmath>

namespace ve {

TerrainLod::TerrainLod(Renderer& renderer) : m_renderer(&renderer)
{
    // Nodes build independently: a few workers (the first view needs a few hundred).
    const unsigned n = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u);
    for (unsigned i = 0; i < n; ++i) m_workers.emplace_back([this] { workerLoop(); });
}

TerrainLod::~TerrainLod()
{
    {
        std::lock_guard lock(m_mutex);
        m_quit = true;
        m_jobs.clear();
    }
    m_wake.notify_all();
    for (std::thread& t : m_workers) t.join();
}

void TerrainLod::setData(std::shared_ptr<const TerrainData> data, const TerrainLook& look, const Settings& settings,
                         const glm::vec4* changed)
{
    const bool layoutChanged = !m_data || data->res != m_data->res || data->cellSize != m_data->cellSize ||
                               data->origin != m_data->origin || settings.nodeCells != m_settings.nodeCells;
    m_data = std::move(data);
    m_look = std::make_shared<const TerrainLook>(look);
    m_settings = settings;
    m_settings.nodeCells = std::max(m_settings.nodeCells, 4) & ~1; // even: parent vertices are every other one
    const uint64_t previous = m_version++;
    if (layoutChanged) m_nodes.clear(); // different grid: old meshes don't fit any more
    m_levels = 1;
    while (int64_t(m_settings.nodeCells) << (m_levels - 1) < m_data->res - 1) ++m_levels;
    if (changed && !layoutChanged) {
        // Nodes away from the edit are still up to date (normals reach 2 vertices beyond a node).
        for (auto& [k, n] : m_nodes) {
            if (n.version != previous) continue;
            const float size = nodeSize(k.level), margin = 3.0f * m_data->cellSize * float(1 << k.level);
            const glm::vec2 lo = m_data->origin + glm::vec2(float(k.x), float(k.z)) * size - margin;
            const glm::vec2 hi = lo + size + 2.0f * margin;
            if (hi.x < changed->x || lo.x > changed->z || hi.y < changed->y || lo.y > changed->w) n.version = m_version;
        }
    }
}

float TerrainLod::nodeSize(int level) const
{
    return float(m_settings.nodeCells << level) * m_data->cellSize;
}

bool TerrainLod::exists(const Key& k) const
{
    const int64_t cells = int64_t(m_settings.nodeCells) << k.level;
    return k.x >= 0 && k.z >= 0 && k.x * cells < m_data->res - 1 && k.z * cells < m_data->res - 1;
}

glm::vec2 TerrainLod::morphRange(int level) const
{
    if (!m_data || level + 1 >= m_levels) return glm::vec2(1e30f); // the root never morphs
    // A level-L node exists while its parent is split, i.e. closer than this:
    const float merge = nodeSize(level + 1) * m_settings.lodFactor;
    return {merge * 0.7f, merge};
}

void TerrainLod::request(const Key& k, float priority)
{
    Node& n = m_nodes[k];
    if (n.queued) return;
    n.queued = true;
    m_requests.push_back({priority, k});
}

void TerrainLod::select(const Key& k, const glm::vec3& camera, std::vector<Key>& want)
{
    Node& n = m_nodes[k];
    const float size = nodeSize(k.level);
    const glm::vec2 lo = m_data->origin + glm::vec2(float(k.x), float(k.z)) * size;
    const glm::vec2 hi = glm::min(lo + size, m_data->boundsMax());
    // Distance to the node's box (its height range once built; until then
    // horizontal only, which errs towards more detail).
    const glm::vec2 dxz = glm::max(glm::max(lo - glm::vec2(camera.x, camera.z), glm::vec2(camera.x, camera.z) - hi), glm::vec2(0.0f));
    const float dy = n.mesh ? std::max({n.minY - camera.y, camera.y - n.maxY, 0.0f}) : 0.0f;
    const float dist = std::sqrt(glm::dot(dxz, dxz) + dy * dy);

    if (k.level > 0 && dist < size * m_settings.lodFactor) {
        bool ready = true;
        Key children[4];
        int count = 0;
        for (int c = 0; c < 4; ++c) {
            const Key ck{k.level - 1, k.x * 2 + (c & 1), k.z * 2 + (c >> 1)};
            if (!exists(ck)) continue;
            children[count++] = ck;
            auto it = m_nodes.find(ck);
            if (it == m_nodes.end() || !it->second.mesh) {
                ready = false;
                request(ck, dist / size);
            }
        }
        if (ready) {
            for (int c = 0; c < count; ++c) select(children[c], camera, want);
            return;
        }
    }
    // Draw this node (it has a mesh: only ready nodes are walked into).
    n.lastUse = m_frame;
    want.push_back(k);
    if (n.version != m_version) request(k, dist / size); // edited terrain: rebuild, keep showing the old one
}

void TerrainLod::update(const glm::vec3& camera, bool preload, bool uploadAll)
{
    if (!m_data) return;
    ++m_frame;
    const Key root{m_levels - 1, 0, 0};

    for (int pass = 0; pass < (preload ? 64 : 1); ++pass) {
        // 1. Finished meshes (budgeted, except while preloading).
        std::vector<Result> results;
        {
            std::lock_guard lock(m_mutex);
            const size_t n = preload || uploadAll ? m_results.size() : std::min<size_t>(m_results.size(), m_settings.uploadsPerFrame);
            results.assign(std::make_move_iterator(m_results.begin()), std::make_move_iterator(m_results.begin() + ptrdiff_t(n)));
            m_results.erase(m_results.begin(), m_results.begin() + ptrdiff_t(n));
        }
        for (Result& r : results) upload(r);

        // 2. Jobs nobody started yet are re-decided below from this frame's view.
        {
            std::lock_guard lock(m_mutex);
            for (const Job& j : m_jobs)
                if (auto it = m_nodes.find(j.key); it != m_nodes.end()) it->second.queued = false;
            m_jobs.clear();
        }

        // 3. Walk the tree.
        m_requests.clear();
        std::vector<Key> want;
        auto rootIt = m_nodes.find(root);
        if (rootIt == m_nodes.end() || !rootIt->second.mesh) request(root, 0.0f);
        else select(root, camera, want);

        // 4. Hand the missing meshes to the worker: coarse / near first.
        std::sort(m_requests.begin(), m_requests.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        if (preload) {
            // Build them right here.
            std::vector<Job> jobs;
            for (const auto& [prio, k] : m_requests) jobs.push_back({k, m_version, m_settings.nodeCells, m_data, m_look});
            for (Job& j : jobs) {
                Result r{j.key, j.version, {}, 0.0f, 0.0f};
                r.mesh = build(j, r.minY, r.maxY);
                upload(r);
            }
            bool busy;
            {
                std::lock_guard lock(m_mutex);
                busy = m_busy > 0 || !m_results.empty();
            }
            if (jobs.empty() && !busy) {
                m_drawn.clear();
                for (const Key& k : want) m_drawn.push_back({m_nodes[k].mesh, k.level});
                break;
            }
            if (jobs.empty()) std::this_thread::yield();
            continue;
        }
        {
            std::lock_guard lock(m_mutex);
            for (const auto& [prio, k] : m_requests) m_jobs.push_back({k, m_version, m_settings.nodeCells, m_data, m_look});
        }
        if (!m_requests.empty()) m_wake.notify_one();

        m_drawn.clear();
        for (const Key& k : want) m_drawn.push_back({m_nodes[k].mesh, k.level});
    }

    evict();
    std::lock_guard lock(m_mutex);
    m_stats.drawn = uint32_t(m_drawn.size());
    m_stats.pending = uint32_t(m_jobs.size()) + uint32_t(m_busy) + uint32_t(m_results.size());
    m_stats.missing = uint32_t(m_requests.size());
}

void TerrainLod::upload(Result& r)
{
    Node& n = m_nodes[r.key];
    n.queued = false;
    // An outdated result is still better than nothing; a node with a mesh waits for the current one.
    if (r.version != m_version && n.mesh) return;
    n.mesh = Mesh::create(*m_renderer, r.mesh, {}, "terrain node");
    n.version = r.version;
    n.minY = r.minY;
    n.maxY = r.maxY;
}

void TerrainLod::evict()
{
    if (m_nodes.size() <= m_settings.maxNodes) return;
    // Least recently drawn first; never what's on screen or still being built.
    std::vector<std::pair<uint64_t, Key>> old;
    for (const auto& [k, n] : m_nodes)
        if (n.lastUse + 2 < m_frame && !n.queued) old.push_back({n.lastUse, k});
    std::sort(old.begin(), old.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (size_t i = 0; i < old.size() && m_nodes.size() > m_settings.maxNodes; ++i) m_nodes.erase(old[i].second);
}

void TerrainLod::workerLoop()
{
    for (;;) {
        Job job;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [&] { return m_quit || !m_jobs.empty(); });
            if (m_quit) return;
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
            ++m_busy;
        }
        Result r{job.key, job.version, {}, 0.0f, 0.0f};
        r.mesh = build(job, r.minY, r.maxY);
        std::lock_guard lock(m_mutex);
        m_results.push_back(std::move(r));
        --m_busy;
    }
}

MeshData TerrainLod::build(const Job& job, float& minY, float& maxY) const
{
    const TerrainData& d = *job.data;
    const int L = job.key.level, step = 1 << L, N = job.nodeCells, V = N + 1;
    const int i0 = job.key.x * N * step, j0 = job.key.z * N * step;
    const float e = d.cellSize * float(step); // vertex spacing

    // Heights on this level's grid with a 2-vertex apron (normals, parent normals).
    const int E = N + 5;
    auto clampI = [&](int i) { return std::clamp(i, 0, d.res - 1); };
    auto sample = [&](int i, int j) {
        i = clampI(i);
        j = clampI(j);
        // Fine levels reuse the shared tile cache; coarse ones evaluate the
        // generator only where they need it (same values: tiles come from it).
        return L <= 1 ? d.sampleHeight(i, j) : d.generateHeight(d.samplePosition(i, j));
    };
    std::vector<float> H(size_t(E) * E);
    for (int b = -2; b <= N + 2; ++b)
        for (int a = -2; a <= N + 2; ++a) H[size_t(b + 2) * E + size_t(a + 2)] = sample(i0 + a * step, j0 + b * step);
    auto h = [&](int a, int b) { return H[size_t(b + 2) * E + size_t(a + 2)]; };
    auto normalAt = [&](int a, int b, int s) { // spacing s vertices
        return glm::normalize(glm::vec3(h(a - s, b) - h(a + s, b), 2.0f * e * float(s), h(a, b - s) - h(a, b + s)));
    };

    MeshData md;
    md.vertices.reserve(size_t(V) * V + size_t(4 * V));
    std::vector<float> w(d.biomeNames.size());

    // Splat layers: the textured biomes, then rock. Each vertex keeps its
    // weight for (at most) three of them, the same three across the node;
    // whatever weight is left over shows the vertex colour.
    const TerrainLook& look = *job.look;
    const size_t nb = d.biomeNames.size(), layers = nb + 1;
    std::vector<uint8_t> textured(layers);
    for (size_t k = 0; k < nb; ++k) textured[k] = d.biomeTextured(int(k));
    textured[nb] = look.rockMaterial.textured();
    const bool splat = std::find(textured.begin(), textured.end(), 1) != textured.end();
    std::vector<float> layerWeights(splat ? size_t(V) * V * layers : 0);
    std::vector<double> layerTotals(layers, 0.0);
    minY = 1e30f;
    maxY = -1e30f;
    for (int b = 0; b <= N; ++b) {
        for (int a = 0; a <= N; ++a) {
            const int i = clampI(i0 + a * step), j = clampI(j0 + b * step);
            const glm::vec2 p = d.samplePosition(i, j);
            const float y = h(a, b);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);

            // Fine-scale slope and biomes (as at full detail, so colours don't change with the level).
            glm::vec3 fine;
            if (L <= 1) {
                // Level 0 and 1 vertices are full-resolution samples: the
                // cached tiles hold their heights and biome weights (computed
                // from the same neighbours, see TerrainData::generateTile).
                if (L == 0) {
                    fine = normalAt(a, b, 1);
                } else {
                    auto g = [&](int di, int dj) { return d.sampleHeight(clampI(i + di), clampI(j + dj)); };
                    fine = glm::normalize(glm::vec3(g(-1, 0) - g(1, 0), 2.0f * d.cellSize, g(0, -1) - g(0, 1)));
                }
                for (size_t k = 0; k < w.size(); ++k) w[k] = d.sampleBiome(int(k), i, j);
            } else {
                const float c = d.cellSize;
                auto g = [&](int di, int dj) { return d.generateHeight(d.samplePosition(clampI(i + di), clampI(j + dj))); };
                fine = glm::normalize(glm::vec3(g(-1, 0) - g(1, 0), 2.0f * c, g(0, -1) - g(0, 1)));
                d.generateBiomes(p, y, 1.0f - fine.y, w.data());
            }

            // Where the vertex lies on the parent's grid (every other vertex,
            // cells split along the same diagonal): the geomorph target.
            const bool oa = a & 1, ob = b & 1;
            float py;
            glm::vec3 pn;
            if (!oa && !ob) {
                py = y;
                pn = normalAt(a, b, 2);
            } else if (oa && !ob) {
                py = 0.5f * (h(a - 1, b) + h(a + 1, b));
                pn = normalAt(a - 1, b, 2) + normalAt(a + 1, b, 2);
            } else if (!oa && ob) {
                py = 0.5f * (h(a, b - 1) + h(a, b + 1));
                pn = normalAt(a, b - 1, 2) + normalAt(a, b + 1, 2);
            } else {
                py = 0.5f * (h(a - 1, b - 1) + h(a + 1, b + 1));
                pn = normalAt(a - 1, b - 1, 2) + normalAt(a + 1, b + 1, 2);
            }
            pn = glm::normalize(pn);

            float rock = 0.0f, mud = 0.0f;
            Vertex v;
            v.position = {p.x, y, p.y};
            v.normal = normalAt(a, b, 1);
            v.uv = glm::vec2(0.0f);
            // Parent normal as x/z (terrain normals always point up): .w is a splat weight.
            v.tangent = glm::vec4(py, pn.x, pn.z, 0.0f);
            v.color = glm::vec4(d.groundColor(look, p, y, fine, w.data(), &rock, &mud), 16777215.0f); // no splat layers
            md.vertices.push_back(v);
            if (splat) {
                float* lw = &layerWeights[(size_t(b) * V + size_t(a)) * layers];
                for (size_t k = 0; k < nb; ++k) lw[k] = textured[k] ? w[k] * (1.0f - mud) * (1.0f - rock) : 0.0f;
                lw[nb] = textured[nb] ? rock : 0.0f;
                for (size_t k = 0; k < layers; ++k) layerTotals[k] += lw[k];
            }
        }
    }
    if (splat) {
        // The node's three heaviest layers; indices packed into colour.a
        // (8 bits each, 255 = unused), weights into uv.x, uv.y, tangent.w.
        std::vector<size_t> order(layers);
        for (size_t k = 0; k < layers; ++k) order[k] = k;
        std::sort(order.begin(), order.end(), [&](size_t x, size_t y) { return layerTotals[x] > layerTotals[y]; });
        int channel[3] = {-1, -1, -1};
        float packed = 0.0f;
        for (int c = 0; c < 3; ++c) {
            const bool used = c < int(layers) && layerTotals[order[size_t(c)]] > 0.0 && order[size_t(c)] < 255;
            if (used) channel[c] = int(order[size_t(c)]);
            packed += float(used ? order[size_t(c)] : 255) * float(1 << (8 * c));
        }
        for (size_t vi = 0; vi < md.vertices.size(); ++vi) {
            const float* lw = &layerWeights[vi * layers];
            Vertex& v = md.vertices[vi];
            v.color.a = packed;
            v.uv = {channel[0] >= 0 ? lw[channel[0]] : 0.0f, channel[1] >= 0 ? lw[channel[1]] : 0.0f};
            v.tangent.w = channel[2] >= 0 ? lw[channel[2]] : 0.0f;
        }
    }
    // Cells: split along the (i, j) -> (i + 1, j + 1) diagonal (as TerrainData::height and Jolt).
    md.indices.reserve(size_t(N) * N * 6 + size_t(4 * N * 12));
    for (int b = 0; b < N; ++b) {
        for (int a = 0; a < N; ++a) {
            const auto v00 = uint32_t(b * V + a), v10 = v00 + 1, v01 = v00 + uint32_t(V), v11 = v01 + 1;
            md.indices.insert(md.indices.end(), {v00, v01, v11, v00, v11, v10});
        }
    }
    // Skirts: each edge gets a copy of its vertices pushed down, joined both ways round.
    const float depth = 2.0f * e + 1.0f;
    auto skirt = [&](auto vertexIndex) {
        const auto first = uint32_t(md.vertices.size());
        for (int k = 0; k <= N; ++k) {
            Vertex v = md.vertices[vertexIndex(k)];
            v.position.y -= depth;
            v.tangent.x -= depth;
            md.vertices.push_back(v);
        }
        for (int k = 0; k < N; ++k) {
            const auto t0 = uint32_t(vertexIndex(k)), t1 = uint32_t(vertexIndex(k + 1)), s0 = first + uint32_t(k), s1 = s0 + 1;
            md.indices.insert(md.indices.end(), {t0, s0, t1, t1, s0, s1, t0, t1, s0, t1, s1, s0});
        }
    };
    skirt([&](int k) { return size_t(k); });                     // b = 0
    skirt([&](int k) { return size_t(N) * V + size_t(k); });     // b = N
    skirt([&](int k) { return size_t(k) * V; });                 // a = 0
    skirt([&](int k) { return size_t(k) * V + size_t(N); });     // a = N
    return md;
}

} // namespace ve
