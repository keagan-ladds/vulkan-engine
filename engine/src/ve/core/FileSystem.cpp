#include "ve/core/FileSystem.hpp"

#include "ve/core/Log.hpp"
#include "ve/core/Pack.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <shared_mutex>
#include <streambuf>

namespace ve::vfs {

namespace {

struct Mount {
    std::filesystem::path root; // normalized absolute folder
    std::unique_ptr<PackReader> pack;
};

struct State {
    std::shared_mutex lock;
    std::vector<Mount> mounts; // lookup order: back to front
};

State& state()
{
    static State s;
    return s;
}

std::filesystem::path normalized(const std::filesystem::path& p)
{
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(p, ec);
    if (ec) abs = p;
    // weakly_canonical resolves the part that exists on disk (symlinks, "..")
    // and keeps the rest lexically: archive-only paths normalize the same way.
    std::filesystem::path c = std::filesystem::weakly_canonical(abs, ec);
    return ec ? abs.lexically_normal() : c;
}

// The entry name `file` would have in an archive mounted at `root` (empty: outside it).
std::string entryName(const std::filesystem::path& file, const std::filesystem::path& root)
{
    const std::filesystem::path rel = file.lexically_relative(root);
    if (rel.empty()) return {};
    const std::string s = rel.generic_string();
    if (s == "." || s.starts_with("..")) return {};
    return s;
}

bool onDisk(const std::filesystem::path& file)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(file, ec);
}

// The newest archive entry for `file` (shared lock held by the caller).
const PackReader::Entry* findEntry(const State& s, const std::filesystem::path& file, const PackReader** pack)
{
    if (s.mounts.empty()) return nullptr;
    const std::filesystem::path abs = normalized(file);
    for (auto it = s.mounts.rbegin(); it != s.mounts.rend(); ++it) {
        const std::string name = entryName(abs, it->root);
        if (name.empty()) continue;
        if (const PackReader::Entry* e = it->pack->find(name)) {
            if (pack) *pack = it->pack.get();
            return e;
        }
    }
    return nullptr;
}

// A read-only streambuf over owned bytes (seekable).
class MemoryBuf : public std::streambuf {
public:
    explicit MemoryBuf(std::vector<uint8_t> bytes) : m_bytes(std::move(bytes))
    {
        char* b = reinterpret_cast<char*>(m_bytes.data());
        setg(b, b, b + m_bytes.size());
    }

protected:
    pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode) override
    {
        const off_type base = dir == std::ios_base::beg ? 0 : dir == std::ios_base::cur ? off_type(gptr() - eback()) : off_type(m_bytes.size());
        const off_type to = base + off;
        if (to < 0 || to > off_type(m_bytes.size())) return pos_type(off_type(-1));
        setg(eback(), eback() + to, egptr());
        return pos_type(to);
    }
    pos_type seekpos(pos_type pos, std::ios_base::openmode mode) override { return seekoff(off_type(pos), std::ios_base::beg, mode); }

private:
    std::vector<uint8_t> m_bytes;
};

class MemoryStream : public std::istream {
public:
    explicit MemoryStream(std::vector<uint8_t> bytes) : std::istream(nullptr), m_buf(std::move(bytes)) { rdbuf(&m_buf); }

private:
    MemoryBuf m_buf;
};

} // namespace

bool mount(const std::filesystem::path& archive, const std::filesystem::path& root, std::string* error)
{
    auto pack = std::make_unique<PackReader>();
    std::string err;
    if (!pack->open(archive, &err)) {
        if (error) *error = err;
        log::error("vfs: {}", err);
        return false;
    }
    log::info("vfs: mounted {} ({} files) over {}", archive.filename().string(), pack->entries().size(), root.string());
    State& s = state();
    std::unique_lock lock(s.lock);
    s.mounts.push_back({normalized(root), std::move(pack)});
    return true;
}

