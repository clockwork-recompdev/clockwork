#include "autobuild.h"

#include "game_code.h"
#include "handoff.h"
#include "log.h"
#include "platform.h"
#include "recomp_module.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
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

#if defined(_WIN32)
constexpr const char *EXE = ".exe";
constexpr char PATH_SEP = ';';
#else
constexpr const char *EXE = "";
constexpr char PATH_SEP = ':';
#endif

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

// A packaged release: the clockwork program with the tools and the module's sources next to it
// (tools/ctrx, tools/recomp, sdk/). Games are built as modules with the C compiler on this PC
struct Kit {
    fs::path ctrx, recomp, sdk;
    bool msvc = false; // Windows: Visual Studio's cl and link; elsewhere a cc-like compiler
    std::string cc;    // that compiler (cc, clang, gcc); for MSVC, vcvars64.bat, which sets it up
};

// `exe` on the PATH, or empty
fs::path on_path(const std::string &exe) {
    std::error_code ec;
    if (const char *path = std::getenv("PATH")) {
        std::string all = path;
        for (size_t at = 0; at <= all.size();) {
            size_t end = all.find(PATH_SEP, at);
            if (end == std::string::npos) end = all.size();
            if (end > at)
                if (fs::path p = fs::path(all.substr(at, end - at)) / exe; fs::is_regular_file(p, ec)) return p;
            at = end + 1;
        }
    }
    return {};
}

