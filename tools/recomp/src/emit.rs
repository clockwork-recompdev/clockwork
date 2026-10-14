//! C code generation: one C function per discovered guest function.
//!
//! Guest registers and flags live in `Ctx` (runtime/include/recomp.h). Control flow inside a
//! function becomes labels and gotos; calls become C calls; returns become `return`.
//! A per-function flag liveness pass means flags are only computed when something reads them.

use crate::discover::{Analysis, Flow, Function, Resolved, flow};
use crate::insn::*;
use std::collections::{BTreeMap, BTreeSet, HashSet};
use std::fmt::Write;

const N: u8 = 1;
const Z: u8 = 2;
const C: u8 = 4;
const V: u8 = 8;
const NZCV: u8 = N | Z | C | V;

pub fn fn_name(addr: u32) -> String {
    format!("f_{addr:08x}")
}

fn label(addr: u32) -> String {
    format!("L_{addr:08x}")
}

fn cond_flags(c: Cond) -> u8 {
    use Cond::*;
    match c {
        Eq | Ne => Z,
        Cs | Cc => C,
        Mi | Pl => N,
        Vs | Vc => V,
        Hi | Ls => C | Z,
        Ge | Lt => N | V,
        Gt | Le => N | Z | V,
        Al => 0,
    }
}

fn cond_expr(c: Cond) -> &'static str {
    use Cond::*;
    match c {
        Eq => "ctx->z",
        Ne => "!ctx->z",
        Cs => "ctx->c",
        Cc => "!ctx->c",
        Mi => "ctx->n",
        Pl => "!ctx->n",
        Vs => "ctx->v",
        Vc => "!ctx->v",
        Hi => "ctx->c && !ctx->z",
        Ls => "!ctx->c || ctx->z",
        Ge => "ctx->n == ctx->v",
        Lt => "ctx->n != ctx->v",
        Gt => "!ctx->z && ctx->n == ctx->v",
        Le => "ctx->z || ctx->n != ctx->v",
        Al => "1",
    }
}

fn is_plain(s: &Shifted) -> bool {
    s.kind == ShiftKind::Lsl && s.amount == ShiftAmount::Imm(0)
}

/// Whether a logical flag-setting op updates C (from the shifter); if not, C is preserved.
fn shifter_writes_c(op2: &Operand2) -> bool {
    match op2 {
        Operand2::Imm { rotated, .. } => *rotated,
        Operand2::Reg(s) => !is_plain(s),
    }
}

/// (flags read, flags written) ignoring the condition. Calls are treated as writing all flags
/// (the callee defines them; EABI helpers like __aeabi_cfcmple return results in flags).
fn flag_effects(insn: &Insn) -> (u8, u8) {
    let rrx = |op2: &Operand2| matches!(op2, Operand2::Reg(s) if s.kind == ShiftKind::Rrx);
    match &insn.op {
        Op::DataProc { op, s, op2, .. } => {
            let mut reads = if rrx(op2) { C } else { 0 };
            if matches!(op, DpOp::Adc | DpOp::Sbc | DpOp::Rsc) {
                reads |= C;
            }
            if !*s {
                return (reads, 0);
            }
            let arithmetic = matches!(
                op,
                DpOp::Add | DpOp::Adc | DpOp::Sub | DpOp::Sbc | DpOp::Rsb | DpOp::Rsc | DpOp::Cmp | DpOp::Cmn
            );
            if arithmetic {
                (reads, NZCV)
            } else if shifter_writes_c(op2) {
                // a register shift by zero leaves C unchanged at runtime
                let dynamic = matches!(
                    op2,
                    Operand2::Reg(Shifted {
                        amount: ShiftAmount::Reg(_),
                        ..
                    })
                );
                (reads | if dynamic { C } else { 0 }, N | Z | C)
            } else {
                (reads, N | Z)
            }
        }
        Op::Mul { s: true, .. } | Op::MulLong { s: true, .. } => (0, N | Z),
        Op::Mrs { .. } => (NZCV, 0),
        Op::Msr { spsr: false, mask, .. } if mask & 8 != 0 => (0, NZCV),
        Op::Vsys {
            to_core: true, rt: PC, ..
        } => (0, NZCV),
        Op::Branch { link: true, .. } | Op::BranchReg { link: true, .. } => (0, NZCV),
        Op::Svc(_) => (0, NZCV),
        _ => (0, 0),
    }
}

/// Per-instruction set of flags live after it.
fn flag_liveness(f: &Function, succ: &[Vec<usize>], exits: &[bool]) -> Vec<u8> {
    let insns: Vec<&Insn> = f.insns.values().collect();
    let mut live_in = vec![0u8; insns.len()];
    let mut live_out = vec![0u8; insns.len()];
    let mut changed = true;
    while changed {
        changed = false;
        for i in (0..insns.len()).rev() {
            let mut out = if exits[i] { NZCV } else { 0 };
            for &s in &succ[i] {
                out |= live_in[s];
            }
            let (reads, writes) = flag_effects(insns[i]);
            let cond = cond_flags(insns[i].cond);
            // a conditional instruction may not execute, so its writes don't kill
            let killed = if insns[i].cond == Cond::Al { writes } else { 0 };
            let inn = (out & !killed) | reads | cond;
            if inn != live_in[i] || out != live_out[i] {
                live_in[i] = inn;
                live_out[i] = out;
                changed = true;
            }
        }
    }
    live_out
}

pub struct Emitter<'a> {
    analysis: &'a Analysis,
    noreturn: &'a HashSet<u32>,
    variants: &'a Variants,
}

/// Instructions that differ in other versions of the game built from the same code (Yo-kai Watch 2
/// Bony Spirits and Fleshy Souls pass a different version number): address -> the other words.
/// The generated code picks the translation matching the code it was loaded with.
pub type Variants = BTreeMap<u32, Vec<u32>>;

/// Where control goes when an instruction transfers it outside straight-line code.
enum Exit<'s> {
    Goto(u32),
    TailCall(u32),
    Indirect(&'s str),
    Return,
}

impl<'a> Emitter<'a> {
    pub fn new(analysis: &'a Analysis, noreturn: &'a HashSet<u32>, variants: &'a Variants) -> Self {
        Emitter { analysis, noreturn, variants }
    }

    /// `body` (for `insn`) behind a check of the loaded code, with the other versions' translations
    fn with_variants(&self, f: &Function, insn: &Insn, live: u8, body: String, words: &[u32]) -> String {
        let indent = |b: &str| b.lines().map(|l| format!("    {l}\n")).collect::<String>();
        let mut o = format!("if (rd32(0x{:08x}u) == 0x{:08x}u) {{\n{}", insn.addr, insn.raw, indent(&body));
        for (k, &w) in words.iter().enumerate() {
            let alt = crate::arm::decode(insn.addr, w).expect("variant instruction decodes");
            let alt_body = self.insn(f, &alt, live, None);
            if k + 1 == words.len() {
                writeln!(o, "}} else {{ /* {alt} */").unwrap();
            } else {
                writeln!(o, "}} else if (rd32(0x{:08x}u) == 0x{w:08x}u) {{ /* {alt} */", insn.addr).unwrap();
            }
            o.push_str(&indent(&alt_body));
        }
        o.push('}');
        o
    }

    fn is_function(&self, addr: u32) -> bool {
        self.analysis.functions.contains_key(&addr)
    }

    /// C statement(s) transferring control out of instruction `site`.
    fn exit(&self, f: &Function, site: u32, e: Exit) -> String {
        match e {
            // a computed jump with a hand-set return address is a call that resumes here
            Exit::Indirect(v) if f.manual_calls.contains_key(&site) => {
                let ret = f.manual_calls[&site];
                let resume = self.exit(f, site, Exit::Goto(ret));
                format!("recomp_call_indirect(ctx, {v}); {resume}")
            }
            Exit::Goto(t) if f.insns.contains_key(&t) => format!("goto {};", label(t)),
            Exit::Goto(t) | Exit::TailCall(t) if self.is_function(t) => format!("{}(ctx); return;", fn_name(t)),
            Exit::Goto(t) | Exit::TailCall(t) => format!("recomp_call_indirect(ctx, 0x{t:08x}u); return;"),
            Exit::Indirect(v) => format!("recomp_call_indirect(ctx, {v}); return;"),
            Exit::Return => "return;".to_string(),
        }
    }

