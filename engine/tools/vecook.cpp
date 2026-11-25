// =============================================================================
// vecook - cooks a game's assets ahead of time (the same cookers the engine
// runs on demand while you develop; see ve/assets/Cookers.hpp), and packages
// a release.
//
//   vecook                      cook everything new or changed into the game's cache
//   vecook --check              report what's stale or missing (exit code 1 if anything is)
//   vecook --force              cook everything again
//   vecook --clean              also delete cooked files whose source is gone
//   vecook --package <dir>      cook, then write a release: the game's executable
//                               (+ runtime libraries) and data.vepak (assets, cooked
//                               files and shaders; ve/core/Pack.hpp)
//       --loose                 ...as plain folders (assets/, cooked/, shaders/) instead
//       --game <name>           which executable (default: the only one next to vecook)
//       --level <n>             zstd level for the archive (default 9; 0 = store)
//   vecook --out <dir> <assets folder>...
//
// Defaults (vecook is built next to the game, in build/bin): the game's source
// assets folder (build/bin/ve-source-assets.txt, written by ve_setup_game),
// else build/bin/assets; the cache is build/bin/cooked, where the game looks.
//
// Most cooks are pure CPU work, spread over the worker threads. Cooks that
// need the renderer (models whose <model>.model asks for LODs / an impostor:
// impostors are rendered) run afterwards on the main thread, on a headless
// ve::Engine that vecook starts only when one is needed.
//
// Packaging: every file under the asset folders ships, except
//   - files a cooker takes (images, WAV / FLAC sounds, glTF models): their
//     cooked copies ship instead (cooked/<asset path>.<ext>);
//   - files those are made from (a glTF's .bin buffers: in the .vemodel);
//   - cooker settings files (.texture, .audio, .model);
//   - hidden files / folders, Blender files and temporaries (*.blend, *.blend1,
//     *.tmp, *~), and anything matched by <assets>/.vepackignore: one glob per
//     line, '#' comments; '*' and '?' within a path segment, '**' across
//     segments; no '/' = matched against the file name; a trailing '/' = a folder.
// plus the compiled shaders (build/bin/shaders/*.spv) and data the engine
// generated into the cache (cooked/generated/: e.g. the cloud noise, written
// by the first run of the game).
// =============================================================================

#include <ve/assets/Cookers.hpp>
#include <ve/core/Engine.hpp>
#include <ve/core/Jobs.hpp>
#include <ve/core/Pack.hpp>
#include <ve/core/Window.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

void usage()
{
    std::puts("usage: vecook [--out <cache dir>] [--force] [--check] [--clean] [--quiet] [assets folder...]\n"
              "       vecook --package <release dir> [--loose] [--game <executable>] [--level <0-19>] [...]\n"
              "Cooks every cookable file under the asset folders into the cook cache (images -> BC4 / BC5 / BC7 KTX2,\n"
              "WAV / FLAC -> Ogg Vorbis, glTF -> .vemodel with LODs / impostors; settings: <file>.texture / .audio / .model). --package then writes a release folder:\n"
              "the game's executable and data.vepak (or, with --loose, assets/ cooked/ shaders/). See the header of\n"
              "engine/tools/vecook.cpp for what ships.");
}

struct Asset {
    fs::path source;
    std::string name; // asset path, relative to its folder ("textures/a.png"): the cache key and archive name
    const ve::Cooker* cooker = nullptr;
    fs::path cooked;  // cooker's output (if a cooker takes it)
};

// --- Globs (.vepackignore) ---------------------------------------------------------

bool globMatch(std::string_view p, std::string_view s)
{
    if (p.empty()) return s.empty();
    if (p.starts_with("**")) {
        p.remove_prefix(2);
        if (p.starts_with('/')) p.remove_prefix(1);
        for (size_t i = 0; i <= s.size(); ++i)
            if (globMatch(p, s.substr(i))) return true;
        return false;
    }
    if (p[0] == '*') {
        for (size_t i = 0; i <= s.size(); ++i) {
            if (globMatch(p.substr(1), s.substr(i))) return true;
            if (i < s.size() && s[i] == '/') break;
        }
        return false;
    }
    if (s.empty()) return false;
    if (p[0] == '?' ? s[0] != '/' : p[0] == s[0]) return globMatch(p.substr(1), s.substr(1));
    return false;
}

struct Ignore {
    std::vector<std::string> patterns{"*.blend", "*.blend1", "*.tmp", "*~", "Thumbs.db", "desktop.ini"};

