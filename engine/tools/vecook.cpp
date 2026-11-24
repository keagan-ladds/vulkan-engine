// =============================================================================
// vecook - cooks a game's assets ahead of time (the same cooker the engine
// runs on demand while you develop; see ve/assets/TextureCook.hpp).
//
//   vecook                      cook everything new or changed into the game's cache
//   vecook --check              report what's stale or missing (exit code 1 if anything is)
//   vecook --force              cook everything again
//   vecook --clean              also delete cooked files whose source is gone
//   vecook --out <dir> <assets folder>...
//
// Defaults (vecook is built next to the game, in build/bin): the game's source
// assets folder (build/bin/ve-source-assets.txt, written by ve_setup_game),
// else build/bin/assets; the cache is build/bin/cooked, where the game looks.
// Run it before packaging a release, or in CI; while developing, the engine
// cooks what it loads by itself.
// =============================================================================

#include <ve/assets/TextureCook.hpp>
#include <ve/core/Jobs.hpp>
#include <ve/core/Window.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void usage()
{
    std::puts("usage: vecook [--out <cache dir>] [--force] [--check] [--clean] [--quiet] [assets folder...]\n"
              "Cooks every image under the asset folders (textures: BC4 / BC5 / BC7 KTX2 with mips; settings from\n"
              "<image>.texture files or the file names, see ve/assets/TextureSettings.hpp) into the cook cache.\n"
              "Defaults: the game's source assets folder (ve-source-assets.txt next to vecook) or ./assets next to\n"
              "vecook, and ./cooked next to vecook (the game's cache).");
}

const char* formatName(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_BC4_UNORM_BLOCK: return "BC4";
    case VK_FORMAT_BC5_UNORM_BLOCK: return "BC5";
    case VK_FORMAT_BC7_UNORM_BLOCK: return "BC7";
    case VK_FORMAT_BC7_SRGB_BLOCK: return "BC7 sRGB";
    case VK_FORMAT_R8G8B8A8_SRGB: return "RGBA8 sRGB";
    case VK_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
    default: return "?";
    }
}

struct Item {
    fs::path source;
    std::string name; // asset path (relative to its folder): the cache key
    fs::path cooked;
};

} // namespace

int main(int argc, char** argv)
{
    fs::path out;
    std::vector<fs::path> roots;
    bool force = false, check = false, clean = false, quiet = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) out = argv[++i];
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

    // Every cookable image (hidden folders skipped).
    std::vector<Item> items;
    for (const fs::path& root : roots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) {
            std::fprintf(stderr, "vecook: %s is not a folder\n", root.string().c_str());
            return 2;
        }
        for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_directory() && it->path().filename().string().starts_with(".")) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file() || !ve::isCookableImage(it->path())) continue;
            const std::string name = fs::relative(it->path(), root).generic_string();
            items.push_back({it->path(), name, ve::cookedTexturePath(out, name)});
        }
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.name < b.name; });
    std::printf("vecook: %zu images in %zu folder(s) -> %s\n", items.size(), roots.size(), out.string().c_str());

    // Which need cooking.
    std::vector<size_t> todo;
    for (size_t i = 0; i < items.size(); ++i) {
        const ve::TextureSettings settings = ve::TextureSettings::forImage(items[i].source);
        ve::Ktx2Texture header;
        const bool current = !force && ve::readKtx2(items[i].cooked, header, true) &&
                             ve::checkCookedTexture(header, items[i].source, &settings) == ve::CookedState::Current;
        if (!current) todo.push_back(i);
        if (check && !current) std::printf("  stale: %s\n", items[i].name.c_str());
    }

    // Cooked files whose source is gone.
    std::vector<fs::path> orphans;
    if (clean || check) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(out, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file() || it->path().extension() != ".ktx2") continue;
            const std::string name = fs::relative(it->path(), out).generic_string();
            const std::string sourceName = name.substr(0, name.size() - 5); // without ".ktx2"
            const bool known = std::any_of(items.begin(), items.end(), [&](const Item& item) { return item.name == sourceName; });
            if (!known) orphans.push_back(it->path());
        }
    }
    if (check) {
        for (const fs::path& o : orphans) std::printf("  orphan: %s\n", o.string().c_str());
        std::printf("vecook: %zu to cook, %zu current, %zu orphaned\n", todo.size(), items.size() - todo.size(), orphans.size());
        return todo.empty() && orphans.empty() ? 0 : 1;
    }

    // Cook, spread over the worker threads.
    const auto t0 = std::chrono::steady_clock::now();
    std::mutex printLock;
    std::atomic<uint64_t> sourceBytes{0}, cookedBytes{0}, rgbaBytes{0};
    std::atomic<int> failed{0};
    ve::jobs::parallelFor(todo.size(), [&](size_t k) {
        const Item& item = items[todo[k]];
        const ve::TextureSettings settings = ve::TextureSettings::forImage(item.source);
        const ve::TextureCookResult r = ve::cookTexture(item.source, settings, item.cooked);
        std::error_code ec;
        sourceBytes += fs::file_size(item.source, ec);
        std::lock_guard lock(printLock);
        if (!r.ok) {
            ++failed;
            std::fprintf(stderr, "  FAILED %s: %s\n", item.name.c_str(), r.error.c_str());
            return;
        }
        cookedBytes += r.bytes;
        for (uint32_t l = 0; l < r.levels; ++l) rgbaBytes += uint64_t(std::max(1u, r.width >> l)) * std::max(1u, r.height >> l) * 4;
        if (!quiet)
            std::printf("  %-48s %-6s %-10s %5ux%-5u %2u mips %8.0f KB %6.0f ms\n", item.name.c_str(), ve::toString(settings.type),
                        formatName(r.format), r.width, r.height, r.levels, double(r.bytes) / 1024.0, r.milliseconds);
    });
    for (const fs::path& o : orphans) {
        std::error_code ec;
        fs::remove(o, ec);
        if (!quiet) std::printf("  removed orphan %s\n", o.string().c_str());
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("vecook: cooked %zu (%d failed), %zu already current, %zu orphans removed in %.1f s",
                todo.size() - size_t(failed.load()), failed.load(), items.size() - todo.size(), orphans.size(), seconds);
    if (!todo.empty())
        std::printf("; %.1f MB of images -> %.1f MB GPU-ready (%.1f MB of GPU memory uncompressed)", double(sourceBytes) / 1048576.0,
                    double(cookedBytes) / 1048576.0, double(rgbaBytes) / 1048576.0);
    std::printf("\n");
    ve::jobs::shutdown();
    return failed > 0 ? 1 : 0;
}