int mountAll(const std::filesystem::path& folder)
{
    std::vector<std::filesystem::path> archives;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(folder, ec))
        if (e.is_regular_file() && e.path().extension() == ".vepak") archives.push_back(e.path());
    std::sort(archives.begin(), archives.end()); // data.vepak, then patch_01.vepak... (later wins)
    int n = 0;
    for (const auto& a : archives) n += mount(a, folder) ? 1 : 0;
    return n;
}

void unmountAll()
{
    State& s = state();
    std::unique_lock lock(s.lock);
    s.mounts.clear();
}

std::vector<std::filesystem::path> mounted()
{
    State& s = state();
    std::shared_lock lock(s.lock);
    std::vector<std::filesystem::path> out;
    for (auto it = s.mounts.rbegin(); it != s.mounts.rend(); ++it) out.push_back(it->pack->file());
    return out;
}

bool exists(const std::filesystem::path& file)
{
    if (onDisk(file)) return true;
    State& s = state();
    std::shared_lock lock(s.lock);
    return findEntry(s, file, nullptr) != nullptr;
}

bool inArchive(const std::filesystem::path& file)
{
    if (onDisk(file)) return false;
    State& s = state();
    std::shared_lock lock(s.lock);
    return findEntry(s, file, nullptr) != nullptr;
}

std::optional<uint64_t> fileSize(const std::filesystem::path& file)
{
    std::error_code ec;
    if (onDisk(file)) return uint64_t(std::filesystem::file_size(file, ec));
    State& s = state();
    std::shared_lock lock(s.lock);
    if (const PackReader::Entry* e = findEntry(s, file, nullptr)) return e->size;
    return std::nullopt;
}

std::optional<std::vector<uint8_t>> read(const std::filesystem::path& file)
{
    if (onDisk(file)) {
        std::ifstream in(file, std::ios::binary | std::ios::ate);
        if (!in) return std::nullopt;
        std::vector<uint8_t> bytes(size_t(in.tellg()));
        in.seekg(0);
        if (!in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()))) return std::nullopt;
        return bytes;
    }
    State& s = state();
    std::shared_lock lock(s.lock);
    const PackReader* pack = nullptr;
    if (const PackReader::Entry* e = findEntry(s, file, &pack)) return pack->read(*e);
    return std::nullopt;
}

std::optional<std::string> readText(const std::filesystem::path& file)
{
    auto bytes = read(file);
    if (!bytes) return std::nullopt;
    return std::string(bytes->begin(), bytes->end());
}

std::unique_ptr<std::istream> open(const std::filesystem::path& file)
{
    if (onDisk(file)) {
        auto in = std::make_unique<std::ifstream>(file, std::ios::binary);
        if (*in) return in;
        return nullptr;
    }
    auto bytes = read(file);
    if (!bytes) return nullptr;
    return std::make_unique<MemoryStream>(std::move(*bytes));
}

std::vector<std::filesystem::path> list(const std::filesystem::path& folder, bool recursive)
{
    std::set<std::filesystem::path> out;
    std::error_code ec;
    if (std::filesystem::is_directory(folder, ec)) {
        auto add = [&](const std::filesystem::directory_entry& e) {
            if (e.is_regular_file()) out.insert(e.path());
        };
        if (recursive)
            for (auto it = std::filesystem::recursive_directory_iterator(folder, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) add(*it);
        else
            for (const auto& e : std::filesystem::directory_iterator(folder, ec)) add(e);
    }
    State& s = state();
    std::shared_lock lock(s.lock);
    const std::filesystem::path abs = normalized(folder);
    for (const Mount& m : s.mounts) {
        std::string prefix = entryName(abs, m.root);
        if (abs == m.root) prefix.clear();
        else if (prefix.empty()) continue;
        else prefix += '/';
        for (const PackReader::Entry& e : m.pack->entries()) {
            if (!e.name.starts_with(prefix)) continue;
            const std::string rest = e.name.substr(prefix.size());
            if (!recursive && rest.find('/') != std::string::npos) continue;
            out.insert(folder / std::filesystem::path(rest));
        }
    }
    return {out.begin(), out.end()};
}

} // namespace ve::vfs
