#include "ve/assets/DataAsset.hpp"

#include "ve/assets/AssetManager.hpp"
#include "ve/core/Engine.hpp"
#include "ve/core/FileSystem.hpp"
#include "ve/core/Log.hpp"

#include <fstream>
#include <map>
#include <system_error>

namespace ve {

namespace {

struct Entry {
    std::shared_ptr<const Json> content = std::make_shared<const Json>();
    std::filesystem::path file; // resolved, empty = not found
    std::filesystem::file_time_type stamp{};
    bool dirty = false;
};

struct State {
    std::map<std::string, Entry> entries;
    uint64_t generation = 1;
    float sincePoll = 0.0f;
};

State& state()
{
    static State s;
    return s;
}

std::filesystem::path resolveFile(const std::string& path)
{
    try {
        return Engine::get().assets().resolve(path);
    } catch (const std::exception&) {
        return {};
    }
}

std::filesystem::file_time_type stampOf(const std::filesystem::path& file)
{
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(file, ec);
    return ec ? std::filesystem::file_time_type{} : t;
}

// (Re)reads the entry's file; false (content unchanged) if it can't be parsed.
bool readFile(const std::string& path, Entry& e)
{
    e.file = resolveFile(path);
    if (e.file.empty()) {
        log::warnOnce("Data asset not found: " + path);
        return false;
    }
    e.stamp = stampOf(e.file); // (an archive's file: no time, never reloaded)
    const std::unique_ptr<std::istream> in = vfs::open(e.file);
    try {
        if (!in) throw std::runtime_error("can't read " + e.file.string());
        Json j = Json::parse(*in);
        e.content = std::make_shared<const Json>(std::move(j));
        return true;
    } catch (const std::exception& ex) {
        log::error("Data asset {}: invalid JSON ({})", path, ex.what());
        return false;
    }
}

Entry& entry(const std::string& path)
{
    State& s = state();
    auto it = s.entries.find(path);
    if (it != s.entries.end()) return it->second;
    Entry& e = s.entries[path];
    readFile(path, e);
    return e;
}

} // namespace

std::shared_ptr<const Json> DataAssets::get(const std::string& path)
{
    if (path.empty()) return std::make_shared<const Json>();
    return entry(path).content;
}

uint64_t DataAssets::generation()
{
    return state().generation;
}

void DataAssets::set(const std::string& path, Json content)
{
    Entry& e = entry(path);
    e.content = std::make_shared<const Json>(std::move(content));
    e.dirty = true;
    ++state().generation;
}

bool DataAssets::dirty(const std::string& path)
{
    auto it = state().entries.find(path);
    return it != state().entries.end() && it->second.dirty;
}

std::filesystem::path DataAssets::writablePath(const std::string& path)
{
    const std::filesystem::path p(path);
    if (p.is_absolute()) return p;
    const auto& dirs = Engine::get().assets().searchPaths();
    return dirs.empty() ? p : dirs.front() / p;
}

namespace {
bool writeJson(const std::filesystem::path& file, const Json& content)
{
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) return false;
        out << content.dump(2) << '\n';
        if (!out) return false;
    }
    std::filesystem::rename(tmp, file, ec); // atomic: readers never see half a file
    return !ec;
}
} // namespace

bool DataAssets::save(const std::string& path)
{
    Entry& e = entry(path);
    const std::filesystem::path file = writablePath(path);
    if (!writeJson(file, *e.content)) {
        log::error("Data asset: can't write {}", file.string());
        return false;
    }
    e.file = resolveFile(path);
    e.stamp = stampOf(e.file); // our own write isn't a change to reload
    e.dirty = false;
    log::info("Saved {}", file.string());
    return true;
}

void DataAssets::revert(const std::string& path)
{
    Entry& e = entry(path);
    readFile(path, e);
    e.dirty = false;
    ++state().generation;
}

bool DataAssets::create(const std::string& path, const Json& content)
{
    set(path, content);
    return save(path);
}

bool DataAssets::exists(const std::string& path)
{
    return !resolveFile(path).empty();
}

void DataAssets::poll(float dt)
{
    State& s = state();
    s.sincePoll += dt;
    if (s.sincePoll < 0.5f || s.entries.empty()) return;
    s.sincePoll = 0.0f;
    for (auto& [path, e] : s.entries) {
        if (e.file.empty()) {
            // Not found before: it may have been created since.
            if (resolveFile(path).empty()) continue;
        } else if (stampOf(e.file) == e.stamp) {
            continue;
        }
        if (e.dirty) {
            // Unsaved editor changes win; the file's stamp is remembered so we warn once.
            e.stamp = stampOf(e.file);
            log::warn("Data asset {} changed on disk but has unsaved edits (revert to load it)", path);
            continue;
        }
        if (readFile(path, e)) {
            ++s.generation;
            log::info("Reloaded {}", path);
        }
    }
}

std::vector<std::string> DataAssets::cachedPaths()
{
    std::vector<std::string> out;
    for (const auto& [path, e] : state().entries) out.push_back(path);
    return out;
}

} // namespace ve