    void load(const fs::path& file)
    {
        std::ifstream in(file);
        for (std::string line; std::getline(in, line);) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty() && line[0] != '#') patterns.push_back(line);
        }
    }

    bool matches(const std::string& rel) const // rel: '/'-separated, relative to the assets folder
    {
        const std::string fileName = rel.substr(rel.find_last_of('/') + 1);
        for (std::string p : patterns) {
            if (p.ends_with('/')) { // a folder: any parent of the file
                p.pop_back();
                for (size_t i = rel.find('/'); i != std::string::npos; i = rel.find('/', i + 1)) {
                    const std::string dir = rel.substr(0, i);
                    const std::string last = dir.substr(dir.find_last_of('/') + 1);
                    if (p.find('/') == std::string::npos ? globMatch(p, last) : globMatch(p, dir)) return true;
                }
            } else if (p.find('/') == std::string::npos ? globMatch(p, fileName) : globMatch(p, rel)) {
                return true;
            }
        }
        return false;
    }
};

// --- The game's executable -------------------------------------------------------

bool isExecutable(const fs::path& p)
{
#ifdef _WIN32
    return p.extension() == ".exe";
#else
    std::error_code ec;
    const auto perms = fs::status(p, ec).permissions();
    const std::string ext = p.extension().string();
    return !ec && (perms & fs::perms::owner_exec) != fs::perms::none && ext != ".so" && ext != ".sh" && ext != ".py";
#endif
}

bool isRuntimeLibrary(const fs::path& p)
{
#ifdef _WIN32
    return p.extension() == ".dll";
#else
    return p.filename().string().find(".so") != std::string::npos;
#endif
}

std::string human(uint64_t bytes)
{
    char b[32];
    if (bytes >= 1048576) std::snprintf(b, sizeof(b), "%.1f MB", double(bytes) / 1048576.0);
    else std::snprintf(b, sizeof(b), "%.0f KB", double(bytes) / 1024.0);
    return b;
}

} // namespace