    pub fn function(&self, f: &Function) -> String {
        let insns: Vec<&Insn> = f.insns.values().collect();
        let index: BTreeMap<u32, usize> = insns.iter().enumerate().map(|(i, x)| (x.addr, i)).collect();
        let tables: BTreeMap<u32, &Vec<u32>> = f.jump_tables.iter().map(|(a, t)| (*a, t)).collect();

        // successors and exits for liveness; labels for gotos
        let mut succ = vec![Vec::new(); insns.len()];
        let mut exits = vec![false; insns.len()];
        let mut labels = BTreeSet::new();
        for (i, insn) in insns.iter().enumerate() {
            let next = insn.addr + insn.size as u32;
            let to = |t: u32, succ: &mut Vec<usize>, exits: &mut bool| match index.get(&t) {
                Some(&j) => succ.push(j),
                None => *exits = true,
            };
            match flow(insn) {
                Flow::Next | Flow::Call { .. } | Flow::IndirectCall => to(next, &mut succ[i], &mut exits[i]),
                Flow::Jump { target, also_next } => {
                    let target = f.manual_calls.get(&insn.addr).copied().unwrap_or(target);
                    to(target, &mut succ[i], &mut exits[i]);
                    labels.insert(target);
                    if also_next {
                        to(next, &mut succ[i], &mut exits[i]);
                    }
                }
                Flow::Return { also_next } | Flow::IndirectJump { also_next } => {
                    exits[i] = true;
                    if let Some(&ret) = f.manual_calls.get(&insn.addr) {
                        to(ret, &mut succ[i], &mut exits[i]);
                        labels.insert(ret);
                    }
                    if let Some(t) = tables.get(&insn.addr) {
                        for &target in t.iter() {
                            to(target, &mut succ[i], &mut exits[i]);
                            labels.insert(target);
                        }
                    }
                    if also_next {
                        to(next, &mut succ[i], &mut exits[i]);
                    }
                }
                Flow::Stop => {}
            }
        }
        for (i, insn) in insns.iter().enumerate() {
            let next = insn.addr + insn.size as u32;
            if self.falls_through(insn) && insns.get(i + 1).map(|x| x.addr) != Some(next) {
                labels.insert(next);
            }
        }
        if insns.first().is_some_and(|i| i.addr != f.entry) {
            labels.insert(f.entry);
        }
        let live = flag_liveness(f, &succ, &exits);

        let mut out = String::new();
        writeln!(out, "void {}(Ctx *RECOMP_RESTRICT ctx) {{", fn_name(f.entry)).unwrap();
        writeln!(out, "    RECOMP_ENTER(ctx, 0x{:08x}u);", f.entry).unwrap();
        // code absorbed from below the entry (e.g. a tail-called veneer) is emitted first
        // in address order, so start at the entry explicitly
        if insns.first().is_some_and(|i| i.addr != f.entry) {
            writeln!(out, "    goto {};", label(f.entry)).unwrap();
        }
        for (i, insn) in insns.iter().enumerate() {
            if labels.contains(&insn.addr) {
                writeln!(out, "{}:;", label(insn.addr)).unwrap();
            }
            writeln!(out, "    /* {:08x}: {} */", insn.addr, insn).unwrap();
            let mut body = self.insn(f, insn, live[i], tables.get(&insn.addr).copied());
            if let Some(words) = self.variants.get(&insn.addr) {
                body = self.with_variants(f, insn, live[i], body, words);
            }
            if insn.cond == Cond::Al {
                // a block keeps each instruction's temporaries separate
                let single = body.lines().count() == 1 && !body.starts_with("uint");
                if single {
                    write!(out, "    {body}").unwrap();
                } else if !body.is_empty() {
                    writeln!(out, "    {{").unwrap();
                    for line in body.lines() {
                        writeln!(out, "        {line}").unwrap();
                    }
                    writeln!(out, "    }}").unwrap();
                }
            } else {
                writeln!(out, "    if ({}) {{", cond_expr(insn.cond)).unwrap();
                for line in body.lines() {
                    writeln!(out, "        {line}").unwrap();
                }
                writeln!(out, "    }}").unwrap();
            }

            // fallthrough to something other than the next emitted instruction
            let next = insn.addr + insn.size as u32;
            let next_emitted = insns.get(i + 1).map(|x| x.addr);
            if self.falls_through(insn) && next_emitted != Some(next) {
                writeln!(out, "    {}", self.exit(f, insn.addr, Exit::Goto(next))).unwrap();
            }
        }
        writeln!(out, "}}").unwrap();
        out
    }

    fn falls_through(&self, insn: &Insn) -> bool {
        match flow(insn) {
            Flow::Next | Flow::IndirectCall => true,
            Flow::Call { target, .. } => {
                insn.cond != Cond::Al
                    || !(self.noreturn.contains(&target) || self.analysis.pops_caller.contains(&target))
            }
            Flow::Jump { also_next, .. } | Flow::Return { also_next } | Flow::IndirectJump { also_next } => also_next,
            Flow::Stop => false,
        }
    }