fs::path find_cargo() {
    std::error_code ec;
    std::string exe = std::string("cargo") + EXE;
    if (const char *c = std::getenv("CARGO"); c && fs::is_regular_file(c, ec)) return c;
    const char *home = std::getenv("CARGO_HOME");
    std::string fallback = home ? std::string(home) : std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/.cargo";
    if (fs::path p = fs::path(fallback) / "bin" / exe; fs::is_regular_file(p, ec)) return p;
    return on_path(exe);
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

FILE *open_command(const std::string &cmd) {
#if defined(_WIN32)
    // cmd /c drops the outermost quotes, so a command that starts with a quoted path keeps its own
    return _popen(("\"" + cmd + " 2>&1\"").c_str(), "r");
#else
    return popen((cmd + " 2>&1").c_str(), "r");
#endif
}

bool close_command(FILE *p) {
#if defined(_WIN32)
    return _pclose(p) == 0;
#else
    int status = pclose(p);
    return status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

// a command's whole output, or nullopt if it failed
std::optional<std::string> capture(const std::string &cmd) {
    FILE *p = open_command(cmd);
    if (!p) return std::nullopt;
    std::string out;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, p)) out += buf;
    if (!close_command(p)) return std::nullopt;
    return out;
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
    if (!in) return fail("This copy can't build games: its tools folder is missing.");
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

// whether this is a packaged release; nullopt (with `why` set only if it is one that can't build)
std::optional<Kit> find_kit(std::string *why) {
    auto fail = [&](const char *reason) -> std::optional<Kit> {
        if (why) *why = reason;
        return std::nullopt;
    };
    const char *base = SDL_GetBasePath();
    if (!base || game_code::linked()) return std::nullopt;
    Kit k;
    k.ctrx = fs::path(base) / "tools" / (std::string("ctrx") + EXE);
    k.recomp = fs::path(base) / "tools" / (std::string("recomp") + EXE);
    k.sdk = fs::path(base) / "sdk";
    std::error_code ec;
    if (!fs::is_regular_file(k.ctrx, ec) || !fs::is_regular_file(k.recomp, ec) || !fs::is_regular_file(k.sdk / "module.c", ec))
        return std::nullopt;
    if (const char *cc = std::getenv("CC"); cc && *cc) {
        k.cc = cc;
        return k;
    }
#if defined(_WIN32)
    // Visual Studio (or its Build Tools) with the C++ workload, found the way its installer records it
    const char *pf = std::getenv("ProgramFiles(x86)");
    fs::path vswhere = fs::path(pf ? pf : "C:\\Program Files (x86)") / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
    if (fs::is_regular_file(vswhere, ec)) {
        auto found = capture(quote(vswhere.string()) +
                             " -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath");
        if (found) {
            std::string dir = *found;
            while (!dir.empty() && (dir.back() == '\n' || dir.back() == '\r')) dir.pop_back();
            if (fs::path bat = fs::path(dir) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"; !dir.empty() && fs::is_regular_file(bat, ec)) {
                k.msvc = true, k.cc = bat.string();
                return k;
            }
        }
    }
    return fail("Building games needs Visual Studio's C++ compiler: install \"Build Tools for Visual Studio\" with "
                "\"Desktop development with C++\" (free, visualstudio.microsoft.com/downloads), then open Clockwork again.");
#else
    for (const char *cc : {"cc", "clang", "gcc"})
        if (fs::path p = on_path(cc); !p.empty() && capture(quote(p.string()) + " --version")) {
            k.cc = p.string();
            return k;
        }
#if defined(__APPLE__)
    return fail("Building games needs Apple's C compiler: run  xcode-select --install  in Terminal, then open Clockwork again.");
#else
    return fail("Building games needs a C compiler: install gcc or clang (e.g. the build-essential or base-devel "
                "package), then open Clockwork again.");
#endif
#endif
}

// found once: the launcher asks often
struct Builder {
    std::optional<Kit> kit;
    std::optional<SourceTree> tree;
    std::string why; // when neither can build
};
const Builder &builder() {
    static const Builder b = [] {
        Builder b;
        std::string kit_why;
        b.kit = find_kit(&kit_why);
        if (b.kit || !kit_why.empty()) {
            b.why = kit_why;
            return b; // a packaged copy builds modules, or says why it can't
        }
        b.tree = find_source_tree(&b.why);
        return b;
    }();
    return b;
}

fs::path marker(const SourceTree &t, const std::string &binary) { return t.generated(binary) / "clockwork-dumps.txt"; }
// a module's record of what it was built from: "v<module version>" then the program ids
fs::path module_marker(const std::string &binary) {
    fs::path p = game_code::module_path(binary);
    return p.replace_extension(".txt");
}

// the program ids an executable (or module) was last built from by the launcher; none if it never
// was, or if a module was built for another module version
std::optional<std::set<uint64_t>> read_marker(const fs::path &path, bool versioned) {
    std::ifstream in(path);
    if (!in) return std::nullopt;
    std::set<uint64_t> ids;
    std::string line;
    if (versioned && (!std::getline(in, line) || line != "v" + std::to_string(CLOCKWORK_MODULE_VERSION))) return std::nullopt;
    while (std::getline(in, line))
        if (!line.empty()) ids.insert(std::strtoull(line.c_str(), nullptr, 16));
    return ids;
}

void write_marker(const fs::path &path, bool versioned, const std::vector<std::pair<uint64_t, std::string>> &dumps) {
    std::ofstream m(path);
    if (versioned) m << "v" << CLOCKWORK_MODULE_VERSION << "\n";
    for (auto &[id, dump_path] : dumps) {
        char hex[20];
        std::snprintf(hex, sizeof hex, "%016llx", (unsigned long long)id);
        m << hex << "\n";
    }
}

std::string hex_id(uint64_t id) {
    char dir[24];
    std::snprintf(dir, sizeof dir, "%016llx", (unsigned long long)id);
    return dir;
}

std::string version_name(uint64_t id) {
    const KnownDump *d = find_dump(id);
    const GameInfo *g = d ? find_game(d->game) : nullptr;
    if (!d) return "?";
    std::string v = version_label(*d);
    return !v.empty() ? v : g ? g->title : d->game;
}

// One build's progress, shared by the steps and the log
struct Run {
    std::shared_ptr<BuildJob> job;
    std::ofstream log;
    std::mutex log_lock;

    // (never holding log_lock and job->lock at once)
    void note(const std::string &line) {
        std::lock_guard ll(log_lock);
        log << line << "\n";
        log.flush();
    }
    void step(const std::string &what) {
        note("== " + what);
        LOG("build: %s", what.c_str());
        std::lock_guard lk(job->lock);
        job->step = what, job->step_index++, job->progress = -1;
    }
    void fail(const std::string &binary, const std::string &why) {
        note("!! " + why);
        LOG("build: %s failed: %s", binary.c_str(), why.c_str());
        std::lock_guard lk(job->lock);
        job->error = why, job->running = false, job->finished = true, job->ok = false;
    }
    void succeed(const std::string &binary) {
        LOG("build: %s built", binary.c_str());
        std::lock_guard lk(job->lock);
        job->running = false, job->finished = true, job->ok = true, job->progress = 1;
    }
    // runs a command; its output goes to the log, its last line to the job. `ninja` reads ninja's
    // "[12/340] ..." lines as the step's progress
    bool run(const std::string &cmd, bool ninja = false) {
        note("$ " + cmd);
        FILE *p = open_command(cmd);
        if (!p) return false;
        char buf[4096];
        while (std::fgets(buf, sizeof buf, p)) {
            std::string line = buf;
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            note(line);
            std::lock_guard lk(job->lock);
            job->last_line = line;
            int done = 0, total = 0;
            if (ninja && std::sscanf(line.c_str(), "[%d/%d]", &done, &total) == 2 && total > 0)
                job->progress = float(done) / float(total);
        }
        return close_command(p);
    }
};

// in a source checkout: the game's executable, with the tools run by cargo and the build by CMake
void build_executable(Run &r, const SourceTree &t, const std::string &binary,
                      const std::vector<std::pair<uint64_t, std::string>> &dumps) {
    fs::path gen = t.generated(binary);
    if (gen.empty()) return r.fail(binary, "CMake doesn't know " + binary + "'s generated code folder (an older build folder?).");
    std::string in_root = (
#if defined(_WIN32)
                              "cd /d "
#else
                              "cd "
#endif
                              + quote(t.root.string()) + " && ");
    std::string cargo = quote(t.cargo.string()), cmake = quote(t.cmake.string());
    std::vector<fs::path> unpacked;
    for (auto &[id, path] : dumps) {
        fs::path out = t.root / "extracted" / hex_id(id);
        r.step("Unpacking " + version_name(id));
        if (!r.run(in_root + cargo + " run --release -p ctrx -- " + quote(path) + " " + quote(out.string())))
            return r.fail(binary, "Couldn't unpack " + path + ".");
        unpacked.push_back(out);
    }
    r.step("Recompiling to C");
    std::string emit = in_root + cargo + " run --release -p recomp -- emit " + quote(unpacked[0].string()) + " " + quote(gen.string());
    for (size_t i = 1; i < unpacked.size(); i++) emit += " " + quote(unpacked[i].string());
    if (!r.run(emit)) return r.fail(binary, "The recompiler stopped.");
    r.step("Setting up the build");
    if (!r.run(in_root + cmake + " -S " + quote(t.root.string()) + " -B " + quote(t.build.string())))
        return r.fail(binary, "CMake couldn't set up the build.");
    r.step("Compiling (this takes a while)");
    if (!r.run(in_root + cmake + " --build " + quote(t.build.string()) + " --target " + binary, true))
        return r.fail(binary, "Compiling stopped.");
    write_marker(marker(t, binary), false, dumps);
    r.succeed(binary);
}

// Visual Studio's environment (INCLUDE, LIB, PATH for cl and link), taken from vcvars64.bat once
// and kept in this process, so every compile after it finds the compiler
bool enter_msvc(Run &r, const Kit &k) {
#if defined(_WIN32)
    static bool done = false;
    if (done) return true;
    auto env = capture(quote(k.cc) + " >nul && set");
    if (!env) return false;
    for (size_t at = 0; at < env->size();) {
        size_t end = env->find('\n', at);
        if (end == std::string::npos) end = env->size();
        std::string line = env->substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (auto eq = line.find('='); eq != std::string::npos && eq > 0)
            _putenv_s(line.substr(0, eq).c_str(), line.substr(eq + 1).c_str());
        at = end + 1;
    }
    r.note("(Visual Studio: " + k.cc + ")");
    done = true;
    return true;
#else
    (void)r, (void)k;
    return false;
#endif
}

// in a packaged copy: the game's module, with the bundled tools and this PC's C compiler, in the
// data folder's build/ (removed afterwards)
void build_module(Run &r, const Kit &k, const std::string &binary, const std::vector<std::pair<uint64_t, std::string>> &dumps) {
    std::error_code ec;
    fs::path work = platform::data_dir() / "build" / binary, gen = work / "c", obj = work / "obj";
    fs::remove_all(work, ec);
    fs::create_directories(gen, ec);
    fs::create_directories(obj, ec);
    std::vector<fs::path> unpacked;
    for (auto &[id, path] : dumps) {
        fs::path out = work / hex_id(id);
        r.step("Unpacking " + version_name(id));
        if (!r.run(quote(k.ctrx.string()) + " --code-only " + quote(path) + " " + quote(out.string())))
            return r.fail(binary, "Couldn't unpack " + path + ".");
        unpacked.push_back(out);
    }
    r.step("Recompiling to C");
    std::string emit = quote(k.recomp.string()) + " emit " + quote(unpacked[0].string()) + " " + quote(gen.string());
    for (size_t i = 1; i < unpacked.size(); i++) emit += " " + quote(unpacked[i].string());
    if (!r.run(emit)) return r.fail(binary, "The recompiler stopped.");

    r.step("Compiling (this takes a while)");
    if (k.msvc && !enter_msvc(r, k)) return r.fail(binary, "Visual Studio's compiler couldn't be set up (" + k.cc + ").");
    std::vector<fs::path> sources;
    for (auto &e : fs::directory_iterator(gen, ec))
        if (e.path().extension() == ".c") sources.push_back(e.path());
    sources.push_back(k.sdk / "module.c");
    sources.push_back(k.sdk / "arm_ops.c");
    std::sort(sources.begin(), sources.end());
    // VFP multiply-accumulate rounds the product separately (no FMA contraction); signed arithmetic
    // wraps like the ARM instructions it came from
    std::string includes = " -I" + quote(k.sdk.string()) + " -I" + quote(gen.string());
    auto compile = [&](const fs::path &src, const fs::path &o) {
        if (k.msvc)
            return "cl /nologo /c /O2 /fp:precise /w /I" + quote(k.sdk.string()) + " /I" + quote(gen.string()) +
                   " " + quote(src.string()) + " /Fo" + quote(o.string());
        return quote(k.cc) + " -c -O2 -fPIC -fvisibility=hidden -ffp-contract=off -fwrapv -w" + includes + " " +
               quote(src.string()) + " -o " + quote(o.string());
    };
    std::vector<fs::path> objects;
    for (auto &src : sources) objects.push_back(obj / (src.stem().string() + (k.msvc ? ".obj" : ".o")));
    std::atomic<size_t> next{0}, done{0};
    std::atomic<bool> failed{false};
    unsigned workers = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (unsigned w = 0; w < workers; w++)
        pool.emplace_back([&] {
            for (size_t i; !failed && (i = next++) < sources.size();) {
                if (!r.run(compile(sources[i], objects[i]))) failed = true;
                size_t n = ++done;
                std::lock_guard lk(r.job->lock);
                r.job->progress = float(n) / float(sources.size());
                r.job->last_line = "Compiled " + sources[i].filename().string();
            }
        });
    for (auto &t : pool) t.join();
    if (failed) return r.fail(binary, "Compiling stopped (see build.log).");

    r.step("Linking");
    fs::path rsp = work / "objects.rsp";
    {
        std::ofstream list(rsp);
        for (auto &o : objects) list << quote(o.generic_string()) << "\n";
    }
    fs::path out = game_code::module_path(binary), staged = work / out.filename();
    fs::create_directories(out.parent_path(), ec);
    std::string link = k.msvc ? "link /nologo /DLL /OUT:" + quote(staged.string()) + " @" + quote(rsp.string())
#if defined(__APPLE__)
                              : quote(k.cc) + " -dynamiclib -o " + quote(staged.string()) + " @" + quote(rsp.string());
#else
                              : quote(k.cc) + " -shared -o " + quote(staged.string()) + " @" + quote(rsp.string()) + " -lm";
#endif
    if (!r.run(link)) return r.fail(binary, "Linking stopped (see build.log).");
    fs::remove(out, ec);
    fs::rename(staged, out, ec);
    if (ec) return r.fail(binary, "Couldn't put the game in " + out.string() + ": " + ec.message());
    write_marker(module_marker(binary), true, dumps);
    fs::remove_all(work, ec);
    r.succeed(binary);
}

} // namespace

