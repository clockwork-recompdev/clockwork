//! ARM-mode (A32) decoder for ARMv6K + VFPv2, the ISA of the 3DS's ARM11 MPCore.
//!
//! Returns `None` for encodings that are undefined on ARMv6K or that user-mode game code
//! never uses (RFE/SRS, BXJ, generic coprocessor data ops). Those surface during
//! function discovery instead of being silently mistranslated.

use crate::insn::*;

fn bits(w: u32, hi: u32, lo: u32) -> u32 {
    (w >> lo) & ((1 << (hi - lo + 1)) - 1)
}

fn bit(w: u32, n: u32) -> bool {
    (w >> n) & 1 != 0
}

fn reg(w: u32, lo: u32) -> Reg {
    bits(w, lo + 3, lo) as Reg
}

fn sext(value: u32, width: u32) -> u32 {
    let shift = 32 - width;
    (((value << shift) as i32) >> shift) as u32
}

/// Immediate-shifted register operand (bits 11:0 with bit 4 clear).
fn shift_imm(w: u32) -> Shifted {
    let rm = reg(w, 0);
    let imm = bits(w, 11, 7) as u8;
    let (kind, amount) = match bits(w, 6, 5) {
        0 => (ShiftKind::Lsl, imm),
        1 => (ShiftKind::Lsr, if imm == 0 { 32 } else { imm }),
        2 => (ShiftKind::Asr, if imm == 0 { 32 } else { imm }),
        _ if imm == 0 => (ShiftKind::Rrx, 1),
        _ => (ShiftKind::Ror, imm),
    };
    Shifted {
        rm,
        kind,
        amount: ShiftAmount::Imm(amount),
    }
}

/// Register-shifted register operand (bits 11:0 with bit 4 set, bit 7 clear).
fn shift_reg(w: u32) -> Shifted {
    let kind = [ShiftKind::Lsl, ShiftKind::Lsr, ShiftKind::Asr, ShiftKind::Ror][bits(w, 6, 5) as usize];
    Shifted {
        rm: reg(w, 0),
        kind,
        amount: ShiftAmount::Reg(reg(w, 8)),
    }
}

fn rotated_imm(w: u32) -> Operand2 {
    let rot = bits(w, 11, 8) * 2;
    Operand2::Imm {
        value: bits(w, 7, 0).rotate_right(rot),
        rotated: rot != 0,
    }
}

/// Builds the address for an immediate/register offset, resolving PC-relative immediates.
fn mem_addr(addr: u32, rn: Reg, offset: MemOffset, add: bool, pre: bool, wb: bool) -> MemAddr {
    if rn == PC
        && pre
        && !wb
        && let MemOffset::Imm(imm) = offset
    {
        let base = addr.wrapping_add(8);
        return MemAddr::Abs(if add {
            base.wrapping_add(imm)
        } else {
            base.wrapping_sub(imm)
        });
    }
    MemAddr::Base {
        rn,
        offset,
        add,
        pre_index: pre,
        writeback: wb || !pre,
    }
}

pub fn decode(addr: u32, w: u32) -> Option<Insn> {
    let make = |cond, op| {
        Some(Insn {
            addr,
            raw: w,
            size: 4,
            thumb: false,
            cond,
            op,
        })
    };
    let Some(cond) = Cond::from_bits(w >> 28) else {
        return decode_unconditional(addr, w).and_then(|op| make(Cond::Al, op));
    };
    let op = match bits(w, 27, 25) {
        0b000 => decode_000(addr, w)?,
        0b001 => decode_001(w)?,
        0b010 => {
            let offset = MemOffset::Imm(bits(w, 11, 0));
            decode_word_byte(addr, w, offset)?
        }
        0b011 if !bit(w, 4) => decode_word_byte(addr, w, MemOffset::Reg(shift_imm(w)))?,
        0b011 => decode_media(w)?,
        0b100 => {
            let mode = match (bit(w, 24), bit(w, 23)) {
                (false, true) => BlockMode::Ia,
                (true, true) => BlockMode::Ib,
                (false, false) => BlockMode::Da,
                (true, false) => BlockMode::Db,
            };
            let regs = bits(w, 15, 0) as u16;
            if regs == 0 {
                return None;
            }
            Op::Multi {
                load: bit(w, 20),
                rn: reg(w, 16),
                regs,
                mode,
                writeback: bit(w, 21),
                psr: bit(w, 22),
            }
        }
        0b101 => Op::Branch {
            target: addr.wrapping_add(8).wrapping_add(sext(bits(w, 23, 0), 24) << 2),
            link: bit(w, 24),
            exchange: false,
        },
        0b110 => decode_coproc_mem(addr, w)?,
        _ if bit(w, 24) => Op::Svc(bits(w, 23, 0)),
        _ => decode_coproc(w)?,
    };
    if !vfpv2_regs(&op) {
        return None;
    }
    make(cond, op)
}