    /// The C body for one instruction (without its condition check).
    fn insn(&self, f: &Function, insn: &Insn, live: u8, table: Option<&Vec<u32>>) -> String {
        let mut o = String::new();
        // jump table (`addls pc, pc, rX, lsl #2` or `ldrls pc, [pc, rX, lsl #2]`): a switch
        if let Some(targets) = table {
            let index = match insn.op {
                Op::DataProc {
                    op2: Operand2::Reg(s), ..
                } => s.rm,
                Op::Mem {
                    addr:
                        MemAddr::Base {
                            offset: MemOffset::Reg(s),
                            ..
                        },
                    ..
                } => s.rm,
                _ => unreachable!("jump tables are only recognized on these forms"),
            };
            writeln!(o, "switch (ctx->r[{index}]) {{").unwrap();
            for (i, t) in targets.iter().enumerate() {
                writeln!(o, "case {i}: {}", self.exit(f, insn.addr, Exit::Goto(*t))).unwrap();
            }
            writeln!(
                o,
                "default: recomp_unhandled(ctx, 0x{:08x}u, \"jump table index\");",
                insn.addr
            )
            .unwrap();
            writeln!(o, "}}").unwrap();
            return o;
        }
        let rd = |r: Reg| -> String {
            if r == PC {
                format!("0x{:08x}u", insn.pc_value())
            } else {
                format!("ctx->r[{r}]")
            }
        };
        match &insn.op {
            Op::DataProc { op, s, rd: d, rn, op2 } => self.data_proc(f, insn, *op, *s, *d, *rn, op2, live, &mut o),
            Op::Mul { s, rd: d, rm, rs, acc } => {
                let acc = acc.map(|a| format!(" + {}", rd(a))).unwrap_or_default();
                writeln!(o, "uint32_t res = {} * {}{acc};", rd(*rm), rd(*rs)).unwrap();
                if *s {
                    flags_nz(&mut o, "res", live);
                }
                writeln!(o, "ctx->r[{d}] = res;").unwrap();
            }
            Op::MulLong { kind, s, rdlo, rdhi, rm, rs } => {
                let prod = match kind {
                    MulLongKind::Umull | MulLongKind::Umlal => format!("(uint64_t){} * {}", rd(*rm), rd(*rs)),
                    _ => format!("(uint64_t)((int64_t)(int32_t){} * (int32_t){})", rd(*rm), rd(*rs)),
                };
                let acc = match kind {
                    MulLongKind::Umlal | MulLongKind::Smlal => {
                        format!(" + (((uint64_t){} << 32) | {})", rd(*rdhi), rd(*rdlo))
                    }
                    _ => String::new(),
                };
                writeln!(o, "uint64_t res = {prod}{acc};").unwrap();
                if *s {
                    if live & N != 0 {
                        writeln!(o, "ctx->n = (uint32_t)(res >> 63);").unwrap();
                    }
                    if live & Z != 0 {
                        writeln!(o, "ctx->z = res == 0;").unwrap();
                    }
                }
                writeln!(o, "ctx->r[{rdlo}] = (uint32_t)res; ctx->r[{rdhi}] = (uint32_t)(res >> 32);").unwrap();
            }
            Op::Umaal { rdlo, rdhi, rm, rs } => {
                writeln!(
                    o,
                    "uint64_t res = (uint64_t){} * {} + {} + {};",
                    rd(*rm),
                    rd(*rs),
                    rd(*rdlo),
                    rd(*rdhi)
                )
                .unwrap();
                writeln!(o, "ctx->r[{rdlo}] = (uint32_t)res; ctx->r[{rdhi}] = (uint32_t)(res >> 32);").unwrap();
            }
            Op::SmulHalf { rd: d, rm, rs, m_top, s_top, acc } => {
                writeln!(o, "int32_t p = {} * {};", half(&rd(*rm), *m_top), half(&rd(*rs), *s_top)).unwrap();
                match acc {
                    None => writeln!(o, "ctx->r[{d}] = (uint32_t)p;").unwrap(),
                    Some(a) => q_add(&mut o, *d, "p", &format!("(int32_t){}", rd(*a))),
                }
            }
            Op::SmulWord { rd: d, rm, rs, s_top, acc } => {
                writeln!(
                    o,
                    "int32_t p = (int32_t)(((int64_t)(int32_t){} * {}) >> 16);",
                    rd(*rm),
                    half(&rd(*rs), *s_top)
                )
                .unwrap();
                match acc {
                    None => writeln!(o, "ctx->r[{d}] = (uint32_t)p;").unwrap(),
                    Some(a) => q_add(&mut o, *d, "p", &format!("(int32_t){}", rd(*a))),
                }
            }
            Op::SmlalHalf { rdlo, rdhi, rm, rs, m_top, s_top } => {
                writeln!(
                    o,
                    "uint64_t res = (((uint64_t){} << 32) | {}) + (uint64_t)(int64_t)({} * {});",
                    rd(*rdhi),
                    rd(*rdlo),
                    half(&rd(*rm), *m_top),
                    half(&rd(*rs), *s_top)
                )
                .unwrap();
                writeln!(o, "ctx->r[{rdlo}] = (uint32_t)res; ctx->r[{rdhi}] = (uint32_t)(res >> 32);").unwrap();
            }
            Op::DualMul { kind, swap, rd: d, rn, rm, acc } => {
                dual_products(&mut o, &rd(*rn), &rd(*rm), *swap);
                let sub = matches!(kind, DualMulKind::Smlsd | DualMulKind::Smlsld);
                let sum = if sub { "(int64_t)p1 - p2" } else { "(int64_t)p1 + p2" };
                let acc = acc.map(|a| format!(" + (int32_t){}", rd(a))).unwrap_or_default();
                writeln!(o, "int64_t sum = {sum}{acc};").unwrap();
                writeln!(o, "if (sum != (int32_t)sum) ctx->q = 1;").unwrap();
                writeln!(o, "ctx->r[{d}] = (uint32_t)sum;").unwrap();
            }
            Op::DualMulLong { kind, swap, rdlo, rdhi, rn, rm } => {
                dual_products(&mut o, &rd(*rn), &rd(*rm), *swap);
                let sub = matches!(kind, DualMulKind::Smlsd | DualMulKind::Smlsld);
                writeln!(
                    o,
                    "uint64_t res = (((uint64_t){} << 32) | {}) + (uint64_t)((int64_t)p1 {} p2);",
                    rd(*rdhi),
                    rd(*rdlo),
                    if sub { "-" } else { "+" }
                )
                .unwrap();
                writeln!(o, "ctx->r[{rdlo}] = (uint32_t)res; ctx->r[{rdhi}] = (uint32_t)(res >> 32);").unwrap();
            }
            Op::MulHigh { sub, round, rd: d, rn, rm, acc } => {
                writeln!(o, "int64_t p = (int64_t)(int32_t){} * (int32_t){};", rd(*rn), rd(*rm)).unwrap();
                let base = match (sub, acc) {
                    (false, None) => "p".to_string(),
                    (false, Some(a)) => format!("(int64_t)((uint64_t){} << 32) + p", rd(*a)),
                    (true, Some(a)) => format!("(int64_t)((uint64_t){} << 32) - p", rd(*a)),
                    (true, None) => "-p".to_string(),
                };
                let r = if *round { " + 0x80000000" } else { "" };
                writeln!(o, "ctx->r[{d}] = (uint32_t)((uint64_t)({base}{r}) >> 32);").unwrap();
            }
            Op::SatArith { op, rd: d, rm, rn } => {
                let (m, n) = (format!("(int64_t)(int32_t){}", rd(*rm)), format!("(int64_t)(int32_t){}", rd(*rn)));
                let expr = match op {
                    SatArith::Qadd => format!("{m} + {n}"),
                    SatArith::Qsub => format!("{m} - {n}"),
                    SatArith::Qdadd => format!("{m} + (int32_t)sat_s32(2 * {n}, &ctx->q)"),
                    SatArith::Qdsub => format!("{m} - (int32_t)sat_s32(2 * {n}, &ctx->q)"),
                };
                writeln!(o, "ctx->r[{d}] = sat_s32({expr}, &ctx->q);").unwrap();
            }
            Op::Clz { rd: d, rm } => writeln!(o, "ctx->r[{d}] = clz32({});", rd(*rm)).unwrap(),
            Op::Sat { signed, rd: d, bits, rn } => {
                let v = shifted_value(insn, rn);
                let f = if *signed { "ssat" } else { "usat" };
                writeln!(o, "ctx->r[{d}] = {f}((int32_t)({v}), {bits}, &ctx->q);").unwrap();
            }
            Op::Sat16 { signed, rd: d, bits, rn } => {
                let f = if *signed { "ssat" } else { "usat" };
                let v = rd(*rn);
                writeln!(
                    o,
                    "ctx->r[{d}] = ({f}((int16_t){v}, {bits}, &ctx->q) & 0xFFFFu) | ({f}((int16_t)({v} >> 16), {bits}, &ctx->q) << 16);"
                )
                .unwrap();
            }
            Op::Extend { signed, size, rd: d, rn, rm, rot } => {
                let x = if *rot == 0 { rd(*rm) } else { format!("ror_r({}, {rot})", rd(*rm)) };
                writeln!(o, "uint32_t x = {x};").unwrap();
                let ext = match (size, signed) {
                    (ExtendSize::Byte, true) => "(uint32_t)(int32_t)(int8_t)x",
                    (ExtendSize::Byte, false) => "(x & 0xFFu)",
                    (ExtendSize::Half, true) => "(uint32_t)(int32_t)(int16_t)x",
                    (ExtendSize::Half, false) => "(x & 0xFFFFu)",
                    (ExtendSize::Byte16, true) => {
                        "(((uint32_t)(uint16_t)(int16_t)(int8_t)x) | ((uint32_t)(uint16_t)(int16_t)(int8_t)(x >> 16) << 16))"
                    }
                    (ExtendSize::Byte16, false) => "(x & 0x00FF00FFu)",
                };
                match (rn, size) {
                    (None, _) => writeln!(o, "ctx->r[{d}] = {ext};").unwrap(),
                    (Some(n), ExtendSize::Byte16) => {
                        let n = rd(*n);
                        writeln!(o, "uint32_t e = {ext};").unwrap();
                        writeln!(
                            o,
                            "ctx->r[{d}] = ((({n} & 0xFFFFu) + (e & 0xFFFFu)) & 0xFFFFu) | ((({n} >> 16) + (e >> 16)) << 16);"
                        )
                        .unwrap();
                    }
                    (Some(n), _) => writeln!(o, "ctx->r[{d}] = {} + {ext};", rd(*n)).unwrap(),
                }
            }
            Op::Rev { kind, rd: d, rm } => {
                let v = rd(*rm);
                let e = match kind {
                    RevKind::Rev => format!("rev32({v})"),
                    RevKind::Rev16 => format!("((({v} >> 8) & 0x00FF00FFu) | (({v} << 8) & 0xFF00FF00u))"),
                    RevKind::Revsh => format!("(uint32_t)(int32_t)(int16_t)((({v} & 0xFFu) << 8) | (({v} >> 8) & 0xFFu))"),
                };
                writeln!(o, "ctx->r[{d}] = {e};").unwrap();
            }
            Op::Pkh { tb, rd: d, rn, rm, shift } => {
                let (n, m) = (rd(*rn), rd(*rm));
                if *tb {
                    let sh = (*shift).min(31);
                    writeln!(o, "ctx->r[{d}] = ({n} & 0xFFFF0000u) | ((uint32_t)((int32_t){m} >> {sh}) & 0xFFFFu);").unwrap();
                } else {
                    writeln!(o, "ctx->r[{d}] = ({n} & 0xFFFFu) | (({m} << {shift}) & 0xFFFF0000u);").unwrap();
                }
            }
            Op::Sel { rd: d, rn, rm } => writeln!(o, "ctx->r[{d}] = sel(ctx->ge, {}, {});", rd(*rn), rd(*rm)).unwrap(),
            Op::Parallel { prefix, op, rd: d, rn, rm } => writeln!(
                o,
                "ctx->r[{d}] = recomp_parallel(ctx, {}, {}, {}, {});",
                *prefix as u8,
                *op as u8,
                rd(*rn),
                rd(*rm)
            )
            .unwrap(),
            Op::Usad8 { rd: d, rn, rm, acc } => {
                let acc = acc.map(|a| format!(" + {}", rd(a))).unwrap_or_default();
                writeln!(o, "ctx->r[{d}] = recomp_usad8({}, {}){acc};", rd(*rn), rd(*rm)).unwrap();
            }
            Op::Mem { load, size, rt, addr, .. } => self.mem(f, insn, *load, *size, *rt, addr, &mut o),
            Op::Multi { load, rn, regs, mode, writeback, psr } => {
                if *psr {
                    writeln!(o, "recomp_unhandled(ctx, 0x{:08x}u, \"ldm/stm ^\");", insn.addr).unwrap();
                } else {
                    self.multi(f, insn, *load, *rn, *regs, *mode, *writeback, &mut o);
                }
            }
            Op::Swp { byte, rt, rt2, rn } => {
                let (r, w) = if *byte { ("rd8", "wr8") } else { ("rd32", "wr32") };
                writeln!(o, "uint32_t a = {}; uint32_t old = {r}(a);", rd(*rn)).unwrap();
                writeln!(o, "{w}(a, {}); ctx->r[{rt}] = old;", rd(*rt2)).unwrap();
            }
            // guest threads only switch at SVCs, so exclusive pairs can't be interrupted
            Op::Ldrex { size, rt, rn } => {
                let a = rd(*rn);
                match size {
                    MemSize::Dual => writeln!(o, "ctx->r[{rt}] = rd32({a}); ctx->r[{}] = rd32({a} + 4);", rt + 1).unwrap(),
                    _ => writeln!(o, "ctx->r[{rt}] = {}({a});", load_fn(*size)).unwrap(),
                }
            }
            Op::Strex { size, rd: d, rt, rn } => {
                let a = rd(*rn);
                match size {
                    MemSize::Dual => writeln!(o, "wr32({a}, {}); wr32({a} + 4, {});", rd(*rt), rd(rt + 1)).unwrap(),
                    _ => writeln!(o, "{}({a}, {});", store_fn(*size), rd(*rt)).unwrap(),
                }
                writeln!(o, "ctx->r[{d}] = 0;").unwrap();
            }
            Op::Clrex | Op::Pld | Op::Hint(_) => {}
            Op::Branch { target, link: true, exchange } => {
                let ret = insn.addr + insn.size as u32;
                let ret = if insn.thumb { ret | 1 } else { ret };
                writeln!(o, "ctx->r[14] = 0x{ret:08x}u;").unwrap();
                let callee_thumb = insn.thumb ^ exchange;
                if self.is_function(*target) {
                    writeln!(o, "{}(ctx);", fn_name(*target)).unwrap();
                } else {
                    let t = target | callee_thumb as u32;
                    writeln!(o, "recomp_call_indirect(ctx, 0x{t:08x}u);").unwrap();
                }
                // the helper already returned on the caller's behalf
                if self.analysis.pops_caller.contains(target) {
                    writeln!(o, "{}", self.exit(f, insn.addr, Exit::Return)).unwrap();
                }
            }
            Op::Branch { target, link: false, .. } => match f.manual_calls.get(&insn.addr) {
                // a branch with a hand-set return address: call, then resume
                Some(&ret) => {
                    if self.is_function(*target) {
                        writeln!(o, "{}(ctx);", fn_name(*target)).unwrap();
                    } else {
                        writeln!(o, "recomp_call_indirect(ctx, 0x{target:08x}u);").unwrap();
                    }
                    writeln!(o, "{}", self.exit(f, insn.addr, Exit::Goto(ret))).unwrap();
                }
                None => writeln!(o, "{}", self.exit(f, insn.addr, Exit::Goto(*target))).unwrap(),
            },
            Op::BranchReg { rm, link } => {
                let resolved = f.resolved.get(&insn.addr);
                if *link {
                    let ret = insn.addr + insn.size as u32;
                    let ret = if insn.thumb { ret | 1 } else { ret };
                    match resolved {
                        Some(Resolved::Target(t)) if self.is_function(t & !1) => {
                            writeln!(o, "ctx->r[14] = 0x{ret:08x}u;").unwrap();
                            writeln!(o, "{}(ctx);", fn_name(t & !1)).unwrap()
                        }
                        _ => {
                            // read the target before LR is overwritten (blx lr)
                            writeln!(o, "uint32_t t = {};", rd(*rm)).unwrap();
                            writeln!(o, "ctx->r[14] = 0x{ret:08x}u;").unwrap();
                            writeln!(o, "recomp_call_indirect(ctx, t);").unwrap()
                        }
                    }
                } else {
                    let e = match (rm, resolved) {
                        (&LR, _) | (_, Some(Resolved::Return)) => Exit::Return,
                        (_, Some(Resolved::Target(t))) => Exit::TailCall(t & !1),
                        _ => Exit::Indirect(""),
                    };
                    match e {
                        Exit::Indirect(_) => {
                            let v = rd(*rm);
                            writeln!(o, "{}", self.exit(f, insn.addr, Exit::Indirect(&v))).unwrap()
                        }
                        e => writeln!(o, "{}", self.exit(f, insn.addr, e)).unwrap(),
                    }
                }
            }
            Op::Svc(n) => {
                writeln!(o, "recomp_svc(ctx, 0x{n:x}u, 0x{:08x}u);", insn.addr).unwrap();
                if matches!(flow(insn), Flow::Stop) {
                    writeln!(o, "return;").unwrap();
                }
            }
            Op::Bkpt(_) | Op::Udf => {
                writeln!(o, "recomp_unhandled(ctx, 0x{:08x}u, \"{}\");", insn.addr, insn).unwrap();
            }
            Op::Mrs { spsr: false, rd: d } => writeln!(
                o,
                "ctx->r[{d}] = (ctx->n << 31) | (ctx->z << 30) | (ctx->c << 29) | (ctx->v << 28) | (ctx->q << 27) | (ctx->ge << 16) | 0x10u;"
            )
            .unwrap(),
            Op::Msr { spsr: false, mask, src } => {
                let v = match src {
                    Operand2::Imm { value, .. } => format!("0x{value:08x}u"),
                    Operand2::Reg(s) => rd(s.rm),
                };
                writeln!(o, "uint32_t psr = {v};").unwrap();
                if mask & 8 != 0 {
                    writeln!(
                        o,
                        "ctx->n = psr >> 31; ctx->z = (psr >> 30) & 1; ctx->c = (psr >> 29) & 1; ctx->v = (psr >> 28) & 1; ctx->q = (psr >> 27) & 1;"
                    )
                    .unwrap();
                }
                if mask & 4 != 0 {
                    writeln!(o, "ctx->ge = (psr >> 16) & 0xF;").unwrap();
                }
            }
            Op::Mrs { .. } | Op::Msr { .. } | Op::Cps | Op::Setend { .. } => {
                writeln!(o, "recomp_unhandled(ctx, 0x{:08x}u, \"{}\");", insn.addr, insn).unwrap();
            }
            Op::CoprocReg { load: true, coproc: 15, opc1: 0, rt, crn: 13, crm: 0, opc2: 3 } => {
                writeln!(o, "ctx->r[{rt}] = ctx->tls;").unwrap();
            }
            // ARMv6 barriers through CP15 (DMB, DSB/drain write buffer, prefetch flush): one guest
            // thread runs at a time, so there's nothing to order
            Op::CoprocReg {
                load: false,
                coproc: 15,
                opc1: 0,
                crn: 7,
                crm: 10,
                opc2: 4 | 5,
                ..
            }
            | Op::CoprocReg {
                load: false,
                coproc: 15,
                opc1: 0,
                crn: 7,
                crm: 5,
                opc2: 4,
                ..
            } => {}
            Op::CoprocReg { .. } => {
                writeln!(o, "recomp_coproc(ctx, 0x{:08x}u, 0x{:08x}u);", insn.addr, insn.raw).unwrap();
            }
            Op::VfpBinary { op, d, n, m } => {
                let (dv, nv, mv) = (fget(*d), fget(*n), fget(*m));
                let e = match op {
                    VfpBinary::Mla => format!("{dv} + {nv} * {mv}"),
                    VfpBinary::Mls => format!("{dv} - {nv} * {mv}"),
                    VfpBinary::Nmls => format!("{nv} * {mv} - {dv}"),
                    VfpBinary::Nmla => format!("-{dv} - {nv} * {mv}"),
                    VfpBinary::Mul => format!("{nv} * {mv}"),
                    VfpBinary::Nmul => format!("-({nv} * {mv})"),
                    VfpBinary::Add => format!("{nv} + {mv}"),
                    VfpBinary::Sub => format!("{nv} - {mv}"),
                    VfpBinary::Div => format!("{nv} / {mv}"),
                };
                // the product is rounded on its own (not fused), which FP_CONTRACT OFF guarantees
                writeln!(o, "{}", fset(*d, &e)).unwrap();
            }
            Op::VfpUnary { op, d, m } => match (op, d, m) {
                (VfpUnary::Mov, FReg::S(d), FReg::S(m)) => writeln!(o, "ctx->s[{d}] = ctx->s[{m}];").unwrap(),
                (VfpUnary::Mov, FReg::D(d), FReg::D(m)) => {
                    writeln!(o, "ctx->s[{}] = ctx->s[{}]; ctx->s[{}] = ctx->s[{}];", 2 * d, 2 * m, 2 * d + 1, 2 * m + 1)
                        .unwrap()
                }
                // sign-bit ops work on the bits, so NaN payloads pass through like on hardware
                (VfpUnary::Abs, FReg::S(d), FReg::S(m)) => {
                    writeln!(o, "ctx->s[{d}] = ctx->s[{m}] & 0x7FFFFFFFu;").unwrap()
                }
                (VfpUnary::Neg, FReg::S(d), FReg::S(m)) => {
                    writeln!(o, "ctx->s[{d}] = ctx->s[{m}] ^ 0x80000000u;").unwrap()
                }
                (VfpUnary::Abs, FReg::D(d), FReg::D(m)) => writeln!(
                    o,
                    "ctx->s[{}] = ctx->s[{}]; ctx->s[{}] = ctx->s[{}] & 0x7FFFFFFFu;",
                    2 * d,
                    2 * m,
                    2 * d + 1,
                    2 * m + 1
                )
                .unwrap(),
                (VfpUnary::Neg, FReg::D(d), FReg::D(m)) => writeln!(
                    o,
                    "ctx->s[{}] = ctx->s[{}]; ctx->s[{}] = ctx->s[{}] ^ 0x80000000u;",
                    2 * d,
                    2 * m,
                    2 * d + 1,
                    2 * m + 1
                )
                .unwrap(),
                (VfpUnary::Sqrt, FReg::S(_), _) => writeln!(o, "{}", fset(*d, &format!("sqrtf({})", fget(*m)))).unwrap(),
                _ => writeln!(o, "{}", fset(*d, &format!("sqrt({})", fget(*m)))).unwrap(),
            },
            Op::VfpCmp { d, m, .. } => {
                let rhs = m.map(fget).unwrap_or_else(|| "0.0".into());
                writeln!(o, "vfp_cmp(ctx, (double){}, (double){rhs});", fget(*d)).unwrap();
            }
            Op::VfpCvt { from, to, d, m, round_zero } => {
                let src = match (from, m) {
                    (NumType::S32, FReg::S(n)) => format!("(int32_t)ctx->s[{n}]"),
                    (NumType::U32, FReg::S(n)) => format!("ctx->s[{n}]"),
                    _ => fget(*m),
                };
                match (to, d) {
                    (NumType::S32, FReg::S(n)) => {
                        writeln!(o, "ctx->s[{n}] = vfp_to_s32((double){src}, {});", *round_zero as u8).unwrap()
                    }
                    (NumType::U32, FReg::S(n)) => {
                        writeln!(o, "ctx->s[{n}] = vfp_to_u32((double){src}, {});", *round_zero as u8).unwrap()
                    }
                    (NumType::F32, _) => writeln!(o, "{}", fset(*d, &format!("(float){src}"))).unwrap(),
                    _ => writeln!(o, "{}", fset(*d, &format!("(double){src}"))).unwrap(),
                }
            }
            Op::VmovSingle { to_core, rt, sn } => {
                if *to_core {
                    writeln!(o, "ctx->r[{rt}] = ctx->s[{sn}];").unwrap()
                } else {
                    writeln!(o, "ctx->s[{sn}] = {};", rd(*rt)).unwrap()
                }
            }
            Op::VmovPair { to_core, rt, rt2, m } => {
                let lo = match m {
                    FReg::S(n) => *n as u32,
                    FReg::D(n) => 2 * *n as u32,
                };
                if *to_core {
                    writeln!(o, "ctx->r[{rt}] = ctx->s[{lo}]; ctx->r[{rt2}] = ctx->s[{}];", lo + 1).unwrap()
                } else {
                    writeln!(o, "ctx->s[{lo}] = {}; ctx->s[{}] = {};", rd(*rt), lo + 1, rd(*rt2)).unwrap()
                }
            }
            Op::VmovHalf { to_core, rt, dn, high } => {
                let s = 2 * *dn as u32 + *high as u32;
                if *to_core {
                    writeln!(o, "ctx->r[{rt}] = ctx->s[{s}];").unwrap()
                } else {
                    writeln!(o, "ctx->s[{s}] = {};", rd(*rt)).unwrap()
                }
            }
            Op::Vsys { to_core: true, rt: PC, .. } => writeln!(
                o,
                "ctx->n = ctx->fpscr >> 31; ctx->z = (ctx->fpscr >> 30) & 1; ctx->c = (ctx->fpscr >> 29) & 1; ctx->v = (ctx->fpscr >> 28) & 1;"
            )
            .unwrap(),
            Op::Vsys { to_core: true, rt, reg } => {
                let v = match reg {
                    1 => "ctx->fpscr",
                    0 => "0x410120B4u", // FPSID of the ARM11 MPCore VFP
                    _ => "0x40000000u", // FPEXC: enabled
                };
                writeln!(o, "ctx->r[{rt}] = {v};").unwrap();
            }
            Op::Vsys { to_core: false, rt, reg } => {
                if *reg == 1 {
                    writeln!(o, "ctx->fpscr = {};", rd(*rt)).unwrap();
                }
            }
            Op::VMem { load, d, addr } => {
                let ea = match addr {
                    MemAddr::Abs(a) => format!("0x{a:08x}u"),
                    MemAddr::Base { rn, offset: MemOffset::Imm(imm), add, .. } => {
                        format!("{} {} {imm}u", rd(*rn), if *add { "+" } else { "-" })
                    }
                    MemAddr::Base { .. } => unreachable!("VFP addressing is immediate-only"),
                };
                writeln!(o, "uint32_t a = {ea};").unwrap();
                let words = match d {
                    FReg::S(n) => vec![*n as u32],
                    FReg::D(n) => vec![2 * *n as u32, 2 * *n as u32 + 1],
                };
                for (i, s) in words.iter().enumerate() {
                    if *load {
                        writeln!(o, "ctx->s[{s}] = rd32(a + {});", i * 4).unwrap();
                    } else {
                        writeln!(o, "wr32(a + {}, ctx->s[{s}]);", i * 4).unwrap();
                    }
                }
            }
            Op::VMulti { load, first, count, rn, mode, writeback } => {
                let (start, words) = match first {
                    FReg::S(n) => (*n as u32, *count as u32),
                    FReg::D(n) => (2 * *n as u32, 2 * *count as u32),
                };
                let base = rd(*rn);
                let a = match mode {
                    BlockMode::Db => format!("{base} - {}u", words * 4),
                    _ => base.clone(),
                };
                writeln!(o, "uint32_t a = {a};").unwrap();
                for i in 0..words {
                    if *load {
                        writeln!(o, "ctx->s[{}] = rd32(a + {});", start + i, i * 4).unwrap();
                    } else {
                        writeln!(o, "wr32(a + {}, ctx->s[{}]);", i * 4, start + i).unwrap();
                    }
                }
                if *writeback {
                    let op = if *mode == BlockMode::Db { "-" } else { "+" };
                    writeln!(o, "ctx->r[{rn}] = {base} {op} {}u;", words * 4).unwrap();
                }
            }
        }
        o
    }

