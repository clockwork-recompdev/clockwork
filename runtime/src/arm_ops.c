/* ARMv6 media instructions too long to inline in generated code. */
#include "recomp.h"

/* prefix: 0 S, 1 Q, 2 SH, 3 U, 4 UQ, 5 UH (insn.rs ParallelPrefix order)
 * op:     0 ADD16, 1 ASX, 2 SAX, 3 SUB16, 4 ADD8, 5 SUB8 (insn.rs ParallelOp order) */
enum { P_S, P_Q, P_SH, P_U, P_UQ, P_UH };
enum { O_ADD16, O_ASX, O_SAX, O_SUB16, O_ADD8, O_SUB8 };

static int32_t lane(uint32_t x, int bits, int i, int is_signed) {
    uint32_t v = (x >> (i * bits)) & ((1u << bits) - 1);
    if (is_signed && (v >> (bits - 1))) return (int32_t)v - (1 << bits);
    return (int32_t)v;
}

uint32_t recomp_parallel(Ctx *ctx, int prefix, int op, uint32_t a, uint32_t b) {
    int is_signed = prefix <= P_SH;
    int bits = (op == O_ADD8 || op == O_SUB8) ? 8 : 16;
    int lanes = 32 / bits;
    uint32_t result = 0, ge = 0;
    for (int i = 0; i < lanes; i++) {
        int32_t x = lane(a, bits, i, is_signed), r;
        int add;
        switch (op) {
        case O_ASX: /* lane 0: a.lo - b.hi, lane 1: a.hi + b.lo */
            add = i == 1;
            r = add ? x + lane(b, 16, 0, is_signed) : x - lane(b, 16, 1, is_signed);
            break;
        case O_SAX: /* lane 0: a.lo + b.hi, lane 1: a.hi - b.lo */
            add = i == 0;
            r = add ? x + lane(b, 16, 1, is_signed) : x - lane(b, 16, 0, is_signed);
            break;
        default:
            add = op == O_ADD16 || op == O_ADD8;
            r = add ? x + lane(b, bits, i, is_signed) : x - lane(b, bits, i, is_signed);
        }
        int32_t lo = is_signed ? -(1 << (bits - 1)) : 0;
        int32_t hi = is_signed ? (1 << (bits - 1)) - 1 : (1 << bits) - 1;
        int set_ge;
        switch (prefix) {
        case P_Q: case P_UQ:
            r = r < lo ? lo : r > hi ? hi : r;
            set_ge = 0;
            break;
        case P_SH: case P_UH:
            r >>= 1; /* arithmetic shift of the exact result */
            set_ge = 0;
            break;
        case P_S:
            set_ge = r >= 0;
            break;
        default: /* U: GE is carry for adds, no-borrow for subtracts */
            set_ge = add ? r > hi : r >= 0;
            break;
        }
        if (prefix == P_S || prefix == P_U) {
            uint32_t ge_bits = bits == 16 ? 3u << (2 * i) : 1u << i;
            if (set_ge) ge |= ge_bits;
        }
        result |= ((uint32_t)r & ((1u << bits) - 1)) << (i * bits);
    }
    if (prefix == P_S || prefix == P_U) ctx->ge = ge;
    return result;
}

uint32_t recomp_usad8(uint32_t a, uint32_t b) {
    uint32_t sum = 0;
    for (int i = 0; i < 4; i++) {
        int32_t x = (a >> (8 * i)) & 0xFF, y = (b >> (8 * i)) & 0xFF;
        sum += (uint32_t)(x > y ? x - y : y - x);
    }
    return sum;
}