/// VFPv2 has d0-d15 only; encodings naming d16-d31 are VFPv3.
fn vfpv2_regs(op: &Op) -> bool {
    let ok = |f: &FReg| !matches!(f, FReg::D(n) if *n >= 16);
    match op {
        Op::VfpBinary { d, n, m, .. } => ok(d) && ok(n) && ok(m),
        Op::VfpUnary { d, m, .. } | Op::VfpCvt { d, m, .. } => ok(d) && ok(m),
        Op::VfpCmp { d, m, .. } => ok(d) && m.as_ref().is_none_or(ok),
        Op::VmovPair { m, .. } => ok(m),
        Op::VmovHalf { dn, .. } => *dn < 16,
        Op::VMem { d, .. } => ok(d),
        _ => true,
    }
}

fn decode_unconditional(addr: u32, w: u32) -> Option<Op> {
    if bits(w, 27, 25) == 0b101 {
        let target = addr
            .wrapping_add(8)
            .wrapping_add(sext(bits(w, 23, 0), 24) << 2)
            .wrapping_add((bit(w, 24) as u32) << 1);
        return Some(Op::Branch {
            target,
            link: true,
            exchange: true,
        });
    }
    if w == 0xF57F_F01F {
        return Some(Op::Clrex);
    }
    // PLD: 1111 01x1 U101 nnnn 1111 ...
    if bits(w, 27, 26) == 0b01 && bits(w, 22, 20) == 0b101 && bits(w, 15, 12) == 0xF && bit(w, 24) {
        return Some(Op::Pld);
    }
    if bits(w, 27, 20) == 0x10 {
        if bit(w, 16) && bits(w, 7, 4) == 0 {
            return Some(Op::Setend { big: bit(w, 9) });
        }
        if !bit(w, 16) {
            return Some(Op::Cps);
        }
    }
    None
}

/// Data processing (register), miscellaneous, multiplies, extra loads/stores.
fn decode_000(addr: u32, w: u32) -> Option<Op> {
    let op = bits(w, 24, 21);
    let s = bit(w, 20);

    if bit(w, 7) && bit(w, 4) {
        return if bits(w, 6, 5) == 0 {
            decode_mul_swap_ex(w)
        } else {
            decode_extra_mem(addr, w)
        };
    }

    // TST/TEQ/CMP/CMN without S: miscellaneous instructions
    if bits(w, 24, 23) == 0b10 && !s {
        return decode_misc(w);
    }

    let op2 = if bit(w, 4) {
        Operand2::Reg(shift_reg(w))
    } else {
        Operand2::Reg(shift_imm(w))
    };
    Some(Op::DataProc {
        op: DpOp::from_bits(op),
        s,
        rd: reg(w, 12),
        rn: reg(w, 16),
        op2,
    })
}

