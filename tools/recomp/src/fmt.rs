//! UAL-style disassembly text for decoded instructions, using LLVM's choice of mnemonics
//! and aliases (push/pop, lsl for shifted mov, ...) so output can be diffed against
//! llvm-objdump.

use crate::insn::*;
use std::fmt::{self, Display, Write};

pub fn reg_name(r: Reg) -> &'static str {
    [
        "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc",
    ][r as usize & 15]
}

fn cond_str(c: Cond) -> &'static str {
    use Cond::*;
    match c {
        Eq => "eq",
        Ne => "ne",
        Cs => "hs",
        Cc => "lo",
        Mi => "mi",
        Pl => "pl",
        Vs => "vs",
        Vc => "vc",
        Hi => "hi",
        Ls => "ls",
        Ge => "ge",
        Lt => "lt",
        Gt => "gt",
        Le => "le",
        Al => "",
    }
}

fn shift_name(k: ShiftKind) -> &'static str {
    match k {
        ShiftKind::Lsl => "lsl",
        ShiftKind::Lsr => "lsr",
        ShiftKind::Asr => "asr",
        ShiftKind::Ror => "ror",
        ShiftKind::Rrx => "rrx",
    }
}

fn shifted(s: &Shifted) -> String {
    let rm = reg_name(s.rm);
    match (s.kind, s.amount) {
        (ShiftKind::Lsl, ShiftAmount::Imm(0)) => rm.to_string(),
        (ShiftKind::Rrx, _) => format!("{rm}, rrx"),
        (k, ShiftAmount::Imm(n)) => format!("{rm}, {} #{n}", shift_name(k)),
        (k, ShiftAmount::Reg(r)) => format!("{rm}, {} {}", shift_name(k), reg_name(r)),
    }
}

fn operand2(op: &Operand2) -> String {
    match op {
        Operand2::Imm { value, .. } => format!("#{}", *value as i32),
        Operand2::Reg(s) => shifted(s),
    }
}

fn freg(f: FReg) -> String {
    match f {
        FReg::S(n) => format!("s{n}"),
        FReg::D(n) => format!("d{n}"),
    }
}

fn reglist(regs: u16) -> String {
    let names: Vec<&str> = (0..16)
        .filter(|i| regs & (1 << i) != 0)
        .map(|i| reg_name(i as Reg))
        .collect();
    format!("{{{}}}", names.join(", "))
}

fn freglist(first: FReg, count: u8) -> String {
    let names: Vec<String> = (0..count)
        .map(|i| match first {
            FReg::S(n) => format!("s{}", n + i),
            FReg::D(n) => format!("d{}", n + i),
        })
        .collect();
    format!("{{{}}}", names.join(", "))
}

fn mem_addr(a: &MemAddr) -> String {
    match a {
        MemAddr::Abs(target) => format!("[{target:#x}]"),
        MemAddr::Base {
            rn,
            offset,
            add,
            pre_index,
            writeback,
        } => {
            let sign = if *add { "" } else { "-" };
            let off = match offset {
                MemOffset::Imm(0) if *pre_index => None,
                MemOffset::Imm(n) => Some(format!("#{sign}{n}")),
                MemOffset::Reg(s) => Some(format!("{sign}{}", shifted(s))),
            };
            let rn = reg_name(*rn);
            match (off, pre_index, writeback) {
                (None, _, _) => format!("[{rn}]"),
                (Some(o), true, false) => format!("[{rn}, {o}]"),
                (Some(o), true, true) => format!("[{rn}, {o}]!"),
                (Some(o), false, _) => format!("[{rn}], {o}"),
            }
        }
    }
}

fn num_suffix(t: NumType) -> &'static str {
    match t {
        NumType::F32 => "f32",
        NumType::F64 => "f64",
        NumType::S32 => "s32",
        NumType::U32 => "u32",
    }
}

fn ftype(f: FReg) -> &'static str {
    match f {
        FReg::S(_) => "f32",
        FReg::D(_) => "f64",
    }
}

fn halves(top: bool) -> char {
    if top { 't' } else { 'b' }
}