    #[allow(clippy::too_many_arguments)]
    fn data_proc(
        &self,
        f: &Function,
        insn: &Insn,
        op: DpOp,
        s: bool,
        d: Reg,
        rn: Reg,
        op2: &Operand2,
        live: u8,
        o: &mut String,
    ) {
        // a compare whose flags nothing reads does nothing
        if op.is_test() && live & NZCV == 0 {
            return;
        }
        let is_return = d == PC && op == DpOp::Mov && matches!(op2, Operand2::Reg(sh) if sh.rm == LR && is_plain(sh));
        if is_return {
            writeln!(o, "{}", self.exit(f, insn.addr, Exit::Return)).unwrap();
            return;
        }
        let rd = |r: Reg| -> String {
            if r == PC {
                format!("0x{:08x}u", insn.pc_value())
            } else {
                format!("ctx->r[{r}]")
            }
        };
        let logical = matches!(
            op,
            DpOp::And | DpOp::Eor | DpOp::Orr | DpOp::Bic | DpOp::Mov | DpOp::Mvn | DpOp::Tst | DpOp::Teq
        );
        let need_c = s && logical && live & C != 0;
        let (b, carry) = operand2(insn, op2, need_c, o);
        let a = rd(rn);
        let arith = |o: &mut String, x: &str, y: &str, cin: &str| {
            if s && live & (C | V) != 0 {
                writeln!(o, "uint32_t c, v; uint32_t res = add_cv({x}, {y}, {cin}, &c, &v);").unwrap();
            } else {
                writeln!(o, "uint32_t res = {x} + {y} + {cin};").unwrap();
            }
        };
        match op {
            DpOp::And | DpOp::Tst => writeln!(o, "uint32_t res = {a} & {b};").unwrap(),
            DpOp::Eor | DpOp::Teq => writeln!(o, "uint32_t res = {a} ^ {b};").unwrap(),
            DpOp::Orr => writeln!(o, "uint32_t res = {a} | {b};").unwrap(),
            DpOp::Bic => writeln!(o, "uint32_t res = {a} & ~{b};").unwrap(),
            DpOp::Mov => writeln!(o, "uint32_t res = {b};").unwrap(),
            DpOp::Mvn => writeln!(o, "uint32_t res = ~{b};").unwrap(),
            DpOp::Add | DpOp::Cmn => arith(o, &a, &b, "0"),
            DpOp::Adc => arith(o, &a, &b, "ctx->c"),
            DpOp::Sub | DpOp::Cmp => arith(o, &a, &format!("~{b}"), "1"),
            DpOp::Sbc => arith(o, &a, &format!("~{b}"), "ctx->c"),
            DpOp::Rsb => arith(o, &b, &format!("~{a}"), "1"),
            DpOp::Rsc => arith(o, &b, &format!("~{a}"), "ctx->c"),
        }
        if s && d != PC {
            flags_nz(o, "res", live);
            if logical {
                if let Some(c) = carry.filter(|_| live & C != 0) {
                    writeln!(o, "ctx->c = {c};").unwrap();
                }
            } else {
                if live & C != 0 {
                    writeln!(o, "ctx->c = c;").unwrap();
                }
                if live & V != 0 {
                    writeln!(o, "ctx->v = v;").unwrap();
                }
            }
        }
        if op.is_test() {
            if !s || live & (N | Z) == 0 {
                writeln!(o, "(void)res;").unwrap();
            }
            return;
        }
        if d != PC {
            writeln!(o, "ctx->r[{d}] = res;").unwrap();
            return;
        }
        // other writes to PC are computed jumps, unless discovery worked out the value
        let e = match f.resolved.get(&insn.addr) {
            Some(Resolved::Return) => Exit::Return,
            Some(Resolved::Target(t)) => Exit::TailCall(t & !1),
            None => Exit::Indirect("res"),
        };
        writeln!(o, "{}", self.exit(f, insn.addr, e)).unwrap();
    }

