#include "ve/render/ShaderHotReload.hpp"

#include "ve/core/Log.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <sstream>

#ifdef _WIN32
#define VE_POPEN _popen
#define VE_PCLOSE _pclose
#else
#define VE_POPEN popen
#define VE_PCLOSE pclose
#endif

namespace ve {

namespace {

struct CompileResult {
    bool ok = false;
    std::string output;
};

CompileResult runCommand(const std::vector<std::string>& args)
{
    std::string cmd;
    for (const std::string& a : args) {
        if (!cmd.empty()) cmd += ' ';
        cmd += '"' + a + '"';
    }
    cmd += " 2>&1";
#ifdef _WIN32
    cmd = '"' + cmd + '"'; // cmd.exe strips one level of outer quotes
#endif
    CompileResult r;
    FILE* pipe = VE_POPEN(cmd.c_str(), "r");
    if (!pipe) {
        r.output = "could not run glslc";
        return r;
    }
    char buf[512];
    while (std::fgets(buf, sizeof(buf), pipe)) r.output += buf;
    r.ok = VE_PCLOSE(pipe) == 0;
    return r;
}

} // namespace

ShaderHotReload::ShaderHotReload(const std::vector<std::filesystem::path>& shaderDirectories)
{
    if (const char* v = std::getenv("VE_SHADER_HOT_RELOAD"); v && std::string(v) == "0") return;
    std::error_code ec;
    for (const auto& dir : shaderDirectories) {
        for (const auto& f : std::filesystem::directory_iterator(dir, ec)) {
            const std::filesystem::path p = f.path();
            if (p.extension() != ".cmd") continue; // <name>.spv.cmd
            Entry e;
            e.name = p.stem().stem().string();
            e.depfile = p.parent_path() / (p.stem().string() + ".d");
            std::ifstream in(p);
            for (std::string line; std::getline(in, line);) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) e.command.push_back(line);
            }
            if (e.command.empty()) continue;
            readDepfile(e);
            if (e.sources.empty()) continue;
            StampCache seen;
            e.stamp = newest(e, seen);
            m_entries.push_back(std::move(e));
        }
    }
    if (!m_entries.empty()) log::info("Shader hot-reload: watching {} shaders", m_entries.size());
}

void ShaderHotReload::readDepfile(Entry& e)
{
    // Make syntax: "out.spv: src.frag inc/a.glsl \<newline> inc/b.glsl";
    // spaces inside paths are escaped as "\ ".
    std::ifstream in(e.depfile);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    size_t colon = text.find(": ");
    if (colon == std::string::npos) return;
    text = text.substr(colon + 2);
    std::vector<std::filesystem::path> deps;
    std::string cur;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\' && i + 1 < text.size() && (text[i + 1] == ' ' || text[i + 1] == '\n' || text[i + 1] == '\r')) {
            if (text[i + 1] == ' ') cur += ' ';
            else if (!cur.empty()) {
                deps.emplace_back(cur);
                cur.clear();
            }
            ++i;
        } else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            if (!cur.empty()) {
                deps.emplace_back(cur);
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) deps.emplace_back(cur);
    e.sources = std::move(deps);
}

std::filesystem::file_time_type ShaderHotReload::newest(const Entry& e, StampCache& seen)
{
    // Not {}: libstdc++'s file clock epoch is in the future, so real times are negative.
    constexpr auto kNone = std::filesystem::file_time_type::min();
    auto t = kNone;
    for (const auto& s : e.sources) {
        auto [it, fresh] = seen.try_emplace(s.string(), kNone);
        if (fresh) {
            std::error_code ec;
            const auto m = std::filesystem::last_write_time(s, ec);
            if (!ec) it->second = m;
        }
        t = std::max(t, it->second);
    }
    return t;
}

std::vector<std::string> ShaderHotReload::poll()
{
    const auto now = std::chrono::steady_clock::now();
    if (m_entries.empty() || now - m_lastPoll < std::chrono::milliseconds(500)) return {};
    return pollNow();
}

std::vector<std::string> ShaderHotReload::pollNow()
{
    m_lastPoll = std::chrono::steady_clock::now();
    std::vector<Entry*> changed;
    StampCache seen;
    for (Entry& e : m_entries) {
        const auto t = newest(e, seen);
        if (t > e.stamp) {
            e.stamp = t; // also on failure: don't retry until the next save
            changed.push_back(&e);
        }
    }
    if (changed.empty()) return {};

    // An edited include (ve.glsl) can touch dozens of shaders: compile them in parallel.
    std::vector<std::future<CompileResult>> jobs;
    jobs.reserve(changed.size());
    for (Entry* e : changed) jobs.push_back(std::async(std::launch::async, runCommand, e->command));

    std::vector<std::string> ok;
    for (size_t i = 0; i < changed.size(); ++i) {
        Entry& e = *changed[i];
        const CompileResult r = jobs[i].get();
        if (r.ok) {
            readDepfile(e); // includes may have changed (new ones aren't in `seen` yet)
            e.stamp = std::max(e.stamp, newest(e, seen));
            ok.push_back(e.name);
        } else {
            m_lastError = e.name + ":\n" + r.output;
            log::error("Shader hot-reload: {} failed to compile (keeping the old version):\n{}", e.name, r.output);
        }
    }
    if (!ok.empty()) {
        ++m_reloads;
        std::string list;
        for (const auto& n : ok) list += (list.empty() ? "" : ", ") + n;
        log::info("Shader hot-reload: recompiled {}", list);
        if (ok.size() == changed.size()) m_lastError.clear();
    }
    return ok;
}

} // namespace ve
