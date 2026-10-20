#include "handoff.h"

#include "dump.h"
#include "game_code.h"
#include "log.h"
#include "settings.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <filesystem>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace frontend {
namespace {

std::filesystem::path binary_path(const char *name) {
    const char *base = SDL_GetBasePath(); // this executable's folder, with a trailing separator
    std::string file = name;
#ifdef _WIN32
    file += ".exe";
#endif
    return std::filesystem::path(base ? base : "") / file;
}

} // namespace

namespace {
std::pair<uint64_t, std::string> update_override; // use_update: the game's program id, and its update
} // namespace

bool use_update(const std::string &path) {
    auto id = peek_program_id(path);
    auto game = id ? update_of(*id) : std::nullopt;
    if (!game) return false;
    update_override = {*game, path};
    return true;
}

std::string update_for(uint64_t program_id) {
    if (update_override.first == program_id) return update_override.second;
    const Settings &s = settings();
    auto it = s.updates.find(program_id);
    std::error_code ec;
    if (it == s.updates.end() || !std::filesystem::is_regular_file(it->second, ec)) return "";
    // remembered before it was checked (an e-manual's content shares the update's program id)
    auto id = peek_program_id(it->second);
    return id && update_of(*id) == program_id ? it->second : "";
}

std::string code_binary(const KnownDump &dump, bool updated) { return std::string(dump.binary) + (updated ? "_upd" : ""); }

std::string code_binary(const KnownDump &dump) { return code_binary(dump, !update_for(dump.program_id).empty()); }

bool module_available(const char *name) {
    std::error_code ec;
    return !game_code::linked() && std::filesystem::is_regular_file(game_code::module_path(name), ec);
}

bool binary_available(const char *name) {
    if (std::strcmp(name, clockwork_binary) == 0 || module_available(name)) return true;
    std::error_code ec;
    return std::filesystem::is_regular_file(binary_path(name), ec);
}

Route route(const std::string &dump_path) {
    auto id = peek_program_id(dump_path);
    if (!id) return {Route::UNREADABLE};
    const KnownDump *d = find_dump(*id);
    if (!d) return {Route::UNKNOWN};
    std::string b = code_binary(*d);
    if (b == clockwork_binary || module_available(b.c_str())) return {Route::HERE, d, b};
    return {binary_available(b.c_str()) ? Route::OTHER : Route::NOT_BUILT, d, b};
}

std::filesystem::path executable_for(const Route &r) {
    return binary_path(r.kind == Route::HERE ? clockwork_binary : r.binary.c_str());
}

std::string route_problem(const Route &r) {
    char buf[256];
    switch (r.kind) {
    case Route::UNREADABLE: return "That file isn't a decrypted .3ds, .cci, .cxi or .cia dump.";
    case Route::UNKNOWN: return "Clockwork doesn't recognize that game (or that region of it) yet.";
    case Route::NOT_BUILT: {
        const GameInfo *g = find_game(r.dump->game);
        std::string v = version_label(*r.dump);
        std::snprintf(buf, sizeof buf, "%s%s%s isn't built yet.", g ? g->title : "?", v.empty() ? "" : ": ", v.c_str());
        return buf;
    }
    default: return "";
    }
}

void play_in(const char *name, const std::string &dump_path) {
    std::string exe = binary_path(name).string();
    LOG("handing %s to %s", dump_path.c_str(), exe.c_str());
    std::fflush(nullptr);
    // --update goes along, or the other program falls back to the update in the settings
    auto id = peek_program_id(dump_path);
    std::string update = id && update_override.first == *id ? update_override.second : "";
#ifdef _WIN32
    // quoted, as the C runtime joins the arguments into one command line
    std::string q_exe = "\"" + exe + "\"", q_dump = "\"" + dump_path + "\"", q_update = "\"" + update + "\"";
    const char *argv[] = {q_exe.c_str(), "--play", q_dump.c_str(), nullptr, nullptr, nullptr};
    if (!update.empty()) argv[3] = "--update", argv[4] = q_update.c_str();
    _execv(exe.c_str(), argv);
#else
    const char *argv[] = {exe.c_str(), "--play", dump_path.c_str(), nullptr, nullptr, nullptr};
    if (!update.empty()) argv[3] = "--update", argv[4] = update.c_str();
    execv(exe.c_str(), const_cast<char *const *>(argv));
#endif
    LOG("could not start %s", exe.c_str());
}

} // namespace frontend