    #[allow(clippy::too_many_arguments)]
    fn mem(&self, f: &Function, insn: &Insn, load: bool, size: MemSize, rt: Reg, addr: &MemAddr, o: &mut String) {
        let rd = |r: Reg| -> String {
            if r == PC {
                format!("0x{:08x}u", insn.pc_value())
            } else {
                format!("ctx->r[{r}]")
            }
        };
        let (ea, writeback) = match addr {
            MemAddr::Abs(a) => (format!("0x{a:08x}u"), None),
            MemAddr::Base {
                rn,
                offset,
                add,
                pre_index,
                writeback,
            } => {
                let off = match offset {
                    MemOffset::Imm(n) => format!("{n}u"),
                    MemOffset::Reg(s) => shifted_value(insn, s),
                };
                let sign = if *add { "+" } else { "-" };
                writeln!(o, "uint32_t base = {};", rd(*rn)).unwrap();
                writeln!(o, "uint32_t addr = base {sign} {off};").unwrap();
                let ea = if *pre_index { "addr" } else { "base" };
                (ea.to_string(), writeback.then_some(*rn))
            }
        };
        if load {
            // `ldr pc, [sp], #4` pops a return address, and a veneer's `ldr pc, =target`
            // was resolved by discovery; neither needs the loaded value
            let pop = rt == PC
                && matches!(
                    addr,
                    MemAddr::Base {
                        rn: SP,
                        add: true,
                        pre_index: false,
                        ..
                    }
                );
            let resolved = if rt == PC {
                f.resolved.get(&insn.addr).copied()
            } else {
                None
            };
            match size {
                MemSize::Dual => writeln!(o, "uint32_t v0 = rd32({ea}), v1 = rd32({ea} + 4);").unwrap(),
                _ if pop || resolved.is_some() => {}
                _ => writeln!(o, "uint32_t v0 = {}({ea});", load_fn(size)).unwrap(),
            }
            if let Some(rn) = writeback {
                writeln!(o, "ctx->r[{rn}] = addr;").unwrap();
            }
            if size == MemSize::Dual {
                writeln!(o, "ctx->r[{rt}] = v0; ctx->r[{}] = v1;", rt + 1).unwrap();
            } else if rt == PC {
                let e = match resolved {
                    _ if pop => Exit::Return,
                    Some(Resolved::Target(t)) => Exit::TailCall(t & !1),
                    Some(Resolved::Return) => Exit::Return,
                    None => Exit::Indirect("v0"),
                };
                writeln!(o, "{}", self.exit(f, insn.addr, e)).unwrap();
            } else {
                writeln!(o, "ctx->r[{rt}] = v0;").unwrap();
            }
        } else {
            match size {
                MemSize::Dual => writeln!(o, "wr32({ea}, {}); wr32({ea} + 4, {});", rd(rt), rd(rt + 1)).unwrap(),
                _ => writeln!(o, "{}({ea}, {});", store_fn(size), rd(rt)).unwrap(),
            }
            if let Some(rn) = writeback {
                writeln!(o, "ctx->r[{rn}] = addr;").unwrap();
            }
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn multi(
        &self,
        f: &Function,
        insn: &Insn,
        load: bool,
        rn: Reg,
        regs: u16,
        mode: BlockMode,
        writeback: bool,
        o: &mut String,
    ) {
        let count = regs.count_ones();
        let base = if rn == PC {
            format!("0x{:08x}u", insn.pc_value())
        } else {
            format!("ctx->r[{rn}]")
        };
        writeln!(o, "uint32_t base = {base};").unwrap();
        // `pop {pc}` alone is a plain return: nothing to load
        if load && rn == SP && regs == 1 << PC {
            if writeback {
                writeln!(o, "ctx->r[{rn}] = base + 4;").unwrap();
            }
            writeln!(o, "{}", self.exit(f, insn.addr, Exit::Return)).unwrap();
            return;
        }
        let start = match mode {
            BlockMode::Ia => "base".to_string(),
            BlockMode::Ib => "base + 4".to_string(),
            BlockMode::Da => format!("base - {}", count * 4 - 4),
            BlockMode::Db => format!("base - {}", count * 4),
        };
        writeln!(o, "uint32_t a = {start};").unwrap();
        let list: Vec<Reg> = (0..16).filter(|r| regs & (1 << r) != 0).collect();
        let new_base = match mode {
            BlockMode::Ia | BlockMode::Ib => format!("base + {}", count * 4),
            _ => format!("base - {}", count * 4),
        };
        if load {
            // a popped PC is a return; its value isn't needed
            let pop_return = rn == SP && regs & (1 << PC) != 0;
            for (i, r) in list.iter().enumerate() {
                if !(pop_return && *r == PC) {
                    writeln!(o, "uint32_t v{i} = rd32(a + {});", i * 4).unwrap();
                }
            }
            if writeback {
                writeln!(o, "ctx->r[{rn}] = {new_base};").unwrap();
            }
            for (i, r) in list.iter().enumerate() {
                if *r != PC {
                    writeln!(o, "ctx->r[{r}] = v{i};").unwrap();
                }
            }
            if regs & (1 << PC) != 0 {
                let e = if rn == SP { Exit::Return } else { Exit::Indirect("") };
                match e {
                    Exit::Indirect(_) => {
                        let v = format!("v{}", list.len() - 1);
                        writeln!(o, "{}", self.exit(f, insn.addr, Exit::Indirect(&v))).unwrap()
                    }
                    e => writeln!(o, "{}", self.exit(f, insn.addr, e)).unwrap(),
                }
            }
        } else {
            for (i, r) in list.iter().enumerate() {
                let v = if *r == PC {
                    format!("0x{:08x}u", insn.pc_value())
                } else if *r == rn {
                    "base".to_string()
                } else {
                    format!("ctx->r[{r}]")
                };
                writeln!(o, "wr32(a + {}, {v});", i * 4).unwrap();
            }
            if writeback {
                writeln!(o, "ctx->r[{rn}] = {new_base};").unwrap();
            }
        }
    }
}

fn flags_nz(o: &mut String, res: &str, live: u8) {
    if live & N != 0 {
        writeln!(o, "ctx->n = {res} >> 31;").unwrap();
    }
    if live & Z != 0 {
        writeln!(o, "ctx->z = {res} == 0;").unwrap();
    }
}

fn half(reg: &str, top: bool) -> String {
    if top {
        format!("(int32_t)(int16_t)({reg} >> 16)")
    } else {
        format!("(int32_t)(int16_t){reg}")
    }
}

fn dual_products(o: &mut String, n: &str, m: &str, swap: bool) {
    let m = if swap { format!("ror_r({m}, 16)") } else { m.to_string() };
    writeln!(o, "uint32_t mm = {m};").unwrap();
    writeln!(o, "int32_t p1 = {} * {};", half(n, false), half("mm", false)).unwrap();
    writeln!(o, "int32_t p2 = {} * {};", half(n, true), half("mm", true)).unwrap();
}

/// rd = p + acc with the Q flag set on signed overflow (SMLAxy/SMLAWy).
fn q_add(o: &mut String, d: Reg, p: &str, acc: &str) {
    writeln!(o, "int64_t sum = (int64_t){p} + {acc};").unwrap();
    writeln!(o, "if (sum != (int32_t)sum) ctx->q = 1;").unwrap();
    writeln!(o, "ctx->r[{d}] = (uint32_t)sum;").unwrap();
}

fn load_fn(size: MemSize) -> &'static str {
    match size {
        MemSize::Word | MemSize::Dual => "rd32",
        MemSize::Byte => "rd8",
        MemSize::Half => "rd16",
        MemSize::SignedByte => "rds8",
        MemSize::SignedHalf => "rds16",
    }
}

fn store_fn(size: MemSize) -> &'static str {
    match size {
        MemSize::Byte | MemSize::SignedByte => "wr8",
        MemSize::Half | MemSize::SignedHalf => "wr16",
        _ => "wr32",
    }
}

