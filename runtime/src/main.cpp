#include "dump.h"
#include "frontend/games.h"
#include "mods.h"
#include "frontend/frontend.h"
#include "game_code.h"
#include "kernel/kernel.h"
#include "loader.h"
#include "log.h"
#include "memory.h"
#include "net/air.h"

#include <csignal>
#include <cstring>
#include <cstdlib>

#ifndef _WIN32
#include <dlfcn.h>
#include <execinfo.h>
#include <sys/time.h>
#include <unistd.h>
#if defined(__linux__)
#include <ucontext.h> // the profiler (Linux only)
#endif

#include <algorithm>
#include <atomic>
#include <map>
#endif

namespace {
GameImage game;

// on the game thread: load the dump and run it
void game_main(const char *dump_path) {
    open_dump(dump_path);
    const frontend::KnownDump *known = frontend::find_dump(dump().program_id());
    mods::apply(dump(), known ? known->game : "", dump().program_id());
    mem::init();
    if (!game_code::linked()) { // the clockwork program: the game's code is its module
        std::string why;
        if (!known) fatal("Clockwork doesn't recognize this game.");
        if (!game_code::load_module(known->binary, &why)) fatal("%s", why.c_str());
    }
    game = load_game();
    kernel::init(game);
    if (net::air_wanted()) net::air_start(); // in the room from the start, so the others see us
    kernel::run(game);
}

#ifndef _WIN32
// report faults inside guest memory with the guest address
void on_fault(int sig, siginfo_t *info, void *) {
    uint32_t guest;
    char buf[160];
    int n;
    if (mem::guest_address(info->si_addr, &guest))
        n = std::snprintf(buf, sizeof buf, "fatal: guest memory fault at %08x (unmapped)\n", guest);
    else
        n = std::snprintf(buf, sizeof buf, "fatal: host fault (signal %d) at %p\n", sig, info->si_addr);
    (void)!write(2, buf, size_t(n));
    // not async-signal-safe, but the process is ending and this is what makes faults debuggable
    print_trace();
#if defined(__linux__)
    // where in the runtime it happened: "module(+offset)" lines for addr2line
    void *frames[32];
    int count = backtrace(frames, 32);
    Dl_info self{};
    dladdr(reinterpret_cast<void *>(&on_fault), &self);
    for (int i = 0; i < count; i++) {
        Dl_info d{};
        if (dladdr(frames[i], &d) && d.dli_fbase == self.dli_fbase)
            std::fprintf(stderr, "  host frame %d: yw2+%#lx\n", i,
                         (unsigned long)(uintptr_t(frames[i]) - uintptr_t(self.dli_fbase)));
    }
#endif
    if (fatal_hook) fatal_hook("The game stopped because of an internal error (see the log).");
    _exit(1);
}

void on_quit(int) { kernel::dump_threads(); }

// YW2_PROFILE=<file>: samples the host program counter of whichever thread is using CPU
// (SIGPROF), and on SIGTERM/SIGINT writes "<count> <offset in executable>" lines for
// `addr2line -f -C -e yw2`
#if defined(__linux__) && defined(__x86_64__)
constexpr size_t PROFILE_MAX = 1 << 22;
uintptr_t *profile_samples;
std::atomic<size_t> profile_count{0};
const char *profile_path;

void on_prof(int, siginfo_t *, void *uc) {
    size_t i = profile_count.fetch_add(1, std::memory_order_relaxed);
    if (i < PROFILE_MAX) profile_samples[i] = uintptr_t(static_cast<ucontext_t *>(uc)->uc_mcontext.gregs[REG_RIP]);
}

void write_profile(int) {
    signal(SIGPROF, SIG_IGN);
    Dl_info info{};
    dladdr(reinterpret_cast<void *>(&write_profile), &info);
    uintptr_t base = uintptr_t(info.dli_fbase);
    std::map<uintptr_t, size_t> hist;
    size_t n = std::min(profile_count.load(), PROFILE_MAX);
    for (size_t i = 0; i < n; i++) hist[profile_samples[i]]++;
    // "<count> <offset>" for the game binary; "<count> lib:<library>:<symbol>" for shared libraries
    if (FILE *f = std::fopen(profile_path, "w")) {
        for (auto [pc, c] : hist) {
            Dl_info d{};
            if (dladdr(reinterpret_cast<void *>(pc), &d) && d.dli_fbase != info.dli_fbase) {
                const char *lib = d.dli_fname ? std::strrchr(d.dli_fname, '/') : nullptr;
                std::fprintf(f, "%zu lib:%s:%s:%#lx\n", c, lib ? lib + 1 : "?", d.dli_sname ? d.dli_sname : "?",
                             (unsigned long)(pc - uintptr_t(d.dli_fbase)));
            } else {
                std::fprintf(f, "%zu %#lx\n", c, (unsigned long)(pc - base));
            }
        }
        std::fclose(f);
    }
    _exit(0);
}

void start_profiler() {
    profile_path = std::getenv("YW2_PROFILE");
    if (!profile_path) return;
    profile_samples = new uintptr_t[PROFILE_MAX];
    struct sigaction sa {};
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, nullptr);
    signal(SIGTERM, write_profile);
    signal(SIGINT, write_profile);
    itimerval t{{0, 1000}, {0, 1000}}; // 1 kHz of CPU time
    setitimer(ITIMER_PROF, &t, nullptr);
}
#else
void start_profiler() {}
#endif

void install_fault_handler() {
    signal(SIGQUIT, on_quit); // Ctrl+\ prints the guest threads
    start_profiler();
    struct sigaction sa {};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
}
#else
void install_fault_handler() {}
#endif
} // namespace

int main(int argc, char **argv) {
    log_cfg::init();
    install_fault_handler();
    const char *dump = nullptr;
    bool force_launcher = false, play = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--launcher") == 0) force_launcher = true;
        else if (std::strcmp(argv[i], "--play") == 0) play = true; // straight into the game
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf("Clockwork: Yo-kai Watch, recompiled for PC\n"
                        "usage: %s [--launcher | --play] [your decrypted game dump (.3ds/.cci/.cxi)]\n", argv[0]);
            return 0;
        } else dump = argv[i];
    }
    frontend::run(game_main, dump, force_launcher, play);
}
