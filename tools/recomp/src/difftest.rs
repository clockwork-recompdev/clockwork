//! Differential tests: the same instructions, from the same starting state, run on
//! qemu-arm (as real ARM code) and through the C emitter (natively). tools/difftest.py
//! builds and runs both and compares the final states.
//!
//! Instructions are sampled from the game's own discovered code. This covers register-only
//! instructions (data processing, multiplies, media, VFP); memory and control flow need
//! a different harness.

use crate::discover::Analysis;
use crate::emit;
use crate::insn::*;
use std::fmt::Write;
use std::path::Path;

/// Words of state per test: r0-r12, r14, CPSR, s0-s31, FPSCR.
pub const STATE_WORDS: usize = 48;
const CPSR: usize = 14;
const S0: usize = 15;
const FPSCR: usize = 47;

struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u32 {
        // xorshift64*
        self.0 ^= self.0 >> 12;
        self.0 ^= self.0 << 25;
        self.0 ^= self.0 >> 27;
        (self.0.wrapping_mul(0x2545_F491_4F6C_DD1D) >> 32) as u32
    }
    fn below(&mut self, n: u32) -> u32 {
        self.next() % n
    }
}

fn mentions_sp_or_pc(insn: &Insn) -> bool {
    let text = insn.to_string();
    text.split(|c: char| !c.is_ascii_alphanumeric())
        .any(|tok| tok == "sp" || tok == "pc")
}

fn testable(insn: &Insn) -> bool {
    if insn.thumb || mentions_sp_or_pc(insn) {
        return false;
    }
    matches!(
        insn.op,
        Op::DataProc { .. }
            | Op::Mul { .. }
            | Op::MulLong { .. }
            | Op::Umaal { .. }
            | Op::SmulHalf { .. }
            | Op::SmulWord { .. }
            | Op::SmlalHalf { .. }
            | Op::DualMul { .. }
            | Op::DualMulLong { .. }
            | Op::MulHigh { .. }
            | Op::SatArith { .. }
            | Op::Clz { .. }
            | Op::Sat { .. }
            | Op::Sat16 { .. }
            | Op::Extend { .. }
            | Op::Rev { .. }
            | Op::Pkh { .. }
            | Op::Sel { .. }
            | Op::Parallel { .. }
            | Op::Usad8 { .. }
            | Op::VfpBinary { .. }
            | Op::VfpUnary { .. }
            | Op::VfpCmp { .. }
            | Op::VfpCvt { .. }
            | Op::VmovSingle { .. }
            | Op::VmovPair { .. }
            | Op::VmovHalf { .. }
            | Op::Vsys {
                to_core: true,
                rt: PC,
                ..
            }
    )
}

fn interesting_u32(rng: &mut Rng) -> u32 {
    const SPECIAL: [u32; 10] = [
        0,
        1,
        2,
        0x7FFF_FFFF,
        0x8000_0000,
        0xFFFF_FFFF,
        0xFFFF_FFFE,
        0x8000,
        0xFFFF,
        0x7FFF,
    ];
    match rng.below(4) {
        0 => SPECIAL[rng.below(SPECIAL.len() as u32) as usize],
        1 => rng.below(64),
        _ => rng.next(),
    }
}

fn interesting_f32(rng: &mut Rng) -> u32 {
    const SPECIAL: [f32; 9] = [
        0.0,
        -0.0,
        1.0,
        -1.0,
        0.5,
        f32::INFINITY,
        f32::NEG_INFINITY,
        1e-40,
        3.0e9,
    ];
    match rng.below(5) {
        0 => SPECIAL[rng.below(SPECIAL.len() as u32) as usize].to_bits(),
        1 => ((rng.next() as i32 as f32) / 65536.0).to_bits(),
        2 => (rng.below(2001) as f32 - 1000.0).to_bits(),
        _ => ((rng.next() as f32 / u32::MAX as f32) * 200.0 - 100.0).to_bits(),
    }
}