int main(int argc, char** argv)
{
    fs::path out, packageDir;
    std::vector<fs::path> roots;
    std::string game;
    bool force = false, check = false, clean = false, quiet = false, loose = false;
    int level = 9;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--package" && i + 1 < argc) packageDir = argv[++i];
        else if (a == "--game" && i + 1 < argc) game = argv[++i];
        else if (a == "--level" && i + 1 < argc) level = std::clamp(std::atoi(argv[++i]), 0, 19);
        else if (a == "--loose") loose = true;
        else if (a == "--force") force = true;
        else if (a == "--check") check = true;
        else if (a == "--clean") clean = true;
        else if (a == "--quiet") quiet = true;
        else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "vecook: unknown option %s\n", a.c_str());
            usage();
            return 2;
        } else {
            roots.emplace_back(a);
        }
    }
    const fs::path exeDir = ve::executableDirectory();
    if (roots.empty()) {
        if (std::ifstream src(exeDir / "ve-source-assets.txt"); src) {
            std::string dir;
            std::getline(src, dir);
            if (!dir.empty() && fs::is_directory(dir)) roots.emplace_back(dir);
        }
        if (roots.empty()) roots.push_back(exeDir / "assets");
    }
    if (out.empty()) out = exeDir / "cooked";

    // Every file under the asset folders (hidden folders skipped): cookable
    // ones with their cooker, the rest ship as they are (when packaging).
    std::vector<Asset> assets;
    std::vector<Ignore> ignores;
    for (const fs::path& root : roots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) {
            std::fprintf(stderr, "vecook: %s is not a folder\n", root.string().c_str());
            return 2;
        }
        Ignore ignore;
        ignore.load(root / ".vepackignore");
        for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->path().filename().string().starts_with(".")) {
                if (it->is_directory()) it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file()) continue;
            Asset a;
            a.source = it->path();
            a.name = fs::relative(it->path(), root).generic_string();
            if (ignore.matches(a.name) || ve::isCookerSettingsFile(a.source)) continue;
            a.cooker = ve::cookerFor(a.source);
            if (a.cooker) a.cooked = a.cooker->cookedPath(out, a.name);
            assets.push_back(std::move(a));
        }
    }
    std::sort(assets.begin(), assets.end(), [](const Asset& a, const Asset& b) { return a.name < b.name; });
    std::map<std::string, size_t> perCooker;
    for (const Asset& a : assets)
        if (a.cooker) ++perCooker[a.cooker->name];
    std::string kinds;
    for (const auto& [name, n] : perCooker) kinds += (kinds.empty() ? "" : ", ") + std::to_string(n) + " " + name;
    std::printf("vecook: %zu assets (%s cookable) in %zu folder(s) -> %s\n", assets.size(), kinds.empty() ? "none" : kinds.c_str(),
                roots.size(), out.string().c_str());

    // Which need cooking.
    std::vector<size_t> todo;
    std::set<fs::path> expected;
    for (size_t i = 0; i < assets.size(); ++i) {
        const Asset& a = assets[i];
        if (!a.cooker) continue;
        expected.insert(a.cooked.lexically_normal());
        const bool current = !force && a.cooker->check(a.cooked, a.source) == ve::CookedState::Current;
        if (!current) todo.push_back(i);
        if (check && !current) std::printf("  stale: %s\n", a.name.c_str());
    }

    // Cooked files whose source is gone (anything in the cache no asset expects).
    std::vector<fs::path> orphans;
    if (clean || check) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(out, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            if (it->is_directory() && it->path() == out / "generated") { // engine-made data, not cooked from an asset
                it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file() && !it->path().filename().string().ends_with(".tmp") && !expected.count(it->path().lexically_normal()))
                orphans.push_back(it->path());
        }
    }
    if (check) {
        for (const fs::path& o : orphans) std::printf("  orphan: %s\n", o.string().c_str());
        const size_t cookable = expected.size();
        std::printf("vecook: %zu to cook, %zu current, %zu orphaned\n", todo.size(), cookable - todo.size(), orphans.size());
        return todo.empty() && orphans.empty() ? 0 : 1;
    }

    // Cook, spread over the worker threads.
    const auto t0 = std::chrono::steady_clock::now();
    std::mutex printLock;
    std::map<std::string, std::pair<uint64_t, uint64_t>> sizes; // per cooker: source bytes, cooked bytes
    std::atomic<int> failed{0};
    // CPU cooks on the worker threads; cooks that need the renderer after them, on this thread.
    std::vector<size_t> cpuTodo, gpuTodo;
    for (size_t i : todo) {
        const Asset& a = assets[i];
        (a.cooker->needsRenderer && a.cooker->needsRenderer(a.source) ? gpuTodo : cpuTodo).push_back(i);
    }
    auto cookOne = [&](const Asset& a) {
        const auto c0 = std::chrono::steady_clock::now();
        const ve::Cooker::Result r = a.cooker->cook(a.source, a.cooked, nullptr);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
        std::error_code ec;
        const uint64_t before = fs::file_size(a.source, ec);
        std::lock_guard lock(printLock);
        if (!r.ok) {
            ++failed;
            std::fprintf(stderr, "  FAILED %s: %s\n", a.name.c_str(), r.error.c_str());
            return;
        }
        sizes[a.cooker->name].first += before;
        sizes[a.cooker->name].second += r.bytes;
        if (!quiet) std::printf("  %-48s %s %8s %6.0f ms\n", a.name.c_str(), r.summary.c_str(), human(r.bytes).c_str(), ms);
    };
    ve::jobs::parallelFor(cpuTodo.size(), [&](size_t k) { cookOne(assets[cpuTodo[k]]); });
    if (!gpuTodo.empty()) {
        // A headless engine for the renderer, reading the same asset folders and
        // cook cache (textures and materials the models use resolve as in the game).
        ve::EngineConfig config;
        config.title = "vecook";
        config.width = 64;
        config.height = 64;
        config.renderer.headless = true;
        config.enableAudio = false;
        config.enableEditor = false;
        config.mountArchives = false;
        config.cookOnDemand = false; // textures were cooked above; nothing else to cook in the background
        config.cookCache = out;
        config.assetPaths = roots;
        std::unique_ptr<ve::Engine> engine;
        try {
            engine = std::make_unique<ve::Engine>(config);
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                         "vecook: can't start the renderer for %zu model(s) with LODs / impostors: %s\n"
                         "  (it needs a Vulkan device and a display, like the game: on a Linux server run vecook under xvfb-run -a)\n",
                         gpuTodo.size(), e.what());
            failed += int(gpuTodo.size());
        }
        if (engine)
            for (size_t i : gpuTodo) cookOne(assets[i]);
    }
    if (clean) {
        for (const fs::path& o : orphans) {
            std::error_code ec;
            fs::remove(o, ec);
            if (!quiet) std::printf("  removed orphan %s\n", o.string().c_str());
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("vecook: cooked %zu (%d failed), %zu already current%s in %.1f s\n", todo.size() - size_t(failed.load()), failed.load(),
                expected.size() - todo.size(), clean ? (", " + std::to_string(orphans.size()) + " orphans removed").c_str() : "", seconds);
    for (const auto& [name, s] : sizes) std::printf("  %s: %s -> %s\n", name.c_str(), human(s.first).c_str(), human(s.second).c_str());
    if (failed > 0) {
        ve::jobs::shutdown();
        return 1;
    }
    if (packageDir.empty()) {
        ve::jobs::shutdown();
        return 0;
    }

    // --- Package -------------------------------------------------------------------
    // What ships, by its path relative to the release folder.
    struct Ship {
        std::string name;
        fs::path from;
    };
    std::vector<Ship> ship;
    std::set<fs::path> madeInto; // files a cooked asset is made from (a glTF's .bin buffers): inside the cooked copy
    for (const Asset& a : assets)
        if (a.cooker && a.cooker->dependencies)
            for (const fs::path& d : a.cooker->dependencies(a.source)) madeInto.insert(d.lexically_normal());
    for (const Asset& a : assets) {
        if (a.cooker) ship.push_back({"cooked/" + fs::relative(a.cooked, out).generic_string(), a.cooked});
        else if (!madeInto.count(a.source.lexically_normal())) ship.push_back({"assets/" + a.name, a.source});
    }
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(out / "generated", ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file() && !it->path().filename().string().ends_with(".tmp"))
            ship.push_back({"cooked/" + fs::relative(it->path(), out).generic_string(), it->path()});
    ec.clear(); // (no generated/ folder is fine)
    for (const auto& e : fs::directory_iterator(exeDir / "shaders", ec))
        if (e.is_regular_file() && e.path().extension() == ".spv") ship.push_back({"shaders/" + e.path().filename().string(), e.path()});
    std::sort(ship.begin(), ship.end(), [](const Ship& a, const Ship& b) { return a.name < b.name; });

    // The game's executable and its runtime libraries.
    std::vector<fs::path> executables;
    for (const auto& e : fs::directory_iterator(exeDir, ec)) {
        if (!e.is_regular_file()) continue;
        const std::string stem = e.path().stem().string();
        if (stem == "vecook" || !isExecutable(e.path())) continue;
        if (!game.empty() && stem != game && e.path().filename().string() != game) continue;
        executables.push_back(e.path());
    }
    if (executables.size() != 1) {
        std::fprintf(stderr, "vecook: %s; name the game with --game <name>:\n", executables.empty() ? "no game executable found" : "several executables");
        for (const fs::path& p : executables) std::fprintf(stderr, "  %s\n", p.filename().string().c_str());
        ve::jobs::shutdown();
        return 2;
    }

    // A clean release folder: what an earlier package wrote goes first.
    fs::create_directories(packageDir, ec);
    for (const char* stale : {"data.vepak", "assets", "cooked", "shaders"}) fs::remove_all(packageDir / stale, ec);
    std::vector<fs::path> binaries = executables;
    for (const auto& e : fs::directory_iterator(exeDir, ec))
        if (e.is_regular_file() && isRuntimeLibrary(e.path())) binaries.push_back(e.path());
    for (const fs::path& b : binaries) {
        fs::copy_file(b, packageDir / b.filename(), fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::fprintf(stderr, "vecook: can't copy %s: %s\n", b.string().c_str(), ec.message().c_str());
            ve::jobs::shutdown();
            return 1;
        }
    }

    uint64_t raw = 0, stored = 0;
    if (loose) {
        for (const Ship& s : ship) {
            const fs::path to = packageDir / fs::path(s.name);
            fs::create_directories(to.parent_path(), ec);
            fs::copy_file(s.from, to, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                std::fprintf(stderr, "vecook: can't copy %s: %s\n", s.from.string().c_str(), ec.message().c_str());
                ve::jobs::shutdown();
                return 1;
            }
            raw += fs::file_size(to, ec);
        }
        stored = raw;
    } else {
        ve::PackWriter pack;
        std::string error;
        if (!pack.begin(packageDir / "data.vepak", level, &error)) {
            std::fprintf(stderr, "vecook: %s\n", error.c_str());
            ve::jobs::shutdown();
            return 1;
        }
        for (const Ship& s : ship) {
            std::ifstream in(s.from, std::ios::binary | std::ios::ate);
            std::vector<char> bytes(in ? size_t(in.tellg()) : 0);
            in.seekg(0);
            if (!in || !in.read(bytes.data(), std::streamsize(bytes.size())) || !pack.add(s.name, bytes.data(), bytes.size())) {
                std::fprintf(stderr, "vecook: can't add %s\n", s.from.string().c_str());
                ve::jobs::shutdown();
                return 1;
            }
        }
        if (!pack.finish(&error)) {
            std::fprintf(stderr, "vecook: %s\n", error.c_str());
            ve::jobs::shutdown();
            return 1;
        }
        raw = pack.rawBytes();
        stored = pack.storedBytes();
    }
    size_t shaders = 0, cooked = 0;
    for (const Ship& s : ship) {
        if (s.name.starts_with("shaders/")) ++shaders;
        else if (s.name.starts_with("cooked/")) ++cooked;
    }
    std::printf("vecook: packaged %s -> %s\n  %s + %zu runtime libraries\n  %zu files (%zu assets, %zu cooked, %zu shaders): %s%s\n",
                executables[0].filename().string().c_str(), packageDir.string().c_str(), executables[0].filename().string().c_str(),
                binaries.size() - 1, ship.size(), ship.size() - cooked - shaders, cooked, shaders, human(raw).c_str(),
                loose ? " (loose files)" : (" -> data.vepak " + human(stored)).c_str());
    ve::jobs::shutdown();
    return 0;
}
