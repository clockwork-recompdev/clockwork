#include "autobuild.h"

#include "handoff.h"
#include "log.h"
#include "platform.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <thread>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;

namespace frontend {
namespace {

// the checkout this program was built from, as its build folder's CMakeCache.txt records it
struct SourceTree {
    fs::path root, build, cmake;
    std::map<std::string, std::string> cache; // KEY -> value
    fs::path cargo;

    // where executable `binary`'s generated code goes (the CMake variable <BINARY>_GENERATED_DIR)
    fs::path generated(const std::string &binary) const {
        std::string key = binary;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::toupper(c)); });
        auto it = cache.find(key + "_GENERATED_DIR");
        return it == cache.end() ? fs::path() : fs::path(it->second);
    }
};

fs::path find_cargo() {
    std::error_code ec;
#if defined(_WIN32)
    const char *exe = "cargo.exe";
    const char sep = ';';
#else
    const char *exe = "cargo";
    const char sep = ':';
#endif
    if (const char *c = std::getenv("CARGO"); c && fs::is_regular_file(c, ec)) return c;
    const char *home = std::getenv("CARGO_HOME");
    std::string fallback = home ? std::string(home) : std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/.cargo";
    if (fs::path p = fs::path(fallback) / "bin" / exe; fs::is_regular_file(p, ec)) return p;
    if (const char *path = std::getenv("PATH")) {
        std::string all = path;
        for (size_t at = 0; at <= all.size();) {
            size_t end = all.find(sep, at);
            if (end == std::string::npos) end = all.size();
            if (end > at)
                if (fs::path p = fs::path(all.substr(at, end - at)) / exe; fs::is_regular_file(p, ec)) return p;
            at = end + 1;
        }
    }
    return {};
}

std::optional<SourceTree> find_source_tree(std::string *why) {
    auto fail = [&](const char *reason) -> std::optional<SourceTree> {
        if (why) *why = reason;
        return std::nullopt;
    };
    const char *base = SDL_GetBasePath();
    if (!base) return fail("Can't tell where this program is.");
    SourceTree t;
    t.build = base;
    std::ifstream in(t.build / "CMakeCache.txt");
    if (!in) return fail("This is a packaged copy: games are built from the source code (see README.md).");
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#' || line[0] == '/') continue;
        auto colon = line.find(':'), eq = line.find('=');
        if (colon == std::string::npos || eq == std::string::npos || colon > eq) continue;
        t.cache[line.substr(0, colon)] = line.substr(eq + 1);
    }
    t.root = t.cache["CMAKE_HOME_DIRECTORY"];
    t.cmake = t.cache["CMAKE_COMMAND"];
    std::error_code ec;
    if (t.root.empty() || !fs::is_regular_file(t.root / "Cargo.toml", ec) || !fs::is_directory(t.root / "tools" / "recomp", ec))
        return fail("The source code this was built from isn't there any more.");
    if (t.cmake.empty() || !fs::is_regular_file(t.cmake, ec)) return fail("CMake isn't there any more.");
    t.cargo = find_cargo();
    if (t.cargo.empty()) return fail("Building games needs Rust (cargo): https://rustup.rs");
    return t;
}

// found once: the launcher asks often
std::optional<SourceTree> source_tree(std::string *why) {
    static std::string reason;
    static const std::optional<SourceTree> tree = find_source_tree(&reason);
    if (why) *why = reason;
    return tree;
}

fs::path marker(const SourceTree &t, const std::string &binary) { return t.generated(binary) / "clockwork-dumps.txt"; }

// the program ids an executable was last built from by the launcher; none if it never was
std::optional<std::set<uint64_t>> built_from(const SourceTree &t, const std::string &binary) {
    std::ifstream in(marker(t, binary));
    if (!in) return std::nullopt;
    std::set<uint64_t> ids;
    for (std::string line; std::getline(in, line);)
        if (!line.empty()) ids.insert(std::strtoull(line.c_str(), nullptr, 16));
    return ids;
}

std::string quote(const std::string &s) {
#if defined(_WIN32)
    return "\"" + s + "\"";
#else
    std::string q = "'";
    for (char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return q + "'";
#endif
}

std::string version_name(uint64_t id) {
    const KnownDump *d = find_dump(id);
    const GameInfo *g = d ? find_game(d->game) : nullptr;
    if (!d) return "?";
    return *d->version ? d->version : g ? g->title : d->game;
}

} // namespace

bool can_build_games(std::string *why) { return source_tree(why).has_value(); }

bool needs_build(const KnownDump &dump) {
    if (!binary_available(dump.binary)) return true;
    auto t = source_tree(nullptr);
    if (!t) return false;
    auto ids = built_from(*t, dump.binary);
    return ids && !ids->count(dump.program_id);
}

