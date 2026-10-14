#include "log.h"

#include "recomp.h"

#include <cstdarg>
#include <cstdlib>
#include <cstring>

namespace log_cfg {
bool svc = false;
bool ipc = false;
bool gpu = false;
bool dsp = false;

void init() {
    const char *t = std::getenv("RECOMP_TRACE");
    if (!t) return;
    svc = std::strstr(t, "svc") != nullptr;
    ipc = std::strstr(t, "ipc") != nullptr;
    gpu = std::strstr(t, "gpu") != nullptr;
    dsp = std::strstr(t, "dsp") != nullptr;
}
} // namespace log_cfg

namespace {
// last function entries on this thread (RECOMP_TRACE_CALLS builds)
constexpr int TRACE_LEN = 64;
struct TraceEntry {
    uint32_t addr, lr, sp;
};
thread_local TraceEntry trace[TRACE_LEN];
thread_local unsigned trace_pos = 0;

} // namespace

void print_trace() {
    if (!trace_pos) return;
    std::fprintf(stderr, "recent calls on this thread (oldest first):\n");
    unsigned n = trace_pos < TRACE_LEN ? trace_pos : TRACE_LEN;
    for (unsigned i = trace_pos - n; i < trace_pos; i++) {
        const TraceEntry &e = trace[i % TRACE_LEN];
        std::fprintf(stderr, "  %08x  (lr=%08x sp=%08x)\n", e.addr, e.lr, e.sp);
    }
}
extern "C" void recomp_trace_enter(const Ctx *ctx, uint32_t addr) {
    trace[trace_pos++ % TRACE_LEN] = {addr, ctx->r[14], ctx->r[13]};
}

void (*fatal_hook)(const char *message) = nullptr;

void fatal(const char *fmt, ...) {
    std::fflush(stdout);
    print_trace();
    char message[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof message, fmt, args);
    va_end(args);
    std::fprintf(stderr, "fatal: %s\n", message);
    std::fflush(stderr);
    if (fatal_hook) fatal_hook(message);
    // _Exit: other host threads may be blocked inside the kernel, so skip static destructors
    std::_Exit(1);
}
