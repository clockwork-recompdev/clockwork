/*
 * Contract between generated code (tools/recomp output) and the runtime.
 *
 * Guest state lives in a Ctx. Guest memory is a 4 GiB reserved block at `recomp_mem`,
 * so a guest address is a plain offset. All of this is portable C17: no inline asm,
 * no intrinsics, no compiler extensions beyond what clang and MSVC both accept.
 */
#ifndef RECOMP_H
#define RECOMP_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(_MSC_VER) || defined(__cplusplus)
#define RECOMP_RESTRICT __restrict
#else
#define RECOMP_RESTRICT restrict
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Ctx {
    uint32_t r[16];
    /* condition flags, each 0 or 1 */
    uint32_t n, z, c, v;
    /* sticky saturation flag and the four GE bits (bit i = GE[i]) */
    uint32_t q, ge;
    /* VFP: s0-s31; d[n] is s[2n] (low word) and s[2n+1] (high word) */
    uint32_t s[32];
    uint32_t fpscr;
    /* thread-local storage pointer returned by `mrc p15, 0, rX, c13, c0, 3` */
    uint32_t tls;
} Ctx;

typedef void (*RecompFn)(Ctx *RECOMP_RESTRICT ctx);

/* every recompiled function by guest address, sorted (generated lookup.c) */
typedef struct RecompEntry {
    uint32_t addr;
    RecompFn fn;
} RecompEntry;
extern const RecompEntry recomp_functions[];
extern const size_t recomp_function_count;

extern uint8_t *recomp_mem;

/* runtime entry points used by generated code */
void recomp_svc(Ctx *ctx, uint32_t imm, uint32_t addr);
/* call or tail-jump to a computed address; aborts with the address if it wasn't recompiled */
void recomp_call_indirect(Ctx *ctx, uint32_t target);
/* unsupported instruction reached (e.g. privileged ops); never returns */
void recomp_unhandled(Ctx *ctx, uint32_t addr, const char *what);
void recomp_coproc(Ctx *ctx, uint32_t addr, uint32_t raw);

/* records each function entry in a per-thread ring buffer, printed on fatal errors;
   compiled out unless RECOMP_TRACE_CALLS is defined */
#ifdef RECOMP_TRACE_CALLS
void recomp_trace_enter(const Ctx *ctx, uint32_t addr);
#define RECOMP_ENTER(ctx, addr) recomp_trace_enter(ctx, addr)
#else
#define RECOMP_ENTER(ctx, addr) ((void)0)
#endif

/* ---- memory (little-endian host, unaligned-safe) ---- */

static inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, recomp_mem + a, 4); return v; }
static inline uint32_t rd16(uint32_t a) { uint16_t v; memcpy(&v, recomp_mem + a, 2); return v; }
static inline uint32_t rd8(uint32_t a) { return recomp_mem[a]; }
static inline uint32_t rds16(uint32_t a) { return (uint32_t)(int32_t)(int16_t)rd16(a); }
static inline uint32_t rds8(uint32_t a) { return (uint32_t)(int32_t)(int8_t)rd8(a); }
static inline void wr32(uint32_t a, uint32_t v) { memcpy(recomp_mem + a, &v, 4); }
static inline void wr16(uint32_t a, uint32_t v) { uint16_t h = (uint16_t)v; memcpy(recomp_mem + a, &h, 2); }
static inline void wr8(uint32_t a, uint32_t v) { recomp_mem[a] = (uint8_t)v; }

/* ---- shifts by register (amount = low byte of the register) ---- */

static inline uint32_t lsl_r(uint32_t v, uint32_t n) { n &= 0xFF; return n >= 32 ? 0 : v << n; }
static inline uint32_t lsr_r(uint32_t v, uint32_t n) { n &= 0xFF; return n >= 32 ? 0 : v >> n; }
static inline uint32_t asr_r(uint32_t v, uint32_t n) {
    n &= 0xFF;
    return (uint32_t)((int32_t)v >> (n >= 32 ? 31 : n));
}
static inline uint32_t ror_r(uint32_t v, uint32_t n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }

/* carry-out variants: *c is left unchanged for a zero shift */
static inline uint32_t lsl_rc(uint32_t v, uint32_t n, uint32_t *c) {
    n &= 0xFF;
    if (n == 0) return v;
    if (n < 32) { *c = (v >> (32 - n)) & 1; return v << n; }
    *c = n == 32 ? v & 1 : 0;
    return 0;
}
static inline uint32_t lsr_rc(uint32_t v, uint32_t n, uint32_t *c) {
    n &= 0xFF;
    if (n == 0) return v;
    if (n < 32) { *c = (v >> (n - 1)) & 1; return v >> n; }
    *c = n == 32 ? v >> 31 : 0;
    return 0;
}
static inline uint32_t asr_rc(uint32_t v, uint32_t n, uint32_t *c) {
    n &= 0xFF;
    if (n == 0) return v;
    if (n < 32) { *c = (v >> (n - 1)) & 1; return (uint32_t)((int32_t)v >> n); }
    *c = v >> 31;
    return (uint32_t)((int32_t)v >> 31);
}
static inline uint32_t ror_rc(uint32_t v, uint32_t n, uint32_t *c) {
    n &= 0xFF;
    if (n == 0) return v;
    n &= 31;
    if (n == 0) { *c = v >> 31; return v; }
    v = (v >> n) | (v << (32 - n));
    *c = v >> 31;
    return v;
}