fn decode_misc(w: u32) -> Option<Op> {
    let op = bits(w, 22, 21);
    // fixed should-be-one / should-be-zero fields; real code always has them right
    let fixed = |hi, lo, value| bits(w, hi, lo) == value;
    Some(match bits(w, 7, 4) {
        0b0000 if op & 1 == 0 => {
            if !fixed(19, 16, 0xF) || !fixed(11, 0, 0) {
                return None;
            }
            Op::Mrs {
                rd: reg(w, 12),
                spsr: bit(w, 22),
            }
        }
        0b0000 if bits(w, 19, 16) == 0 || !fixed(15, 12, 0xF) || !fixed(11, 8, 0) => return None,
        0b0000 => Op::Msr {
            spsr: bit(w, 22),
            mask: bits(w, 19, 16) as u8,
            src: Operand2::Reg(Shifted::plain(reg(w, 0))),
        },
        0b0001 | 0b0011 if op == 0b01 && !fixed(19, 8, 0xFFF) => return None,
        0b0001 if op == 0b01 => Op::BranchReg {
            rm: reg(w, 0),
            link: false,
        },
        0b0001 if op == 0b11 && fixed(19, 16, 0xF) && fixed(11, 8, 0xF) => Op::Clz {
            rd: reg(w, 12),
            rm: reg(w, 0),
        },
        0b0011 if op == 0b01 => Op::BranchReg {
            rm: reg(w, 0),
            link: true,
        },
        0b0101 => Op::SatArith {
            op: [SatArith::Qadd, SatArith::Qsub, SatArith::Qdadd, SatArith::Qdsub][op as usize],
            rd: reg(w, 12),
            rm: reg(w, 0),
            rn: reg(w, 16),
        },
        0b0111 if op == 0b01 => Op::Bkpt(((bits(w, 19, 8) << 4) | bits(w, 3, 0)) as u16),
        x if x & 0b1001 == 0b1000 => {
            // signed halfword multiplies; bit 5 = x (rm half), bit 6 = y (rs half)
            let (rd, rm, rs) = (reg(w, 16), reg(w, 0), reg(w, 8));
            let (m_top, s_top) = (bit(w, 5), bit(w, 6));
            match op {
                0b00 => Op::SmulHalf {
                    rd,
                    rm,
                    rs,
                    m_top,
                    s_top,
                    acc: Some(reg(w, 12)),
                },
                0b01 if !m_top => Op::SmulWord {
                    rd,
                    rm,
                    rs,
                    s_top,
                    acc: Some(reg(w, 12)),
                },
                0b01 => Op::SmulWord {
                    rd,
                    rm,
                    rs,
                    s_top,
                    acc: None,
                },
                0b10 => Op::SmlalHalf {
                    rdlo: reg(w, 12),
                    rdhi: rd,
                    rm,
                    rs,
                    m_top,
                    s_top,
                },
                _ => Op::SmulHalf {
                    rd,
                    rm,
                    rs,
                    m_top,
                    s_top,
                    acc: None,
                },
            }
        }
        _ => return None,
    })
}

fn decode_mul_swap_ex(w: u32) -> Option<Op> {
    let (rn, rd, rs, rm) = (reg(w, 16), reg(w, 12), reg(w, 8), reg(w, 0));
    let s = bit(w, 20);
    if !bit(w, 24) {
        return Some(match bits(w, 23, 21) {
            0b000 => Op::Mul {
                s,
                rd: rn,
                rm,
                rs,
                acc: None,
            },
            0b001 => Op::Mul {
                s,
                rd: rn,
                rm,
                rs,
                acc: Some(rd),
            },
            0b010 if !s => Op::Umaal {
                rdlo: rd,
                rdhi: rn,
                rm,
                rs,
            },
            0b100 => Op::MulLong {
                kind: MulLongKind::Umull,
                s,
                rdlo: rd,
                rdhi: rn,
                rm,
                rs,
            },
            0b101 => Op::MulLong {
                kind: MulLongKind::Umlal,
                s,
                rdlo: rd,
                rdhi: rn,
                rm,
                rs,
            },
            0b110 => Op::MulLong {
                kind: MulLongKind::Smull,
                s,
                rdlo: rd,
                rdhi: rn,
                rm,
                rs,
            },
            0b111 => Op::MulLong {
                kind: MulLongKind::Smlal,
                s,
                rdlo: rd,
                rdhi: rn,
                rm,
                rs,
            },
            _ => return None,
        });
    }
    if !bit(w, 23) {
        return (bits(w, 21, 20) == 0 && bits(w, 11, 8) == 0).then_some(Op::Swp {
            byte: bit(w, 22),
            rt: rd,
            rt2: rm,
            rn,
        });
    }
    let size = [MemSize::Word, MemSize::Dual, MemSize::Byte, MemSize::Half][bits(w, 22, 21) as usize];
    Some(if s {
        Op::Ldrex { size, rt: rd, rn }
    } else {
        Op::Strex { size, rd, rt: rm, rn }
    })
}

