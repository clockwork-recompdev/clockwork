#include "mods.h"

#include "dump.h"
#include "level5.h"
#include "log.h"
#include "platform.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <vector>

namespace fs = std::filesystem;

namespace mods {

fs::path game_folder(const std::string &game) { return platform::data_dir() / "mods" / game; }

namespace {
std::vector<fs::path> textures;
std::string current_game;
} // namespace

const std::vector<fs::path> &texture_folders() { return textures; }
const std::string &game() { return current_game; }

void apply(Dump &dump, const std::string &game, uint64_t program_id) {
    std::error_code ec;
    fs::create_directories(platform::data_dir() / "mods", ec); // so it's there to find

    // the game's enabled mods, in name order
    std::vector<fs::path> enabled;
    if (!game.empty())
        for (auto &e : fs::directory_iterator(game_folder(game), ec))
            if (e.is_directory(ec) && !fs::exists(e.path() / "disabled", ec)) enabled.push_back(e.path());
    std::sort(enabled.begin(), enabled.end());
    current_game = game;
    for (const fs::path &mod : enabled)
        if (fs::is_directory(mod / "textures", ec)) textures.push_back(mod / "textures");

    // include/ (and loose/): files inside archives, later mods winning
    std::map<std::string, std::pair<std::string, uint64_t>> inner; // archive path -> (host file, size)
    std::map<std::string, std::string> from;                       // archive path -> mod, for conflicts
    auto take = [&](const std::string &path, const fs::path &file, const fs::path &mod) {
        if (auto prev = from.find(path); prev != from.end() && prev->second != mod.filename().string())
            LOG("mods: %s and %s both change %s; using %s", prev->second.c_str(), mod.filename().string().c_str(),
                path.c_str(), mod.filename().string().c_str());
        inner[path] = {file.string(), uint64_t(fs::file_size(file, ec))};
        from[path] = mod.filename().string();
    };
    bool any_inner = false;
    for (const fs::path &mod : enabled)
        any_inner |= fs::is_directory(mod / "include", ec) || fs::is_directory(mod / "loose", ec);
    if (any_inner) {
        // the game's archives, and every path in them
        struct Archive {
            std::string path;
            Dump::RomfsFile file;
            level5::Arc0 arc;
        };
        std::vector<Archive> archives;
        for (const std::string &path : dump.romfs_paths()) {
            if (path.size() < 3 || path.compare(path.size() - 3, 3, ".fa") != 0) continue;
            auto file = dump.romfs_file(path);
            if (!file) continue;
            auto read = [&](uint64_t off, void *out, uint64_t n) {
                return off + n <= file->size && dump.read_romfs(file->offset + off, out, n) == n;
            };
            if (auto arc = level5::read_arc0(read, file->size)) archives.push_back({path, *file, std::move(*arc)});
            else LOG("mods: %s isn't an archive Clockwork can read", path.c_str());
        }
        std::map<std::string, std::vector<std::string>> by_name; // file name -> paths with it (each once)
        for (const Archive &a : archives)
            for (auto &[p, f] : a.arc.files) {
                auto &v = by_name[p.substr(p.rfind('/') + 1)];
                if (std::find(v.begin(), v.end(), p) == v.end()) v.push_back(p);
            }

        for (const fs::path &mod : enabled) {
            fs::path inc = mod / "include";
            for (auto &e : fs::recursive_directory_iterator(inc, ec))
                if (e.is_regular_file(ec)) take(fs::relative(e.path(), inc, ec).generic_string(), e.path(), mod);
            // loose/: game files without their full path; matched to the one archive path ending with
            // theirs (dropping leading folders the mod added, like its own name), or skipped
            fs::path loose = mod / "loose";
            for (auto &e : fs::recursive_directory_iterator(loose, ec)) {
                if (!e.is_regular_file(ec)) continue;
                std::string rel = fs::relative(e.path(), loose, ec).generic_string();
                auto named = by_name.find(e.path().filename().string());
                std::vector<std::string> match;
                for (std::string tail = rel; named != by_name.end() && match.empty();) {
                    for (const std::string &p : named->second)
                        if (p == tail || (p.size() > tail.size() && p.compare(p.size() - tail.size(), tail.size(), tail) == 0 &&
                                          p[p.size() - tail.size() - 1] == '/'))
                            match.push_back(p);
                    size_t slash = tail.find('/');
                    if (slash == std::string::npos) break;
                    tail = tail.substr(slash + 1);
                }
                if (match.size() == 1) take(match[0], e.path(), mod);
                else
                    LOG("mods: %s (from %s) %s", rel.c_str(), mod.filename().string().c_str(),
                        match.empty() ? "isn't in the game's archives" : "matches several files in them; skipped");
            }
        }

        // every archive holding one of those files gets them (the language archives share paths)
        std::map<std::string, bool> found;
        for (const Archive &a : archives) {
            std::map<std::string, std::pair<std::string, uint64_t>> mine;
            for (auto &[p, src] : inner)
                if (a.arc.files.count(p)) mine[p] = src, found[p] = true;
            if (mine.empty()) continue;
            dump.replace_romfs_file(a.path, level5::arc0_patch(a.arc, mine));
            LOG("mods: %zu file%s replaced in %s", mine.size(), mine.size() == 1 ? "" : "s", a.path.c_str());
        }
        for (auto &[p, src] : inner)
            if (!found.count(p)) LOG("mods: %s (from %s) isn't in any of the game's archives", p.c_str(), from[p].c_str());
    }

    // romfs/: whole files, in order, so later mods win
    for (const fs::path &mod : enabled)
        if (fs::is_directory(mod / "romfs", ec)) {
            int n = dump.apply_romfs_mod(mod / "romfs");
            LOG("mods: %s: %d RomFS file%s replaced", mod.filename().string().c_str(), n, n == 1 ? "" : "s");
        }

    // one version's own romfs/ (a translation for that release), last
    char id[17];
    std::snprintf(id, sizeof id, "%016llx", (unsigned long long)program_id);
    fs::path own = platform::data_dir() / "mods" / id;
    if (fs::is_directory(own / "romfs", ec)) {
        int n = dump.apply_romfs_mod(own / "romfs");
        LOG("mods: %d RomFS file%s replaced from %s", n, n == 1 ? "" : "s", (own / "romfs").string().c_str());
    }
    for (const char *code : {"code.bin", "code.ips", "exefs"})
        if (fs::exists(own / code, ec))
            LOG("mods: %s changes the game's code, which is recompiled ahead of time; it isn't applied",
                (own / code).string().c_str());
    if (!enabled.empty()) LOG("mods: %zu mod%s on", enabled.size(), enabled.size() == 1 ? "" : "s");
}

} // namespace mods