void start_build(const std::shared_ptr<BuildJob> &job, const std::string &binary,
                 std::vector<std::pair<uint64_t, std::string>> dumps) {
    std::sort(dumps.begin(), dumps.end()); // the same main version every time
    std::string title;
    {
        const KnownDump *d = dumps.empty() ? nullptr : find_dump(dumps[0].first);
        const GameInfo *g = d ? find_game(d->game) : nullptr;
        title = g ? g->title : binary;
        std::string versions;
        for (auto &[id, path] : dumps)
            if (const KnownDump *k = find_dump(id); k && *k->version) versions += (versions.empty() ? "" : ", ") + std::string(k->version);
        if (!versions.empty()) title += ": " + versions;
    }
    {
        std::lock_guard lk(job->lock);
        job->running = true, job->finished = job->ok = false;
        job->binary = binary, job->title = title, job->error.clear(), job->last_line.clear();
        job->step_index = 0, job->step_count = int(dumps.size()) + 3, job->progress = -1;
        job->log_path = (platform::data_dir() / "build.log").string();
    }
    std::thread([job, binary, dumps] {
        std::ofstream log(job->log_path);
        auto set_step = [&](const std::string &what) {
            std::lock_guard lk(job->lock);
            job->step = what, job->step_index++, job->progress = -1;
            log << "== " << what << "\n";
            LOG("build: %s", what.c_str());
        };
        auto fail = [&](const std::string &why) {
            std::lock_guard lk(job->lock);
            job->error = why, job->running = false, job->finished = true, job->ok = false;
            log << "!! " << why << "\n";
            LOG("build: %s failed: %s", binary.c_str(), why.c_str());
        };
        std::string why;
        auto t = source_tree(&why);
        if (!t) return fail(why);
        fs::path gen = t->generated(binary);
        if (gen.empty()) return fail("CMake doesn't know " + binary + "'s generated code folder (an older build folder?).");

        // runs a command in the source folder; its output goes to the log, its last line to the job
        auto run = [&](const std::string &cmd) {
#if defined(_WIN32)
            std::string full = "cd /d " + quote(t->root.string()) + " && " + cmd + " 2>&1";
            FILE *p = _popen(full.c_str(), "r");
#else
            std::string full = "cd " + quote(t->root.string()) + " && " + cmd + " 2>&1";
            FILE *p = popen(full.c_str(), "r");
#endif
            log << "$ " << cmd << "\n";
            if (!p) return false;
            char buf[4096];
            while (std::fgets(buf, sizeof buf, p)) {
                std::string line = buf;
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
                log << line << "\n";
                std::lock_guard lk(job->lock);
                job->last_line = line;
                int done = 0, total = 0; // ninja: "[12/340] Building C object ..."
                if (std::sscanf(line.c_str(), "[%d/%d]", &done, &total) == 2 && total > 0) job->progress = float(done) / float(total);
            }
            log.flush();
#if defined(_WIN32)
            return _pclose(p) == 0;
#else
            int status = pclose(p);
            return status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
        };

        std::string cargo = quote(t->cargo.string()), cmake = quote(t->cmake.string());
        std::vector<fs::path> unpacked;
        for (auto &[id, path] : dumps) {
            char dir[24];
            std::snprintf(dir, sizeof dir, "%016llx", (unsigned long long)id);
            fs::path out = t->root / "extracted" / dir;
            set_step("Unpacking " + version_name(id));
            if (!run(cargo + " run --release -p ctrx -- " + quote(path) + " " + quote(out.string())))
                return fail("Couldn't unpack " + path + ".");
            unpacked.push_back(out);
        }
        set_step("Recompiling to C");
        std::string emit = cargo + " run --release -p recomp -- emit " + quote(unpacked[0].string()) + " " + quote(gen.string());
        for (size_t i = 1; i < unpacked.size(); i++) emit += " " + quote(unpacked[i].string());
        if (!run(emit)) return fail("The recompiler stopped.");
        set_step("Setting up the build");
        if (!run(cmake + " -S " + quote(t->root.string()) + " -B " + quote(t->build.string())))
            return fail("CMake couldn't set up the build.");
        set_step("Compiling (this takes a while)");
        if (!run(cmake + " --build " + quote(t->build.string()) + " --target " + binary)) return fail("Compiling stopped.");
        {
            std::ofstream m(marker(*t, binary));
            for (auto &[id, path] : dumps) {
                char hex[20];
                std::snprintf(hex, sizeof hex, "%016llx", (unsigned long long)id);
                m << hex << "\n";
            }
        }
        LOG("build: %s built", binary.c_str());
        std::lock_guard lk(job->lock);
        job->running = false, job->finished = true, job->ok = true, job->progress = 1;
    }).detach();
}

} // namespace frontend