/// LDRH/STRH/LDRSB/LDRSH/LDRD/STRD.
fn decode_extra_mem(addr: u32, w: u32) -> Option<Op> {
    let load = bit(w, 20);
    let size = match (bits(w, 6, 5), load) {
        (0b01, _) => MemSize::Half,
        (0b10, true) => MemSize::SignedByte,
        (0b11, true) => MemSize::SignedHalf,
        (_, false) => MemSize::Dual,
        _ => return None,
    };
    // LDRD is encoded with L=0 and op 0b10; STRD with L=0 and op 0b11
    let load = if size == MemSize::Dual {
        bits(w, 6, 5) == 0b10
    } else {
        load
    };
    let rt = reg(w, 12);
    if size == MemSize::Dual && (rt % 2 == 1 || rt == LR) {
        return None;
    }
    let offset = if bit(w, 22) {
        MemOffset::Imm((bits(w, 11, 8) << 4) | bits(w, 3, 0))
    } else {
        MemOffset::Reg(Shifted::plain(reg(w, 0)))
    };
    let (pre, wb) = (bit(w, 24), bit(w, 21));
    if !pre && wb {
        return None; // LDRHT etc. are ARMv6T2
    }
    Some(Op::Mem {
        load,
        size,
        rt,
        addr: mem_addr(addr, reg(w, 16), offset, bit(w, 23), pre, wb),
        user: false,
    })
}

/// Data processing (immediate), MSR immediate, hints.
fn decode_001(w: u32) -> Option<Op> {
    if bits(w, 24, 23) == 0b10 && !bit(w, 20) {
        if !bit(w, 21) {
            return None; // MOVW/MOVT are ARMv6T2
        }
        let mask = bits(w, 19, 16) as u8;
        if mask == 0 && !bit(w, 22) {
            return Some(Op::Hint(match bits(w, 7, 0) {
                0 => Hint::Nop,
                1 => Hint::Yield,
                2 => Hint::Wfe,
                3 => Hint::Wfi,
                4 => Hint::Sev,
                _ => return None,
            }));
        }
        if mask == 0 || bits(w, 15, 12) != 0xF {
            return None;
        }
        return Some(Op::Msr {
            spsr: bit(w, 22),
            mask,
            src: rotated_imm(w),
        });
    }
    Some(Op::DataProc {
        op: DpOp::from_bits(bits(w, 24, 21)),
        s: bit(w, 20),
        rd: reg(w, 12),
        rn: reg(w, 16),
        op2: rotated_imm(w),
    })
}

/// LDR/STR/LDRB/STRB (and the T variants).
fn decode_word_byte(addr: u32, w: u32, offset: MemOffset) -> Option<Op> {
    let (pre, wb) = (bit(w, 24), bit(w, 21));
    Some(Op::Mem {
        load: bit(w, 20),
        size: if bit(w, 22) { MemSize::Byte } else { MemSize::Word },
        rt: reg(w, 12),
        addr: mem_addr(addr, reg(w, 16), offset, bit(w, 23), pre, wb && pre),
        user: !pre && wb,
    })
}