fn random_state(rng: &mut Rng) -> [u32; STATE_WORDS] {
    let mut s = [0u32; STATE_WORDS];
    for w in s.iter_mut().take(CPSR) {
        *w = interesting_u32(rng);
    }
    // NZCVQ and GE bits
    s[CPSR] = (rng.next() & 0xF800_0000) | ((rng.below(16)) << 16);
    if rng.below(2) == 0 {
        for w in &mut s[S0..S0 + 32] {
            *w = interesting_f32(rng);
        }
    } else {
        // doubles across register pairs
        for pair in s[S0..S0 + 32].chunks_mut(2) {
            let d = match rng.below(4) {
                0 => (rng.next() as i32 as f64) / 1024.0,
                1 => [0.0, -0.0, 1.0, f64::INFINITY, 1e300, -2.5][rng.below(6) as usize],
                _ => (rng.next() as f64 / u32::MAX as f64) * 2000.0 - 1000.0,
            };
            let bits = d.to_bits();
            pair[0] = bits as u32;
            pair[1] = (bits >> 32) as u32;
        }
    }
    // round to nearest, no flush-to-zero, no default NaN
    s[FPSCR] = 0;
    s
}

pub fn generate(analysis: &Analysis, out: &Path, count: usize, states: usize, seed: u64) -> Result<usize, String> {
    let mut candidates: Vec<Insn> = analysis
        .functions
        .values()
        .flat_map(|f| f.insns.values().copied())
        .filter(testable)
        .collect();
    candidates.sort_by_key(|i| i.raw);
    candidates.dedup_by_key(|i| i.raw);

    // spread picks evenly over mnemonics so rare instructions get tested as much as `add`
    let mut groups: std::collections::BTreeMap<String, Vec<Insn>> = std::collections::BTreeMap::new();
    for insn in candidates {
        let text = insn.to_string();
        let mnemonic = text.split_whitespace().next().unwrap_or_default().to_string();
        groups.entry(mnemonic).or_default().push(insn);
    }
    let mut groups: Vec<Vec<Insn>> = groups.into_values().collect();
    let mut rng = Rng(seed | 1);
    let mut picked = Vec::new();
    while picked.len() < count && groups.iter().any(|g| !g.is_empty()) {
        for g in groups.iter_mut().filter(|g| !g.is_empty()) {
            if picked.len() == count {
                break;
            }
            let i = rng.below(g.len() as u32) as usize;
            picked.push(g.swap_remove(i));
        }
    }

    let mut tests = Vec::new();
    for insn in &picked {
        for _ in 0..states {
            tests.push((*insn, random_state(&mut rng)));
        }
    }

    std::fs::create_dir_all(out).map_err(|e| format!("{}: {e}", out.display()))?;
    write_harness(out, &tests)?;
    write_native(out, &tests)?;
    let mut list = String::new();
    for (i, (insn, _)) in tests.iter().enumerate() {
        writeln!(list, "{i}\t{:08x}\t{:08x}\t{insn}", insn.addr, insn.raw).unwrap();
    }
    std::fs::write(out.join("tests.tsv"), list).map_err(|e| e.to_string())?;
    Ok(tests.len())
}

fn words(state: &[u32; STATE_WORDS]) -> String {
    state.iter().map(|w| format!("0x{w:08x}")).collect::<Vec<_>>().join(",")
}

