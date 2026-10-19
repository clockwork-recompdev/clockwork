#pragma once
// Building a game's executable from its dump, from the launcher: in a source checkout (this program
// was built there, with CMake), adding a dump of a game that isn't built unpacks it, recompiles it
// to C and compiles that game's executable, in the background. Packaged releases can't (no tools).
#include "games.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace frontend {

// whether games can be built here; if not, `why` says what's missing
bool can_build_games(std::string *why = nullptr);

// whether `dump`'s executable needs building: it isn't there, or it was built (by the launcher)
// from other versions' dumps only. An executable built by hand is taken as it is
bool needs_build(const KnownDump &dump);

// A build in the background. Read it under `lock`
struct BuildJob {
    std::mutex lock;
    bool running = false, finished = false, ok = false;
    std::string binary;     // the executable being built
    std::string title;      // "Yo-kai Watch 2: Bony Spirits, Fleshy Souls"
    std::string step;       // what it's doing: "Unpacking Bony Spirits"
    int step_index = 0, step_count = 0;
    float progress = -1;    // within the step, 0-1 (compiling); -1 when unknown
    std::string last_line;  // the tools' latest output
    std::string error;      // when it failed
    std::string log_path;   // everything the tools printed
};

// Builds executable `binary` from `dumps` (program id and path; every version that shares it: the
// path is its update's when the executable is built from updates, see code_binary), on its own
// thread. `job` must not be running
void start_build(const std::shared_ptr<BuildJob> &job, const std::string &binary,
                 std::vector<std::pair<uint64_t, std::string>> dumps);

} // namespace frontend