fn decode_media(w: u32) -> Option<Op> {
    let (rn, rd, rm) = (reg(w, 16), reg(w, 12), reg(w, 0));
    let op1 = bits(w, 22, 20);
    let op2 = bits(w, 7, 5);
    match bits(w, 24, 23) {
        0b00 => {
            let prefix = match op1 {
                0b001 => ParallelPrefix::S,
                0b010 => ParallelPrefix::Q,
                0b011 => ParallelPrefix::Sh,
                0b101 => ParallelPrefix::U,
                0b110 => ParallelPrefix::Uq,
                0b111 => ParallelPrefix::Uh,
                _ => return None,
            };
            let op = match op2 {
                0b000 => ParallelOp::Add16,
                0b001 => ParallelOp::Asx,
                0b010 => ParallelOp::Sax,
                0b011 => ParallelOp::Sub16,
                0b100 => ParallelOp::Add8,
                0b111 => ParallelOp::Sub8,
                _ => return None,
            };
            Some(Op::Parallel { prefix, op, rd, rn, rm })
        }
        0b01 => {
            let rot = (bits(w, 11, 10) * 8) as u8;
            let ext = |signed, size| {
                Some(Op::Extend {
                    signed,
                    size,
                    rd,
                    rn: (rn != PC).then_some(rn),
                    rm,
                    rot,
                })
            };
            let sat_bits = bits(w, 20, 16) as u8;
            match (op1, op2) {
                (0b000, x) if x & 1 == 0 => Some(Op::Pkh {
                    tb: bit(w, 6),
                    rd,
                    rn,
                    rm,
                    shift: match bits(w, 11, 7) as u8 {
                        // PKHTB with shift 0 means ASR #32
                        0 if bit(w, 6) => 32,
                        n => n,
                    },
                }),
                (0b000, 0b011) => ext(true, ExtendSize::Byte16),
                (0b000, 0b101) => Some(Op::Sel { rd, rn, rm }),
                (0b010 | 0b011, x) if x & 1 == 0 => Some(Op::Sat {
                    signed: true,
                    rd,
                    bits: sat_bits + 1,
                    rn: shift_imm_sat(w),
                }),
                (0b110 | 0b111, x) if x & 1 == 0 => Some(Op::Sat {
                    signed: false,
                    rd,
                    bits: sat_bits,
                    rn: shift_imm_sat(w),
                }),
                (0b010, 0b001) => Some(Op::Sat16 {
                    signed: true,
                    rd,
                    bits: bits(w, 19, 16) as u8 + 1,
                    rn: rm,
                }),
                (0b110, 0b001) => Some(Op::Sat16 {
                    signed: false,
                    rd,
                    bits: bits(w, 19, 16) as u8,
                    rn: rm,
                }),
                (0b010, 0b011) => ext(true, ExtendSize::Byte),
                (0b011, 0b011) => ext(true, ExtendSize::Half),
                (0b100, 0b011) => ext(false, ExtendSize::Byte16),
                (0b110, 0b011) => ext(false, ExtendSize::Byte),
                (0b111, 0b011) => ext(false, ExtendSize::Half),
                (0b011, 0b001) => Some(Op::Rev {
                    kind: RevKind::Rev,
                    rd,
                    rm,
                }),
                (0b011, 0b101) => Some(Op::Rev {
                    kind: RevKind::Rev16,
                    rd,
                    rm,
                }),
                (0b111, 0b101) => Some(Op::Rev {
                    kind: RevKind::Revsh,
                    rd,
                    rm,
                }),
                _ => None,
            }
        }
        0b10 => {
            // here rd is bits 19:16 and the accumulator is bits 15:12
            let (rd, ra, rm, rn) = (reg(w, 16), reg(w, 12), reg(w, 8), reg(w, 0));
            let acc = (ra != PC).then_some(ra);
            let swap = bit(w, 5);
            match (op1, op2 & 0b110) {
                (0b000, 0b000) => Some(Op::DualMul {
                    kind: DualMulKind::Smlad,
                    swap,
                    rd,
                    rn,
                    rm,
                    acc,
                }),
                (0b000, 0b010) => Some(Op::DualMul {
                    kind: DualMulKind::Smlsd,
                    swap,
                    rd,
                    rn,
                    rm,
                    acc,
                }),
                (0b100, 0b000) => Some(Op::DualMulLong {
                    kind: DualMulKind::Smlald,
                    swap,
                    rdlo: ra,
                    rdhi: rd,
                    rn,
                    rm,
                }),
                (0b100, 0b010) => Some(Op::DualMulLong {
                    kind: DualMulKind::Smlsld,
                    swap,
                    rdlo: ra,
                    rdhi: rd,
                    rn,
                    rm,
                }),
                (0b101, 0b000) => Some(Op::MulHigh {
                    sub: false,
                    round: swap,
                    rd,
                    rn,
                    rm,
                    acc,
                }),
                (0b101, 0b110) => Some(Op::MulHigh {
                    sub: true,
                    round: swap,
                    rd,
                    rn,
                    rm,
                    acc: Some(ra),
                }),
                _ => None,
            }
        }
        _ => {
            if op1 == 0b000 && op2 == 0b000 {
                let (rd, ra, rm, rn) = (reg(w, 16), reg(w, 12), reg(w, 8), reg(w, 0));
                return Some(Op::Usad8 {
                    rd,
                    rn,
                    rm,
                    acc: (ra != PC).then_some(ra),
                });
            }
            if bits(w, 27, 20) == 0x7F && bits(w, 7, 4) == 0xF {
                return Some(Op::Udf);
            }
            None
        }
    }
}