fn write_harness(out: &Path, tests: &[(Insn, [u32; STATE_WORDS])]) -> Result<(), String> {
    let mut s = String::from(".syntax unified\n.arch armv6k\n.fpu vfpv2\n.arm\n.text\n.global _start\n_start:\n");
    let stride = STATE_WORDS * 4;
    for (i, (insn, _)) in tests.iter().enumerate() {
        let off = i * stride;
        writeln!(
            s,
            "@ test {i}: {insn}
    ldr sp, =inputs + {off}
    ldr r0, [sp, #{cpsr}]
    msr APSR_nzcvqg, r0
    ldr r0, [sp, #{fpscr}]
    vmsr fpscr, r0
    add r0, sp, #{s0}
    vldmia r0, {{s0-s31}}
    ldmia sp, {{r0-r12, lr}}
    .word 0x{raw:08x}
    ldr sp, =outputs + {off}
    stmia sp, {{r0-r12, lr}}
    mrs r0, apsr
    str r0, [sp, #{cpsr}]
    add r0, sp, #{s0}
    vstmia r0, {{s0-s31}}
    vmrs r0, fpscr
    str r0, [sp, #{fpscr}]
    b 1f
    .ltorg
1:",
            cpsr = CPSR * 4,
            fpscr = FPSCR * 4,
            s0 = S0 * 4,
            raw = insn.raw
        )
        .unwrap();
    }
    writeln!(
        s,
        "    ldr r1, =outputs
    ldr r2, ={size}
    mov r0, #1
    mov r7, #4
    svc 0
    mov r0, #0
    mov r7, #1
    svc 0
    .ltorg
.data
.balign 4
inputs:",
        size = tests.len() * stride
    )
    .unwrap();
    for (_, state) in tests {
        writeln!(s, "    .word {}", words(state)).unwrap();
    }
    writeln!(s, ".bss\n.balign 4\noutputs:\n    .space {}", tests.len() * stride).unwrap();
    std::fs::write(out.join("harness.S"), s).map_err(|e| e.to_string())
}

fn write_native(out: &Path, tests: &[(Insn, [u32; STATE_WORDS])]) -> Result<(), String> {
    let mut s = String::from(
        "#include \"recomp.h\"\n#include <stdio.h>\n#pragma STDC FP_CONTRACT OFF\n\nuint8_t *recomp_mem;\n\n",
    );
    for (i, (insn, _)) in tests.iter().enumerate() {
        writeln!(
            s,
            "static void t{i}(Ctx *RECOMP_RESTRICT ctx) {{ /* {insn} */\n{}}}",
            emit::single_instruction(insn)
        )
        .unwrap();
    }
    writeln!(s, "static void (*const tests[])(Ctx *RECOMP_RESTRICT) = {{").unwrap();
    for i in 0..tests.len() {
        write!(s, "t{i},").unwrap();
    }
    writeln!(s, "}};\nstatic const uint32_t inputs[][{STATE_WORDS}] = {{").unwrap();
    for (_, state) in tests {
        writeln!(s, "{{{}}},", words(state)).unwrap();
    }
    writeln!(
        s,
        "}};

int main(void) {{
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {{
        const uint32_t *in = inputs[i];
        uint32_t o[{STATE_WORDS}];
        Ctx c;
        memset(&c, 0, sizeof c);
        memcpy(c.r, in, 13 * 4);
        c.r[14] = in[13];
        c.n = in[{CPSR}] >> 31; c.z = (in[{CPSR}] >> 30) & 1; c.c = (in[{CPSR}] >> 29) & 1;
        c.v = (in[{CPSR}] >> 28) & 1; c.q = (in[{CPSR}] >> 27) & 1; c.ge = (in[{CPSR}] >> 16) & 0xF;
        memcpy(c.s, in + {S0}, 32 * 4);
        c.fpscr = in[{FPSCR}];
        tests[i](&c);
        memcpy(o, c.r, 13 * 4);
        o[13] = c.r[14];
        o[{CPSR}] = (c.n << 31) | (c.z << 30) | (c.c << 29) | (c.v << 28) | (c.q << 27) | (c.ge << 16);
        memcpy(o + {S0}, c.s, 32 * 4);
        o[{FPSCR}] = c.fpscr;
        fwrite(o, 4, {STATE_WORDS}, stdout);
    }}
    return 0;
}}"
    )
    .unwrap();
    std::fs::write(out.join("native.c"), s).map_err(|e| e.to_string())
}
