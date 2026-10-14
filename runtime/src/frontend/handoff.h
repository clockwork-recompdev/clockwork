#pragma once
// Handing a dump to the executable that has its game's recompiled code: each game is built as its
// own executable next to the others, and any of them can show the launcher. The clockwork program
// plays every game itself, from the game modules built in the data folder.
#include "games.h"

#include <string>

extern "C" const char *const clockwork_binary; // this executable's name (generated per game by CMake)

namespace frontend {

// whether executable `name` is built next to this one, or (in the clockwork program, which has no
// game of its own) its game module is built
bool binary_available(const char *name);
// whether this is the clockwork program and `name`'s game module is built, so it plays here
bool module_available(const char *name);

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