bool can_build_games(std::string *why) {
    const Builder &b = builder();
    if (why) *why = b.why;
    return b.kit || b.tree;
}

bool needs_build(const KnownDump &dump) {
    const Builder &b = builder();
    if (b.kit) {
        auto ids = read_marker(module_marker(dump.binary), true);
        return !module_available(dump.binary) || !ids || !ids->count(dump.program_id);
    }
    if (!binary_available(dump.binary)) return true;
    if (!b.tree) return false;
    auto ids = read_marker(marker(*b.tree, dump.binary), false);
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
            if (const KnownDump *k = find_dump(id); k && !version_label(*k).empty())
                versions += (versions.empty() ? "" : ", ") + version_label(*k);
        if (!versions.empty()) title += ": " + versions;
    }
    const Builder &b = builder();
    {
        std::lock_guard lk(job->lock);
        job->running = true, job->finished = job->ok = false;
        job->binary = binary, job->title = title, job->error.clear(), job->last_line.clear();
        job->step_index = 0, job->step_count = int(dumps.size()) + 3, job->progress = -1;
        job->log_path = (platform::data_dir() / "build.log").string();
    }
    std::thread([job, binary, dumps, &b] {
        Run r{job, std::ofstream(job->log_path), {}};
        if (b.kit) return build_module(r, *b.kit, binary, dumps);
        if (b.tree) return build_executable(r, *b.tree, binary, dumps);
        r.fail(binary, b.why);
    }).detach();
}

} // namespace frontend
