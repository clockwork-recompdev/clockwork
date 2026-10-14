#pragma once
// Handing a dump to the executable that has its game's recompiled code: each game is built as its
// own executable next to the others, and any of them can show the launcher.
#include "games.h"

#include <string>

extern "C" const char *const clockwork_binary; // this executable's name (generated per game by CMake)

namespace frontend {

// whether executable `name` is built next to this one
bool binary_available(const char *name);

// what to do with a dump
struct Route {
    enum Kind { HERE, OTHER, UNKNOWN, NOT_BUILT, UNREADABLE } kind;
    const KnownDump *dump = nullptr; // when recognized
};
Route route(const std::string &dump_path);

// a sentence saying why a dump can't be played (UNKNOWN, NOT_BUILT, UNREADABLE)
std::string route_problem(const Route &r);

// replaces this process with executable `name`, playing `dump_path` without the launcher; returns
// only if that fails
void play_in(const char *name, const std::string &dump_path);

} // namespace frontend
