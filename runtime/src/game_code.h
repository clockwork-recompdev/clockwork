#pragma once
// The recompiled code being run: linked into this executable (a game's own executable, built with
// CMake), or loaded from a game module (the clockwork program of a packaged release, which holds
// no game code; see recomp_module.h).
#include "recomp.h"

#include <filesystem>
#include <string>

// this executable's own recompiled code: set in the per-executable file CMake writes; null in the
// clockwork program
extern "C" const RecompEntry *const clockwork_linked_functions;
extern "C" const size_t *const clockwork_linked_function_count;

namespace game_code {

// every recompiled function by guest address, sorted (in the clockwork program, empty until a
// module is loaded)
const RecompEntry *functions();
size_t function_count();

// whether this executable has a game's code in it
bool linked();

// where executable `binary`'s module is built (in the data folder): modules/<binary>.<so|dll|dylib>
std::filesystem::path module_path(const std::string &binary);

// loads `binary`'s module after the guest memory is reserved; on failure, `why` says why
bool load_module(const std::string &binary, std::string *why);

} // namespace game_code
