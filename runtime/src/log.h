#pragma once
#include <cstdio>

// printf-style logging to stderr. Use LOG_TRACE for anything per-call that can be noisy.
#define LOG(...) (std::fprintf(stderr, __VA_ARGS__), std::fputc('\n', stderr))

namespace log_cfg {
// set from RECOMP_TRACE (comma-separated: svc, ipc, gpu, dsp)
extern bool svc;
extern bool ipc;
extern bool gpu;
extern bool dsp;
void init();
}

#define LOG_SVC(...) (log_cfg::svc ? (void)LOG("[svc] " __VA_ARGS__) : (void)0)
#define LOG_IPC(...) (log_cfg::ipc ? (void)LOG("[ipc] " __VA_ARGS__) : (void)0)
#define LOG_GPU(...) (log_cfg::gpu ? (void)LOG("[gpu] " __VA_ARGS__) : (void)0)
#define LOG_DSP(...) (log_cfg::dsp ? (void)LOG("[dsp] " __VA_ARGS__) : (void)0)

// prints this thread's recent guest calls (RECOMP_TRACE_CALLS builds)
void print_trace();

// stops the process with a message
[[noreturn]] void fatal(const char *fmt, ...);
// also shows fatal messages (the frontend sets it to show a message box)
extern void (*fatal_hook)(const char *message);