/// SSAT/USAT shift: bit 6 selects ASR (amount 0 = 32) vs LSL.
fn shift_imm_sat(w: u32) -> Shifted {
    let amount = bits(w, 11, 7) as u8;
    if bit(w, 6) {
        Shifted {
            rm: reg(w, 0),
            kind: ShiftKind::Asr,
            amount: ShiftAmount::Imm(if amount == 0 { 32 } else { amount }),
        }
    } else {
        Shifted {
            rm: reg(w, 0),
            kind: ShiftKind::Lsl,
            amount: ShiftAmount::Imm(amount),
        }
    }
}

fn sreg(hi4: u32, lo1: bool) -> u8 {
    ((hi4 << 1) | lo1 as u32) as u8
}

fn dreg(lo4: u32, hi1: bool) -> u8 {
    (lo4 | ((hi1 as u32) << 4)) as u8
}

/// VFP loads/stores (VLDR/VSTR/VLDM/VSTM) and the two-register VMOVs.
fn decode_coproc_mem(addr: u32, w: u32) -> Option<Op> {
    let coproc = bits(w, 11, 8);
    if coproc != 10 && coproc != 11 {
        return None;
    }
    let double = coproc == 11;
    let (p, u, d, wb, l) = (bit(w, 24), bit(w, 23), bit(w, 22), bit(w, 21), bit(w, 20));
    let rn = reg(w, 16);
    let vd = bits(w, 15, 12);
    let first = if double {
        FReg::D(dreg(vd, d))
    } else {
        FReg::S(sreg(vd, d))
    };
    let imm8 = bits(w, 7, 0);

    if !p && !u && !wb {
        // 64-bit transfers between two core registers and VFP registers
        if !d {
            return None;
        }
        let m = if double {
            FReg::D(dreg(bits(w, 3, 0), bit(w, 5)))
        } else {
            FReg::S(sreg(bits(w, 3, 0), bit(w, 5)))
        };
        if bits(w, 7, 6) != 0 || !bit(w, 4) {
            return None;
        }
        return Some(Op::VmovPair {
            to_core: l,
            rt: reg(w, 12),
            rt2: reg(w, 16),
            m,
        });
    }

    if p && !wb {
        let offset = MemOffset::Imm(imm8 * 4);
        let addr = if rn == PC {
            let base = addr.wrapping_add(8) & !3;
            MemAddr::Abs(if u {
                base.wrapping_add(imm8 * 4)
            } else {
                base.wrapping_sub(imm8 * 4)
            })
        } else {
            MemAddr::Base {
                rn,
                offset,
                add: u,
                pre_index: true,
                writeback: false,
            }
        };
        return Some(Op::VMem {
            load: l,
            d: first,
            addr,
        });
    }

    let mode = match (p, u) {
        (false, true) => BlockMode::Ia,
        (true, false) if wb => BlockMode::Db,
        _ => return None,
    };
    // FLDMX/FSTMX (odd imm8 with doubles) transfer imm8/2 doubles too
    let count = if double { imm8 / 2 } else { imm8 };
    let limit = if double { 16 } else { 32 };
    let start = match first {
        FReg::S(n) | FReg::D(n) => n as u32,
    };
    if count == 0 || start + count > limit {
        return None;
    }
    Some(Op::VMulti {
        load: l,
        first,
        count: count as u8,
        rn,
        mode,
        writeback: wb,
    })
}