fn fget(r: FReg) -> String {
    match r {
        FReg::S(n) => format!("get_f32(ctx, {n})"),
        FReg::D(n) => format!("get_f64(ctx, {n})"),
    }
}

fn fset(r: FReg, value: &str) -> String {
    match r {
        FReg::S(n) => format!("set_f32(ctx, {n}, {value});"),
        FReg::D(n) => format!("set_f64(ctx, {n}, {value});"),
    }
}

/// A shifted register's value (no carry), for addressing and saturation operands.
fn shifted_value(insn: &Insn, s: &Shifted) -> String {
    let rm = if s.rm == PC {
        format!("0x{:08x}u", insn.pc_value())
    } else {
        format!("ctx->r[{}]", s.rm)
    };
    match (s.kind, s.amount) {
        (ShiftKind::Lsl, ShiftAmount::Imm(0)) => rm,
        (ShiftKind::Lsl, ShiftAmount::Imm(n)) => format!("({rm} << {n})"),
        (ShiftKind::Lsr, ShiftAmount::Imm(32)) => "0u".to_string(),
        (ShiftKind::Lsr, ShiftAmount::Imm(n)) => format!("({rm} >> {n})"),
        (ShiftKind::Asr, ShiftAmount::Imm(n)) => format!("(uint32_t)((int32_t){rm} >> {})", n.min(31)),
        (ShiftKind::Ror, ShiftAmount::Imm(n)) => format!("ror_r({rm}, {n})"),
        (ShiftKind::Rrx, _) => format!("((ctx->c << 31) | ({rm} >> 1))"),
        (k, ShiftAmount::Reg(r)) => {
            let f = match k {
                ShiftKind::Lsl => "lsl_r",
                ShiftKind::Lsr => "lsr_r",
                ShiftKind::Asr => "asr_r",
                _ => "ror_r",
            };
            let amount = if f == "ror_r" {
                format!("ctx->r[{r}] & 0xFF")
            } else {
                format!("ctx->r[{r}]")
            };
            format!("{f}({rm}, {amount})")
        }
    }
}

