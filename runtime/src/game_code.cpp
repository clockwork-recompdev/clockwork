#include "game_code.h"

#include "log.h"
#include "platform.h"
#include "recomp_module.h"

#include <SDL3/SDL.h>

extern "C" void recomp_svc(Ctx *ctx, uint32_t imm, uint32_t addr);
extern "C" void recomp_trace_enter(const Ctx *ctx, uint32_t addr);

namespace game_code {
namespace {
const RecompEntry *loaded_functions = nullptr;
size_t loaded_count = 0;
} // namespace

bool linked() { return clockwork_linked_functions != nullptr; }

const RecompEntry *functions() { return linked() ? clockwork_linked_functions : loaded_functions; }
size_t function_count() { return linked() ? *clockwork_linked_function_count : loaded_count; }

std::filesystem::path module_path(const std::string &binary) {
#if defined(_WIN32)
    const char *ext = ".dll";
#elif defined(__APPLE__)
    const char *ext = ".dylib";
#else
    const char *ext = ".so";
#endif
    return platform::data_dir() / "modules" / (binary + ext);
}

bool load_module(const std::string &binary, std::string *why) {
    std::string path = module_path(binary).string();
    SDL_SharedObject *so = SDL_LoadObject(path.c_str());
    if (!so) {
        *why = "Couldn't load " + path + ": " + SDL_GetError();
        return false;
    }
    auto entry = reinterpret_cast<ClockworkModuleEntry>(SDL_LoadFunction(so, CLOCKWORK_MODULE_ENTRY));
    if (!entry) {
        *why = path + " isn't a Clockwork game module.";
        return false;
    }
    const ClockworkHost host = {CLOCKWORK_MODULE_VERSION, recomp_mem,    recomp_svc,        recomp_call_indirect,
                                recomp_unhandled,         recomp_coproc, recomp_trace_enter};
    const ClockworkGame *game = entry(&host);
    if (!game) {
        *why = path + " was built for another version of Clockwork: add the dump again to rebuild it.";
        return false;
    }
    loaded_functions = game->functions, loaded_count = game->function_count;
    LOG("game code: %s (%zu functions)", path.c_str(), loaded_count);
    return true; // stays loaded for the process's life
}

} // namespace game_code