impl Display for Insn {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let c = cond_str(self.cond);
        let s_ = |s: bool| if s { "s" } else { "" };
        let mut out = String::new();
        let o = &mut out;
        match &self.op {
            Op::DataProc { op, s, rd, rn, op2 } => {
                let name = format!("{op:?}").to_lowercase();
                let (rd, rn) = (reg_name(*rd), reg_name(*rn));
                match (op, op2) {
                    (DpOp::Mov, Operand2::Reg(sh)) if sh.amount != ShiftAmount::Imm(0) || sh.kind != ShiftKind::Lsl => {
                        let k = shift_name(sh.kind);
                        let rm = reg_name(sh.rm);
                        match sh.amount {
                            _ if sh.kind == ShiftKind::Rrx => write!(o, "rrx{}{c} {rd}, {rm}", s_(*s)),
                            ShiftAmount::Imm(n) => write!(o, "{k}{}{c} {rd}, {rm}, #{n}", s_(*s)),
                            ShiftAmount::Reg(r) => write!(o, "{k}{}{c} {rd}, {rm}, {}", s_(*s), reg_name(r)),
                        }
                    }
                    _ if op.is_test() => write!(o, "{name}{c} {rn}, {}", operand2(op2)),
                    _ if op.is_move() => write!(o, "{name}{}{c} {rd}, {}", s_(*s), operand2(op2)),
                    _ => write!(o, "{name}{}{c} {rd}, {rn}, {}", s_(*s), operand2(op2)),
                }
            }
            Op::Mul { s, rd, rm, rs, acc } => match acc {
                None => write!(
                    o,
                    "mul{}{c} {}, {}, {}",
                    s_(*s),
                    reg_name(*rd),
                    reg_name(*rm),
                    reg_name(*rs)
                ),
                Some(a) => write!(
                    o,
                    "mla{}{c} {}, {}, {}, {}",
                    s_(*s),
                    reg_name(*rd),
                    reg_name(*rm),
                    reg_name(*rs),
                    reg_name(*a)
                ),
            },
            Op::MulLong {
                kind,
                s,
                rdlo,
                rdhi,
                rm,
                rs,
            } => write!(
                o,
                "{}{}{c} {}, {}, {}, {}",
                format!("{kind:?}").to_lowercase(),
                s_(*s),
                reg_name(*rdlo),
                reg_name(*rdhi),
                reg_name(*rm),
                reg_name(*rs)
            ),
            Op::Umaal { rdlo, rdhi, rm, rs } => write!(
                o,
                "umaal{c} {}, {}, {}, {}",
                reg_name(*rdlo),
                reg_name(*rdhi),
                reg_name(*rm),
                reg_name(*rs)
            ),
            Op::SmulHalf {
                rd,
                rm,
                rs,
                m_top,
                s_top,
                acc,
            } => {
                let xy = format!("{}{}", halves(*m_top), halves(*s_top));
                match acc {
                    None => write!(o, "smul{xy}{c} {}, {}, {}", reg_name(*rd), reg_name(*rm), reg_name(*rs)),
                    Some(a) => write!(
                        o,
                        "smla{xy}{c} {}, {}, {}, {}",
                        reg_name(*rd),
                        reg_name(*rm),
                        reg_name(*rs),
                        reg_name(*a)
                    ),
                }
            }
            Op::SmulWord { rd, rm, rs, s_top, acc } => {
                let y = halves(*s_top);
                match acc {
                    None => write!(o, "smulw{y}{c} {}, {}, {}", reg_name(*rd), reg_name(*rm), reg_name(*rs)),
                    Some(a) => write!(
                        o,
                        "smlaw{y}{c} {}, {}, {}, {}",
                        reg_name(*rd),
                        reg_name(*rm),
                        reg_name(*rs),
                        reg_name(*a)
                    ),
                }
            }
            Op::SmlalHalf {
                rdlo,
                rdhi,
                rm,
                rs,
                m_top,
                s_top,
            } => write!(
                o,
                "smlal{}{}{c} {}, {}, {}, {}",
                halves(*m_top),
                halves(*s_top),
                reg_name(*rdlo),
                reg_name(*rdhi),
                reg_name(*rm),
                reg_name(*rs)
            ),
            Op::DualMul {
                kind,
                swap,
                rd,
                rn,
                rm,
                acc,
            } => {
                let x = if *swap { "x" } else { "" };
                let name = match (kind, acc) {
                    (DualMulKind::Smlad, None) => "smuad",
                    (DualMulKind::Smlsd, None) => "smusd",
                    (DualMulKind::Smlad, _) => "smlad",
                    _ => "smlsd",
                };
                write!(
                    o,
                    "{name}{x}{c} {}, {}, {}",
                    reg_name(*rd),
                    reg_name(*rn),
                    reg_name(*rm)
                )?;
                if let Some(a) = acc {
                    write!(o, ", {}", reg_name(*a))?;
                }
                Ok(())
            }
            Op::DualMulLong {
                kind,
                swap,
                rdlo,
                rdhi,
                rn,
                rm,
            } => write!(
                o,
                "{}{}{c} {}, {}, {}, {}",
                format!("{kind:?}").to_lowercase(),
                if *swap { "x" } else { "" },
                reg_name(*rdlo),
                reg_name(*rdhi),
                reg_name(*rn),
                reg_name(*rm)
            ),
            Op::MulHigh {
                sub,
                round,
                rd,
                rn,
                rm,
                acc,
            } => {
                let name = match (sub, acc) {
                    (true, _) => "smmls",
                    (false, None) => "smmul",
                    (false, Some(_)) => "smmla",
                };
                let r = if *round { "r" } else { "" };
                write!(
                    o,
                    "{name}{r}{c} {}, {}, {}",
                    reg_name(*rd),
                    reg_name(*rn),
                    reg_name(*rm)
                )?;
                if let Some(a) = acc {
                    write!(o, ", {}", reg_name(*a))?;
                }
                Ok(())
            }
            Op::SatArith { op, rd, rm, rn } => write!(
                o,
                "{}{c} {}, {}, {}",
                format!("{op:?}").to_lowercase(),
                reg_name(*rd),
                reg_name(*rm),
                reg_name(*rn)
            ),
            Op::Clz { rd, rm } => write!(o, "clz{c} {}, {}", reg_name(*rd), reg_name(*rm)),
            Op::Sat { signed, rd, bits, rn } => write!(
                o,
                "{}sat{c} {}, #{bits}, {}",
                if *signed { "s" } else { "u" },
                reg_name(*rd),
                shifted(rn)
            ),
            Op::Sat16 { signed, rd, bits, rn } => write!(
                o,
                "{}sat16{c} {}, #{bits}, {}",
                if *signed { "s" } else { "u" },
                reg_name(*rd),
                reg_name(*rn)
            ),
            Op::Extend {
                signed,
                size,
                rd,
                rn,
                rm,
                rot,
            } => {
                let su = if *signed { "s" } else { "u" };
                let a = if rn.is_some() { "a" } else { "" };
                let sz = match size {
                    ExtendSize::Byte => "b",
                    ExtendSize::Half => "h",
                    ExtendSize::Byte16 => "b16",
                };
                write!(o, "{su}xt{a}{sz}{c} {}", reg_name(*rd))?;
                if let Some(n) = rn {
                    write!(o, ", {}", reg_name(*n))?;
                }
                write!(o, ", {}", reg_name(*rm))?;
                if *rot != 0 {
                    write!(o, ", ror #{rot}")?;
                }
                Ok(())
            }
            Op::Rev { kind, rd, rm } => write!(
                o,
                "{}{c} {}, {}",
                format!("{kind:?}").to_lowercase(),
                reg_name(*rd),
                reg_name(*rm)
            ),
            Op::Pkh { tb, rd, rn, rm, shift } => {
                let (name, k) = if *tb { ("pkhtb", "asr") } else { ("pkhbt", "lsl") };
                write!(o, "{name}{c} {}, {}, {}", reg_name(*rd), reg_name(*rn), reg_name(*rm))?;
                if *shift != 0 {
                    write!(o, ", {k} #{shift}")?;
                }
                Ok(())
            }
            Op::Sel { rd, rn, rm } => {
                write!(o, "sel{c} {}, {}, {}", reg_name(*rd), reg_name(*rn), reg_name(*rm))
            }
            Op::Parallel { prefix, op, rd, rn, rm } => write!(
                o,
                "{}{}{c} {}, {}, {}",
                format!("{prefix:?}").to_lowercase(),
                format!("{op:?}").to_lowercase(),
                reg_name(*rd),
                reg_name(*rn),
                reg_name(*rm)
            ),
            Op::Usad8 { rd, rn, rm, acc } => {
                let name = if acc.is_some() { "usada8" } else { "usad8" };
                write!(o, "{name}{c} {}, {}, {}", reg_name(*rd), reg_name(*rn), reg_name(*rm))?;
                if let Some(a) = acc {
                    write!(o, ", {}", reg_name(*a))?;
                }
                Ok(())
            }
            Op::Mem {
                load,
                size,
                rt,
                addr,
                user,
            } => {
                let base = if *load { "ldr" } else { "str" };
                let sz = match size {
                    MemSize::Word => "",
                    MemSize::Byte => "b",
                    MemSize::Half => "h",
                    MemSize::SignedByte => "sb",
                    MemSize::SignedHalf => "sh",
                    MemSize::Dual => "d",
                };
                let t = if *user { "t" } else { "" };
                write!(o, "{base}{sz}{t}{c} {}", reg_name(*rt))?;
                if *size == MemSize::Dual {
                    write!(o, ", {}", reg_name(rt + 1))?;
                }
                write!(o, ", {}", mem_addr(addr))
            }
            Op::Multi {
                load,
                rn,
                regs,
                mode,
                writeback,
                psr,
            } => {
                if *rn == SP && *writeback && !*psr {
                    if !*load && *mode == BlockMode::Db {
                        return write!(f, "push{c} {}", reglist(*regs));
                    }
                    if *load && *mode == BlockMode::Ia {
                        return write!(f, "pop{c} {}", reglist(*regs));
                    }
                }
                let base = if *load { "ldm" } else { "stm" };
                let m = match mode {
                    BlockMode::Ia => "",
                    BlockMode::Ib => "ib",
                    BlockMode::Da => "da",
                    BlockMode::Db => "db",
                };
                write!(
                    o,
                    "{base}{m}{c} {}{}, {}{}",
                    reg_name(*rn),
                    if *writeback { "!" } else { "" },
                    reglist(*regs),
                    if *psr { "^" } else { "" }
                )
            }
            Op::Swp { byte, rt, rt2, rn } => write!(
                o,
                "swp{}{c} {}, {}, [{}]",
                if *byte { "b" } else { "" },
                reg_name(*rt),
                reg_name(*rt2),
                reg_name(*rn)
            ),
            Op::Ldrex { size, rt, rn } => {
                let sz = ex_size(*size);
                write!(o, "ldrex{sz}{c} {}", reg_name(*rt))?;
                if *size == MemSize::Dual {
                    write!(o, ", {}", reg_name(rt + 1))?;
                }
                write!(o, ", [{}]", reg_name(*rn))
            }
            Op::Strex { size, rd, rt, rn } => {
                let sz = ex_size(*size);
                write!(o, "strex{sz}{c} {}, {}", reg_name(*rd), reg_name(*rt))?;
                if *size == MemSize::Dual {
                    write!(o, ", {}", reg_name(rt + 1))?;
                }
                write!(o, ", [{}]", reg_name(*rn))
            }
            Op::Clrex => write!(o, "clrex"),
            Op::Pld => write!(o, "pld"),
            Op::Branch { target, link, exchange } => {
                let name = match (link, exchange) {
                    (_, true) => "blx",
                    (true, false) => "bl",
                    (false, false) => "b",
                };
                write!(o, "{name}{c} {target:#x}")
            }
            Op::BranchReg { rm, link } => {
                write!(o, "{}{c} {}", if *link { "blx" } else { "bx" }, reg_name(*rm))
            }
            Op::Svc(n) => write!(o, "svc{c} #{n:#x}"),
            Op::Bkpt(n) => write!(o, "bkpt #{n:#x}"),
            Op::Udf => write!(o, "udf"),
            Op::Hint(h) => write!(o, "{}{c}", format!("{h:?}").to_lowercase()),
            Op::Mrs { rd, spsr } => {
                write!(o, "mrs{c} {}, {}", reg_name(*rd), if *spsr { "spsr" } else { "apsr" })
            }
            Op::Msr { spsr, src, .. } => {
                write!(o, "msr{c} {}, {}", if *spsr { "spsr" } else { "cpsr" }, operand2(src))
            }
            Op::Cps => write!(o, "cps"),
            Op::Setend { big } => write!(o, "setend {}", if *big { "be" } else { "le" }),
            Op::CoprocReg {
                load,
                coproc,
                opc1,
                rt,
                crn,
                crm,
                opc2,
            } => write!(
                o,
                "{}{c} p{coproc}, #{opc1}, {}, c{crn}, c{crm}, #{opc2}",
                if *load { "mrc" } else { "mcr" },
                reg_name(*rt)
            ),
            Op::VfpBinary { op, d, n, m } => write!(
                o,
                "v{}{c}.{} {}, {}, {}",
                format!("{op:?}").to_lowercase(),
                ftype(*d),
                freg(*d),
                freg(*n),
                freg(*m)
            ),
            Op::VfpUnary { op, d, m } => write!(
                o,
                "v{}{c}.{} {}, {}",
                format!("{op:?}").to_lowercase(),
                ftype(*d),
                freg(*d),
                freg(*m)
            ),
            Op::VfpCmp { quiet, d, m } => {
                let e = if *quiet { "" } else { "e" };
                let rhs = m.map(freg).unwrap_or_else(|| "#0".into());
                write!(o, "vcmp{e}{c}.{} {}, {rhs}", ftype(*d), freg(*d))
            }
            Op::VfpCvt {
                from,
                to,
                d,
                m,
                round_zero,
            } => {
                // float->int conversions using the FPSCR mode are spelled vcvtr
                let r = if matches!(to, NumType::S32 | NumType::U32) && !*round_zero {
                    "r"
                } else {
                    ""
                };
                write!(
                    o,
                    "vcvt{r}{c}.{}.{} {}, {}",
                    num_suffix(*to),
                    num_suffix(*from),
                    freg(*d),
                    freg(*m)
                )
            }
            Op::VmovSingle { to_core, rt, sn } => {
                if *to_core {
                    write!(o, "vmov{c} {}, s{sn}", reg_name(*rt))
                } else {
                    write!(o, "vmov{c} s{sn}, {}", reg_name(*rt))
                }
            }
            Op::VmovPair { to_core, rt, rt2, m } => {
                let regs = match m {
                    FReg::S(n) => format!("s{n}, s{}", n + 1),
                    FReg::D(n) => format!("d{n}"),
                };
                if *to_core {
                    write!(o, "vmov{c} {}, {}, {regs}", reg_name(*rt), reg_name(*rt2))
                } else {
                    write!(o, "vmov{c} {regs}, {}, {}", reg_name(*rt), reg_name(*rt2))
                }
            }
            Op::VmovHalf { to_core, rt, dn, high } => {
                let lane = format!("d{dn}[{}]", *high as u8);
                if *to_core {
                    write!(o, "vmov{c}.32 {}, {lane}", reg_name(*rt))
                } else {
                    write!(o, "vmov{c}.32 {lane}, {}", reg_name(*rt))
                }
            }
            Op::Vsys { to_core, rt, reg } => {
                let sys = match reg {
                    0 => "fpsid",
                    1 => "fpscr",
                    8 => "fpexc",
                    _ => "fpunknown",
                };
                let r = if *rt == PC && *to_core {
                    "APSR_nzcv"
                } else {
                    reg_name(*rt)
                };
                if *to_core {
                    write!(o, "vmrs{c} {r}, {sys}")
                } else {
                    write!(o, "vmsr{c} {sys}, {r}")
                }
            }
            Op::VMem { load, d, addr } => {
                write!(
                    o,
                    "{}{c} {}, {}",
                    if *load { "vldr" } else { "vstr" },
                    freg(*d),
                    mem_addr(addr)
                )
            }
            Op::VMulti {
                load,
                first,
                count,
                rn,
                mode,
                writeback,
            } => {
                let list = freglist(*first, *count);
                if *rn == SP && *writeback {
                    if !*load && *mode == BlockMode::Db {
                        return write!(f, "vpush{c} {list}");
                    }
                    if *load && *mode == BlockMode::Ia {
                        return write!(f, "vpop{c} {list}");
                    }
                }
                let m = if *mode == BlockMode::Db { "db" } else { "ia" };
                write!(
                    o,
                    "{}{m}{c} {}{}, {list}",
                    if *load { "vldm" } else { "vstm" },
                    reg_name(*rn),
                    if *writeback { "!" } else { "" }
                )
            }
        }?;
        f.write_str(&out)
    }
}

fn ex_size(s: MemSize) -> &'static str {
    match s {
        MemSize::Byte => "b",
        MemSize::Half => "h",
        MemSize::Dual => "d",
        _ => "",
    }
}