/// Operand 2's value, plus its shifter carry-out when `need_carry` (None = C unchanged).
/// May emit setup statements.
fn operand2(insn: &Insn, op2: &Operand2, need_carry: bool, o: &mut String) -> (String, Option<String>) {
    match op2 {
        Operand2::Imm { value, rotated } => {
            let carry = (need_carry && *rotated).then(|| format!("{}u", value >> 31));
            (format!("0x{value:08x}u"), carry)
        }
        Operand2::Reg(s) => {
            if !need_carry {
                return (shifted_value(insn, s), None);
            }
            let rm = if s.rm == PC {
                format!("0x{:08x}u", insn.pc_value())
            } else {
                format!("ctx->r[{}]", s.rm)
            };
            match (s.kind, s.amount) {
                (ShiftKind::Lsl, ShiftAmount::Imm(0)) => (rm, None),
                (ShiftKind::Lsl, ShiftAmount::Imm(n)) => {
                    (format!("({rm} << {n})"), Some(format!("(({rm} >> {}) & 1)", 32 - n)))
                }
                (ShiftKind::Lsr, ShiftAmount::Imm(32)) => ("0u".into(), Some(format!("({rm} >> 31)"))),
                (ShiftKind::Lsr, ShiftAmount::Imm(n)) => {
                    (format!("({rm} >> {n})"), Some(format!("(({rm} >> {}) & 1)", n - 1)))
                }
                (ShiftKind::Asr, ShiftAmount::Imm(32)) => (
                    format!("(uint32_t)((int32_t){rm} >> 31)"),
                    Some(format!("({rm} >> 31)")),
                ),
                (ShiftKind::Asr, ShiftAmount::Imm(n)) => (
                    format!("(uint32_t)((int32_t){rm} >> {n})"),
                    Some(format!("(({rm} >> {}) & 1)", n - 1)),
                ),
                (ShiftKind::Ror, ShiftAmount::Imm(n)) => {
                    (format!("ror_r({rm}, {n})"), Some(format!("(({rm} >> {}) & 1)", n - 1)))
                }
                (ShiftKind::Rrx, _) => (format!("((ctx->c << 31) | ({rm} >> 1))"), Some(format!("({rm} & 1)"))),
                (k, ShiftAmount::Reg(r)) => {
                    let f = match k {
                        ShiftKind::Lsl => "lsl_rc",
                        ShiftKind::Lsr => "lsr_rc",
                        ShiftKind::Asr => "asr_rc",
                        _ => "ror_rc",
                    };
                    writeln!(o, "uint32_t shc = ctx->c;").unwrap();
                    writeln!(o, "uint32_t sh = {f}({rm}, ctx->r[{r}], &shc);").unwrap();
                    ("sh".into(), Some("shc".into()))
                }
            }
        }
    }
}

