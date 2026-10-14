#include "handoff.h"

#include "dump.h"
#include "log.h"

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

bool binary_available(const char *name) {
    if (std::strcmp(name, clockwork_binary) == 0) return true;
    std::error_code ec;
    return std::filesystem::is_regular_file(binary_path(name), ec);
}

Route route(const std::string &dump_path) {
    auto id = peek_program_id(dump_path);
    if (!id) return {Route::UNREADABLE};
    const KnownDump *d = find_dump(*id);
    if (!d) return {Route::UNKNOWN};
    if (std::strcmp(d->binary, clockwork_binary) == 0) return {Route::HERE, d};
    return {binary_available(d->binary) ? Route::OTHER : Route::NOT_BUILT, d};
}

std::string route_problem(const Route &r) {
    char buf[256];
    switch (r.kind) {
    case Route::UNREADABLE: return "That file isn't a decrypted .3ds, .cci or .cxi dump.";
    case Route::UNKNOWN: return "Clockwork doesn't recognize that game (or that region of it) yet.";
    case Route::NOT_BUILT: {
        const GameInfo *g = find_game(r.dump->game);
        std::snprintf(buf, sizeof buf, "%s%s%s isn't built: its executable (%s) is missing.", g ? g->title : "?",
                      *r.dump->version ? ": " : "", r.dump->version, r.dump->binary);
        return buf;
    }
    default: return "";
    }
}

void play_in(const char *name, const std::string &dump_path) {
    std::string exe = binary_path(name).string();
    LOG("handing %s to %s", dump_path.c_str(), exe.c_str());
    std::fflush(nullptr);
#ifdef _WIN32
    // quoted, as the C runtime joins the arguments into one command line
    std::string q_exe = "\"" + exe + "\"", q_dump = "\"" + dump_path + "\"";
    const char *argv[] = {q_exe.c_str(), "--play", q_dump.c_str(), nullptr};
    _execv(exe.c_str(), argv);
#else
    const char *argv[] = {exe.c_str(), "--play", dump_path.c_str(), nullptr};
    execv(exe.c_str(), const_cast<char *const *>(argv));
#endif
    LOG("could not start %s", exe.c_str());
}

} // namespace frontend
