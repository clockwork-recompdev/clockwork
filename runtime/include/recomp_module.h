/*
 * A game as a module: its recompiled code compiled on the player's PC into a shared library
 * (.dll/.so/.dylib) that the clockwork program loads, so a packaged release holds no game code.
 * The module is the generated C plus runtime/module/module.c and arm_ops.c; it reaches the runtime
 * only through the ClockworkHost it is given, so it links against nothing but the C library.
 */
#ifndef RECOMP_MODULE_H
#define RECOMP_MODULE_H

#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* bumped whenever ClockworkHost, ClockworkGame or recomp.h's contract changes */
#define CLOCKWORK_MODULE_VERSION 1

typedef struct ClockworkHost {
    uint32_t version;
    uint8_t *mem; /* recomp_mem: the guest's address space, reserved before the module is loaded */
    void (*svc)(Ctx *ctx, uint32_t imm, uint32_t addr);
    void (*call_indirect)(Ctx *ctx, uint32_t target);
    void (*unhandled)(Ctx *ctx, uint32_t addr, const char *what);
    void (*coproc)(Ctx *ctx, uint32_t addr, uint32_t raw);
    void (*trace_enter)(const Ctx *ctx, uint32_t addr);
} ClockworkHost;

typedef struct ClockworkGame {
    uint32_t version;
    const RecompEntry *functions;
    size_t function_count;
} ClockworkGame;

/* the one symbol a module exports: takes the host and returns the game (NULL if the versions
   differ: the module was built for another Clockwork and needs rebuilding) */
#define CLOCKWORK_MODULE_ENTRY "clockwork_module"
typedef const ClockworkGame *(*ClockworkModuleEntry)(const ClockworkHost *host);

#ifdef __cplusplus
}
#endif

#endif