/// C statements for one instruction in isolation, with every flag treated as live.
/// Used by the differential tests; control flow and calls aren't meaningful here.
pub fn single_instruction(insn: &Insn) -> String {
    let empty = Analysis {
        functions: BTreeMap::new(),
        issues: Vec::new(),
        rejections: Vec::new(),
        data_words: BTreeSet::new(),
        pc_relative: BTreeSet::new(),
        pops_caller: BTreeSet::new(),
    };
    let noreturn = HashSet::new();
    let variants = Variants::new();
    let emitter = Emitter::new(&empty, &noreturn, &variants);
    let f = Function {
        entry: insn.addr,
        thumb: insn.thumb,
        insns: BTreeMap::from([(insn.addr, *insn)]),
        jump_tables: Vec::new(),
        exits: true,
        tail_calls: Vec::new(),
        resolved: BTreeMap::new(),
        manual_calls: BTreeMap::new(),
    };
    let body = emitter.insn(&f, insn, NZCV, None);
    if insn.cond == Cond::Al {
        format!("{{\n{body}}}\n")
    } else {
        format!("if ({}) {{\n{body}}}\n", cond_expr(insn.cond))
    }
}

/// Writes a file only if its contents changed, so unchanged output keeps its timestamp and
/// incremental builds don't recompile it.
fn write_if_changed(path: &std::path::Path, contents: &[u8]) -> Result<(), String> {
    if std::fs::read(path).is_ok_and(|old| old == contents) {
        return Ok(());
    }
    std::fs::write(path, contents).map_err(|e| format!("{}: {e}", path.display()))
}

pub struct EmitStats {
    pub functions: usize,
    pub files: usize,
    pub bytes: usize,
}

/// Writes `funcs.h`, `recomp_NNN.c` chunks and `lookup.c` into `out_dir`.
/// Checks that every differing instruction is one the generated code can choose between at run time:
/// straight-line ARM code in a discovered function, the same in every version but for its operands.
pub fn check_variants(analysis: &Analysis, variants: &Variants) -> Result<(), String> {
    let mut found = BTreeMap::new();
    for f in analysis.functions.values() {
        for (&addr, insn) in &f.insns {
            if variants.contains_key(&addr) {
                found.insert(addr, *insn);
            }
        }
    }
    for (&addr, words) in variants {
        let Some(insn) = found.get(&addr) else {
            return Err(format!("{addr:08x} differs between versions but isn't code in a function (data?)"));
        };
        if insn.thumb || !matches!(flow(insn), Flow::Next) {
            return Err(format!("{addr:08x}: {insn} differs between versions and isn't straight-line ARM code"));
        }
        for &w in words {
            match crate::arm::decode(addr, w) {
                Some(alt) if alt.cond == insn.cond && matches!(flow(&alt), Flow::Next) => {}
                _ => return Err(format!("{addr:08x}: {insn} is {w:08x} in another version, which changes control flow")),
            }
        }
    }
    Ok(())
}

pub fn write_all(
    analysis: &Analysis,
    noreturn: &HashSet<u32>,
    variants: &Variants,
    out_dir: &std::path::Path,
) -> Result<EmitStats, String> {
    let io = |e: std::io::Error| format!("{}: {e}", out_dir.display());
    std::fs::create_dir_all(out_dir).map_err(io)?;
    check_variants(analysis, variants)?;
    let emitter = Emitter::new(analysis, noreturn, variants);
    let header = "/* generated by tools/recomp; do not edit */\n";

    let mut funcs_h = format!(
        "{header}#ifndef FUNCS_H\n#define FUNCS_H\n#include \"recomp.h\"\n\n#ifdef __cplusplus\nextern \"C\" {{\n#endif\n\n"
    );
    for &addr in analysis.functions.keys() {
        writeln!(funcs_h, "void {}(Ctx *RECOMP_RESTRICT ctx);", fn_name(addr)).unwrap();
    }
    funcs_h.push_str("\n#ifdef __cplusplus\n}\n#endif\n\n#endif\n");
    write_if_changed(&out_dir.join("funcs.h"), funcs_h.as_bytes())?;

    // VMLA etc. round the product separately; forbid the compiler from fusing it
    let prelude = format!("{header}#include \"funcs.h\"\n#pragma STDC FP_CONTRACT OFF\n\n");
    let (mut files, mut bytes) = (0usize, funcs_h.len());
    // one file per 64 KiB of guest code, so a newly discovered function only changes its file
    let mut chunks: BTreeMap<u32, String> = BTreeMap::new();
    for f in analysis.functions.values() {
        let chunk = chunks.entry(f.entry >> 16).or_insert_with(|| prelude.clone());
        chunk.push_str(&emitter.function(f));
        chunk.push('\n');
    }
    let mut keep = HashSet::new();
    for (bucket, chunk) in &chunks {
        let name = format!("recomp_{bucket:04x}.c");
        write_if_changed(&out_dir.join(&name), chunk.as_bytes())?;
        bytes += chunk.len();
        files += 1;
        keep.insert(name);
    }
    // drop files from earlier runs that no longer have functions
    for entry in std::fs::read_dir(out_dir).map_err(io)?.flatten() {
        let name = entry.file_name().to_string_lossy().into_owned();
        if name.starts_with("recomp_") && name.ends_with(".c") && !keep.contains(&name) {
            std::fs::remove_file(entry.path()).map_err(io)?;
        }
    }

    let mut lookup =
        format!("{header}#include \"funcs.h\"\n#include <stddef.h>\n\nconst RecompEntry recomp_functions[] = {{\n");
    for &addr in analysis.functions.keys() {
        writeln!(lookup, "    {{0x{addr:08x}u, {}}},", fn_name(addr)).unwrap();
    }
    writeln!(
        lookup,
        "}};\nconst size_t recomp_function_count = {};",
        analysis.functions.len()
    )
    .unwrap();
    write_if_changed(&out_dir.join("lookup.c"), lookup.as_bytes())?;
    bytes += lookup.len();

    Ok(EmitStats {
        functions: analysis.functions.len(),
        files,
        bytes,
    })
}