/* ---- arithmetic with flags ---- */

/* a + b + carry_in; sets *c and *v */
static inline uint32_t add_cv(uint32_t a, uint32_t b, uint32_t cin, uint32_t *c, uint32_t *v) {
    uint64_t wide = (uint64_t)a + b + cin;
    uint32_t r = (uint32_t)wide;
    *c = (uint32_t)(wide >> 32);
    *v = ((a ^ r) & (b ^ r)) >> 31;
    return r;
}

/* ---- saturation ---- */

static inline uint32_t sat_s32(int64_t x, uint32_t *q) {
    if (x > INT32_MAX) { *q = 1; return (uint32_t)INT32_MAX; }
    if (x < INT32_MIN) { *q = 1; return (uint32_t)INT32_MIN; }
    return (uint32_t)(int32_t)x;
}
/* signed saturate to `bits` bits (1..32) */
static inline uint32_t ssat(int32_t x, unsigned bits, uint32_t *q) {
    int64_t hi = ((int64_t)1 << (bits - 1)) - 1, lo = -((int64_t)1 << (bits - 1));
    if (x > hi) { *q = 1; return (uint32_t)hi; }
    if (x < lo) { *q = 1; return (uint32_t)lo; }
    return (uint32_t)x;
}
/* unsigned saturate to `bits` bits (0..31) */
static inline uint32_t usat(int32_t x, unsigned bits, uint32_t *q) {
    int64_t hi = ((int64_t)1 << bits) - 1;
    if (x > hi) { *q = 1; return (uint32_t)hi; }
    if (x < 0) { *q = 1; return 0; }
    return (uint32_t)x;
}

/* ---- packed / SIMD-within-a-register ops ---- */

uint32_t recomp_parallel(Ctx *ctx, int prefix, int op, uint32_t a, uint32_t b);
uint32_t recomp_usad8(uint32_t a, uint32_t b);

static inline uint32_t sel(uint32_t ge, uint32_t a, uint32_t b) {
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) r |= (((ge >> i) & 1) ? a : b) & (0xFFu << (8 * i));
    return r;
}

static inline uint32_t clz32(uint32_t v) {
    uint32_t n = 0;
    if (!v) return 32;
    while (!(v & 0x80000000u)) { v <<= 1; n++; }
    return n;
}

static inline uint32_t rev32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

/* ---- VFP ---- */

static inline float get_f32(const Ctx *ctx, int n) { float f; memcpy(&f, &ctx->s[n], 4); return f; }
static inline void set_f32(Ctx *ctx, int n, float f) { memcpy(&ctx->s[n], &f, 4); }
static inline double get_f64(const Ctx *ctx, int n) {
    uint64_t bits = (uint64_t)ctx->s[2 * n] | ((uint64_t)ctx->s[2 * n + 1] << 32);
    double d;
    memcpy(&d, &bits, 8);
    return d;
}
static inline void set_f64(Ctx *ctx, int n, double d) {
    uint64_t bits;
    memcpy(&bits, &d, 8);
    ctx->s[2 * n] = (uint32_t)bits;
    ctx->s[2 * n + 1] = (uint32_t)(bits >> 32);
}

/* VCMP: sets FPSCR NZCV (unordered = 0011) */
static inline void vfp_cmp(Ctx *ctx, double a, double b) {
    uint32_t nzcv;
    if (a != a || b != b) nzcv = 0x3;
    else if (a == b) nzcv = 0x6;
    else if (a < b) nzcv = 0x8;
    else nzcv = 0x2;
    ctx->fpscr = (ctx->fpscr & 0x0FFFFFFFu) | (nzcv << 28);
}

/* float -> int with ARM semantics: NaN -> 0, out of range saturates */
static inline uint32_t vfp_to_s32(double x, int round_zero) {
    if (x != x) return 0;
    x = round_zero ? trunc(x) : nearbyint(x);
    if (x >= 2147483647.0) return 0x7FFFFFFFu;
    if (x <= -2147483648.0) return 0x80000000u;
    return (uint32_t)(int32_t)x;
}
static inline uint32_t vfp_to_u32(double x, int round_zero) {
    if (x != x) return 0;
    x = round_zero ? trunc(x) : nearbyint(x);
    if (x >= 4294967295.0) return 0xFFFFFFFFu;
    if (x <= 0.0) return 0;
    return (uint32_t)x;
}

#ifdef __cplusplus
}
#endif

#endif
