// PICA200 vertex shader interpreter.
//
// Vertex programs are uploaded by the game at run time through GPU registers, so they are
// executed here per vertex. Semantics follow the hardware (3dbrew, Citra): 0 * inf = 0,
// MIN/MAX pick the second operand on NaN, LOOP increments aL by an unsigned byte.
#include "internal.h"

#include "log.h"

#include <cmath>

namespace pica {
namespace {

// 0 * anything is 0 on the PICA, including 0 * inf
inline float mul(float a, float b) {
    float r = a * b;
    if (std::isnan(r) && !std::isnan(a) && !std::isnan(b)) r = 0.0f;
    return r;
}

inline bool compare(float a, float b, uint32_t op) {
    switch (op) {
    case 0: return a == b;
    case 1: return a != b;
    case 2: return a < b;
    case 3: return a <= b;
    case 4: return a > b;
    case 5: return a >= b;
    default: return true;
    }
}

struct Frame {
    uint32_t final_pc, return_pc, loop_pc;
    uint8_t repeat, increment;
    bool loop;
};

} // namespace

void run_shader(const ShaderSetup &s, uint32_t entry, const Vec4 in[16], Vec4 out[16]) {
    Vec4 tmp[16] = {};
    int32_t addr[3] = {0, 0, 0}; // a0.x, a0.y, aL
    bool cond[2] = {false, false};
    Frame stack[16];
    int sp = 0;
    uint32_t pc = entry;
    static const Vec4 ones = {{1, 1, 1, 1}};

    auto source = [&](uint32_t reg, int32_t offset) -> const Vec4 & {
        if (reg < 0x10) return in[reg];
        if (reg < 0x20) return tmp[reg - 0x10];
        int32_t index = int32_t(reg - 0x20) + offset;
        return index >= 0 && index < 96 ? s.f[index] : ones;
    };
    auto swizzle = [](const Vec4 &v, uint32_t selector, bool negate) {
        Vec4 r;
        for (int i = 0; i < 4; i++) {
            float x = v[(selector >> (2 * (3 - i))) & 3];
            r[i] = negate ? -x : x;
        }
        return r;
    };
    auto evaluate = [&](uint32_t ins) {
        bool x = ((ins >> 25) & 1) == cond[0], y = ((ins >> 24) & 1) == cond[1];
        switch ((ins >> 22) & 3) {
        case 0: return x || y;
        case 1: return x && y;
        case 2: return x;
        default: return y;
        }
    };
    auto push = [&](Frame f) {
        if (sp < 16) stack[sp++] = f;
    };

    for (int steps = 0; steps < 0x40000; steps++) {
        while (sp > 0 && pc == stack[sp - 1].final_pc) {
            Frame &f = stack[sp - 1];
            if (f.loop) {
                addr[2] += f.increment;
                if (f.repeat-- == 0) {
                    pc = f.return_pc;
                    sp--;
                } else {
                    pc = f.loop_pc;
                }
            } else {
                pc = f.return_pc;
                sp--;
            }
        }
        uint32_t ins = s.code[pc & 4095];
        uint32_t op = ins >> 26;

        // arithmetic: operands, swizzles and the destination mask come from the descriptor
        if (op < 0x20 || op >= 0x2E) {
            uint32_t s1, s2, s3 = 0, dest, desc;
            int32_t off1 = 0, off2 = 0, off3 = 0;
            if (op >= 0x30) { // MAD / MADI
                desc = s.swizzle[ins & 0x1F];
                uint32_t idx = (ins >> 22) & 3;
                int32_t a = idx ? addr[idx - 1] : 0;
                dest = (ins >> 24) & 0x1F;
                s1 = (ins >> 17) & 0x1F;
                if (op >= 0x38) {
                    s2 = (ins >> 10) & 0x7F, s3 = (ins >> 5) & 0x1F, off2 = a;
                } else {
                    s2 = (ins >> 12) & 0x1F, s3 = (ins >> 5) & 0x7F, off3 = a;
                }
            } else {
                desc = s.swizzle[ins & 0x7F];
                uint32_t idx = (ins >> 19) & 3;
                int32_t a = idx ? addr[idx - 1] : 0;
                dest = (ins >> 21) & 0x1F;
                if (op >= 0x18 && op <= 0x1B) { // inverted forms: the wide operand is src2
                    s1 = (ins >> 14) & 0x1F, s2 = (ins >> 7) & 0x7F, off2 = a;
                } else {
                    s1 = (ins >> 12) & 0x7F, s2 = (ins >> 7) & 0x1F, off1 = a;
                }
            }
            Vec4 a = swizzle(source(s1, off1), (desc >> 5) & 0xFF, (desc >> 4) & 1);
            Vec4 b = swizzle(source(s2, off2), (desc >> 14) & 0xFF, (desc >> 13) & 1);
            uint32_t mask = desc & 0xF;
            Vec4 r = a;

            switch (op) {
            case 0x00: // ADD
                for (int i = 0; i < 4; i++) r[i] = a[i] + b[i];
                break;
            case 0x01: { // DP3
                float d = mul(a[0], b[0]) + mul(a[1], b[1]) + mul(a[2], b[2]);
                r = {{d, d, d, d}};
                break;
            }
            case 0x02: { // DP4
                float d = mul(a[0], b[0]) + mul(a[1], b[1]) + mul(a[2], b[2]) + mul(a[3], b[3]);
                r = {{d, d, d, d}};
                break;
            }
            case 0x03:   // DPH
            case 0x18: { // DPHI
                float d = mul(a[0], b[0]) + mul(a[1], b[1]) + mul(a[2], b[2]) + b[3];
                r = {{d, d, d, d}};
                break;
            }
            case 0x04: // DST
            case 0x19: // DSTI
                r = {{1.0f, mul(a[1], b[1]), a[2], b[3]}};
                break;
            case 0x05: { // EX2
                float d = std::exp2(a[0]);
                r = {{d, d, d, d}};
                break;
            }
            case 0x06: { // LG2
                float d = std::log2(a[0]);
                r = {{d, d, d, d}};
                break;
            }
            case 0x07: // LITP: clamp inputs for a lighting power computation
                r = {{std::fmax(a[0], 0.0f), std::fmin(std::fmax(a[1], -127.9961f), 127.9961f), 0.0f,
                      std::fmax(a[3], 0.0f)}};
                cond[0] = a[0] >= 0.0f, cond[1] = a[3] >= 0.0f;
                break;
            case 0x08: // MUL
                for (int i = 0; i < 4; i++) r[i] = mul(a[i], b[i]);
                break;
            case 0x09: // SGE
            case 0x1A: // SGEI
                for (int i = 0; i < 4; i++) r[i] = a[i] >= b[i] ? 1.0f : 0.0f;
                break;
            case 0x0A: // SLT
            case 0x1B: // SLTI
                for (int i = 0; i < 4; i++) r[i] = a[i] < b[i] ? 1.0f : 0.0f;
                break;
            case 0x0B: // FLR
                for (int i = 0; i < 4; i++) r[i] = std::floor(a[i]);
                break;
            case 0x0C: // MAX
                for (int i = 0; i < 4; i++) r[i] = a[i] > b[i] ? a[i] : b[i];
                break;
            case 0x0D: // MIN
                for (int i = 0; i < 4; i++) r[i] = a[i] < b[i] ? a[i] : b[i];
                break;
            case 0x0E: { // RCP
                float d = 1.0f / a[0];
                r = {{d, d, d, d}};
                break;
            }
            case 0x0F: { // RSQ
                float d = 1.0f / std::sqrt(a[0]);
                r = {{d, d, d, d}};
                break;
            }
            case 0x12: // MOVA
                if (mask & 8) addr[0] = int32_t(a[0]);
                if (mask & 4) addr[1] = int32_t(a[1]);
                mask = 0;
                break;
            case 0x13: // MOV
                break;
            case 0x2E:
            case 0x2F: // CMP
                cond[0] = compare(a[0], b[0], (ins >> 24) & 7);
                cond[1] = compare(a[1], b[1], (ins >> 21) & 7);
                mask = 0;
                break;
            default:
                if (op >= 0x30) { // MAD / MADI
                    Vec4 c = swizzle(source(s3, off3), (desc >> 23) & 0xFF, (desc >> 22) & 1);
                    for (int i = 0; i < 4; i++) r[i] = mul(a[i], b[i]) + c[i];
                } else {
                    static bool warned;
                    if (!warned) LOG("pica: unknown shader opcode %#x at %#x", op, pc);
                    warned = true;
                    mask = 0;
                }
            }
            if (mask) {
                Vec4 &d = dest < 0x10 ? out[dest] : tmp[dest - 0x10];
                for (int i = 0; i < 4; i++)
                    if (mask & (8 >> i)) d[i] = r[i];
            }
            pc++;
            continue;
        }

        // flow control
        uint32_t num = ins & 0xFF, dst = (ins >> 10) & 0xFFF;
        switch (op) {
        case 0x22: // END
            return;
        case 0x20: // BREAK
        case 0x23: // BREAKC
            if (op == 0x20 || evaluate(ins)) {
                int k = sp - 1;
                while (k >= 0 && !stack[k].loop) k--;
                if (k >= 0) {
                    pc = stack[k].return_pc;
                    sp = k;
                    continue;
                }
            }
            break;
        case 0x24: // CALL
        case 0x25: // CALLC
        case 0x26: // CALLU
            if (op == 0x24 || (op == 0x25 && evaluate(ins)) || (op == 0x26 && (s.b >> ((ins >> 22) & 0xF)) & 1)) {
                push({dst + num, pc + 1, 0, 0, 0, false});
                pc = dst;
                continue;
            }
            break;
        case 0x27:   // IFU
        case 0x28: { // IFC
            bool taken = op == 0x27 ? (s.b >> ((ins >> 22) & 0xF)) & 1 : evaluate(ins);
            if (taken) {
                push({dst, dst + num, 0, 0, 0, false});
                pc++;
            } else {
                push({dst + num, dst + num, 0, 0, 0, false});
                pc = dst;
            }
            continue;
        }
        case 0x29: { // LOOP: body is pc+1..dst inclusive, run i.x + 1 times
            const uint8_t *iu = s.i[(ins >> 22) & 3];
            addr[2] = iu[1];
            push({dst + 1, dst + 1, pc + 1, iu[0], iu[2], true});
            pc++;
            continue;
        }
        case 0x2C: // JMPC
            if (evaluate(ins)) {
                pc = dst;
                continue;
            }
            break;
        case 0x2D: // JMPU: jumps when the bool uniform differs from bit 0 of NUM
            if (((s.b >> ((ins >> 22) & 0xF)) & 1) == !(num & 1)) {
                pc = dst;
                continue;
            }
            break;
        default: // NOP, EMIT, SETEMIT (geometry shaders only)
            break;
        }
        pc++;
    }
    static bool warned;
    if (!warned) LOG("pica: vertex shader at %#x did not reach END", entry);
    warned = true;
}

} // namespace pica
