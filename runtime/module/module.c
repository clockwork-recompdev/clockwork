/* Compiled into each game module with its generated code (see recomp_module.h): the runtime
   entry points the generated code calls, forwarded to the host that loaded the module. */
#include "recomp_module.h"

#if defined(_WIN32)
#define MODULE_EXPORT __declspec(dllexport)
#else
#define MODULE_EXPORT __attribute__((visibility("default")))
#endif

uint8_t *recomp_mem;
static ClockworkHost host;

void recomp_svc(Ctx *ctx, uint32_t imm, uint32_t addr) { host.svc(ctx, imm, addr); }
void recomp_call_indirect(Ctx *ctx, uint32_t target) { host.call_indirect(ctx, target); }
void recomp_unhandled(Ctx *ctx, uint32_t addr, const char *what) { host.unhandled(ctx, addr, what); }
void recomp_coproc(Ctx *ctx, uint32_t addr, uint32_t raw) { host.coproc(ctx, addr, raw); }
void recomp_trace_enter(const Ctx *ctx, uint32_t addr) { host.trace_enter(ctx, addr); }

MODULE_EXPORT const ClockworkGame *clockwork_module(const ClockworkHost *h) {
    static ClockworkGame game;
    if (!h || h->version != CLOCKWORK_MODULE_VERSION) return NULL;
    host = *h;
    recomp_mem = h->mem;
    game.version = CLOCKWORK_MODULE_VERSION;
    game.functions = recomp_functions;
    game.function_count = recomp_function_count;
    return &game;
}