fn decode_coproc(w: u32) -> Option<Op> {
    let coproc = bits(w, 11, 8);
    if coproc != 10 && coproc != 11 {
        if !bit(w, 4) {
            return None; // CDP
        }
        return Some(Op::CoprocReg {
            load: bit(w, 20),
            coproc: coproc as u8,
            opc1: bits(w, 23, 21) as u8,
            rt: reg(w, 12),
            crn: bits(w, 19, 16) as u8,
            crm: bits(w, 3, 0) as u8,
            opc2: bits(w, 7, 5) as u8,
        });
    }
    let double = coproc == 11;

    if bit(w, 4) {
        // register transfers
        let rt = reg(w, 12);
        let to_core = bit(w, 20);
        return match bits(w, 23, 21) {
            0b000 if !double => Some(Op::VmovSingle {
                to_core,
                rt,
                sn: sreg(bits(w, 19, 16), bit(w, 7)),
            }),
            0b000 | 0b001 if double => Some(Op::VmovHalf {
                to_core,
                rt,
                dn: dreg(bits(w, 19, 16), bit(w, 7)),
                high: bit(w, 21),
            }),
            0b111 if !double => Some(Op::Vsys {
                to_core,
                rt,
                reg: bits(w, 19, 16) as u8,
            }),
            _ => None,
        };
    }

    let (vd, vn, vm) = (bits(w, 15, 12), bits(w, 19, 16), bits(w, 3, 0));
    let (dbit, nbit, mbit) = (bit(w, 22), bit(w, 7), bit(w, 5));
    let fd = |dbl: bool| {
        if dbl {
            FReg::D(dreg(vd, dbit))
        } else {
            FReg::S(sreg(vd, dbit))
        }
    };
    let fn_ = |dbl: bool| {
        if dbl {
            FReg::D(dreg(vn, nbit))
        } else {
            FReg::S(sreg(vn, nbit))
        }
    };
    let fm = |dbl: bool| {
        if dbl {
            FReg::D(dreg(vm, mbit))
        } else {
            FReg::S(sreg(vm, mbit))
        }
    };

    let pqrs = ((bit(w, 23) as u32) << 3) | ((bit(w, 21) as u32) << 2) | ((bit(w, 20) as u32) << 1) | bit(w, 6) as u32;
    let binary = |op| {
        Some(Op::VfpBinary {
            op,
            d: fd(double),
            n: fn_(double),
            m: fm(double),
        })
    };
    match pqrs {
        0b0000 => return binary(VfpBinary::Mla),
        0b0001 => return binary(VfpBinary::Mls),
        0b0010 => return binary(VfpBinary::Nmls),
        0b0011 => return binary(VfpBinary::Nmla),
        0b0100 => return binary(VfpBinary::Mul),
        0b0101 => return binary(VfpBinary::Nmul),
        0b0110 => return binary(VfpBinary::Add),
        0b0111 => return binary(VfpBinary::Sub),
        0b1000 => return binary(VfpBinary::Div),
        0b1111 => {}
        _ => return None,
    }

    // extension instructions: opcode in Vn, with N as an extra bit
    let ty = if double { NumType::F64 } else { NumType::F32 };
    let unary = |op| {
        Some(Op::VfpUnary {
            op,
            d: fd(double),
            m: fm(double),
        })
    };
    let cvt = |from, to, d, m, round_zero| {
        Some(Op::VfpCvt {
            from,
            to,
            d,
            m,
            round_zero,
        })
    };
    match (vn, nbit) {
        (0b0000, false) => unary(VfpUnary::Mov),
        (0b0000, true) => unary(VfpUnary::Abs),
        (0b0001, false) => unary(VfpUnary::Neg),
        (0b0001, true) => unary(VfpUnary::Sqrt),
        (0b0100, e) => Some(Op::VfpCmp {
            quiet: !e,
            d: fd(double),
            m: Some(fm(double)),
        }),
        (0b0101, e) if vm == 0 && !mbit => Some(Op::VfpCmp {
            quiet: !e,
            d: fd(double),
            m: None,
        }),
        // single <-> double: destination is the other precision
        (0b0111, true) => {
            let to = if double { NumType::F32 } else { NumType::F64 };
            cvt(ty, to, fd(!double), fm(double), false)
        }
        // int (always in an S register) -> float
        (0b1000, signed) => {
            let from = if signed { NumType::S32 } else { NumType::U32 };
            cvt(from, ty, fd(double), fm(false), false)
        }
        // float -> int (always into an S register)
        (0b1100, rz) => cvt(ty, NumType::U32, fd(false), fm(double), rz),
        (0b1101, rz) => cvt(ty, NumType::S32, fd(false), fm(double), rz),
        _ => None,
    }
}
