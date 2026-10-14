//! Thumb (T16) decoder for ARMv6K. The ARM11 has no Thumb-2, so apart from the BL/BLX
//! prefix+suffix pair every instruction is 16 bits.
//!
//! Instructions are decoded to their ARM equivalents. Flag-setting follows ARMv6 rules:
//! low-register data processing always sets flags; hi-register ADD/MOV and SP adjustments don't.

use crate::insn::*;

fn bits(w: u32, hi: u32, lo: u32) -> u32 {
    (w >> lo) & ((1 << (hi - lo + 1)) - 1)
}

fn bit(w: u32, n: u32) -> bool {
    (w >> n) & 1 != 0
}

fn r3(w: u32, lo: u32) -> Reg {
    bits(w, lo + 2, lo) as Reg
}

fn sext(value: u32, width: u32) -> u32 {
    let shift = 32 - width;
    (((value << shift) as i32) >> shift) as u32
}

fn imm(value: u32) -> Operand2 {
    Operand2::Imm { value, rotated: false }
}

fn plain(rm: Reg) -> Operand2 {
    Operand2::Reg(Shifted::plain(rm))
}

fn dp(op: DpOp, s: bool, rd: Reg, rn: Reg, op2: Operand2) -> Op {
    Op::DataProc { op, s, rd, rn, op2 }
}

fn base(rn: Reg, offset: MemOffset) -> MemAddr {
    MemAddr::Base {
        rn,
        offset,
        add: true,
        pre_index: true,
        writeback: false,
    }
}

/// Decodes the instruction at `addr`. `next` is the following halfword, needed for BL/BLX.
pub fn decode(addr: u32, hw: u16, next: Option<u16>) -> Option<Insn> {
    let w = hw as u32;
    let pc = addr.wrapping_add(4);
    let mut cond = Cond::Al;
    let mut size = 2;
    let mut raw = w;

    let op = match bits(w, 15, 11) {
        // LSL/LSR/ASR #imm  ->  MOVS rd, rm, <shift> #imm
        0b00000..=0b00010 => {
            let amount = bits(w, 10, 6) as u8;
            let (kind, amount) = match bits(w, 12, 11) {
                0 => (ShiftKind::Lsl, amount),
                1 => (ShiftKind::Lsr, if amount == 0 { 32 } else { amount }),
                _ => (ShiftKind::Asr, if amount == 0 { 32 } else { amount }),
            };
            let shifted = Shifted {
                rm: r3(w, 3),
                kind,
                amount: ShiftAmount::Imm(amount),
            };
            dp(DpOp::Mov, true, r3(w, 0), 0, Operand2::Reg(shifted))
        }
        0b00011 => {
            let op = if bit(w, 9) { DpOp::Sub } else { DpOp::Add };
            let op2 = if bit(w, 10) {
                imm(bits(w, 8, 6))
            } else {
                plain(r3(w, 6))
            };
            dp(op, true, r3(w, 0), r3(w, 3), op2)
        }
        0b00100..=0b00111 => {
            let rd = r3(w, 8);
            let op = [DpOp::Mov, DpOp::Cmp, DpOp::Add, DpOp::Sub][bits(w, 12, 11) as usize];
            dp(op, true, rd, rd, imm(bits(w, 7, 0)))
        }
        0b01000 if !bit(w, 10) => decode_alu(w),
        0b01000 => decode_hi(w)?,
        0b01001 => {
            let target = (pc & !3).wrapping_add(bits(w, 7, 0) * 4);
            Op::Mem {
                load: true,
                size: MemSize::Word,
                rt: r3(w, 8),
                addr: MemAddr::Abs(target),
                user: false,
            }
        }
        0b01010 | 0b01011 => {
            let (load, size) = [
                (false, MemSize::Word),
                (false, MemSize::Half),
                (false, MemSize::Byte),
                (true, MemSize::SignedByte),
                (true, MemSize::Word),
                (true, MemSize::Half),
                (true, MemSize::Byte),
                (true, MemSize::SignedHalf),
            ][bits(w, 11, 9) as usize];
            let addr = base(r3(w, 3), MemOffset::Reg(Shifted::plain(r3(w, 6))));
            Op::Mem {
                load,
                size,
                rt: r3(w, 0),
                addr,
                user: false,
            }
        }
        0b01100..=0b01111 => {
            let byte = bit(w, 12);
            let offset = bits(w, 10, 6) * if byte { 1 } else { 4 };
            Op::Mem {
                load: bit(w, 11),
                size: if byte { MemSize::Byte } else { MemSize::Word },
                rt: r3(w, 0),
                addr: base(r3(w, 3), MemOffset::Imm(offset)),
                user: false,
            }
        }
        0b10000 | 0b10001 => Op::Mem {
            load: bit(w, 11),
            size: MemSize::Half,
            rt: r3(w, 0),
            addr: base(r3(w, 3), MemOffset::Imm(bits(w, 10, 6) * 2)),
            user: false,
        },
        0b10010 | 0b10011 => Op::Mem {
            load: bit(w, 11),
            size: MemSize::Word,
            rt: r3(w, 8),
            addr: base(SP, MemOffset::Imm(bits(w, 7, 0) * 4)),
            user: false,
        },
        // ADR (PC, word-aligned) / ADD rd, SP, #imm
        0b10100 => dp(
            DpOp::Mov,
            false,
            r3(w, 8),
            0,
            imm((pc & !3).wrapping_add(bits(w, 7, 0) * 4)),
        ),
        0b10101 => dp(DpOp::Add, false, r3(w, 8), SP, imm(bits(w, 7, 0) * 4)),
        0b10110 | 0b10111 => decode_misc(w)?,
        0b11000 | 0b11001 => {
            let rn = r3(w, 8);
            let regs = bits(w, 7, 0) as u16;
            if regs == 0 {
                return None;
            }
            let load = bit(w, 11);
            // LDMIA doesn't write back when the base register is loaded
            let writeback = !(load && regs & (1 << rn) != 0);
            Op::Multi {
                load,
                rn,
                regs,
                mode: BlockMode::Ia,
                writeback,
                psr: false,
            }
        }
        0b11010 | 0b11011 => match bits(w, 11, 8) {
            0b1110 => Op::Udf,
            0b1111 => Op::Svc(bits(w, 7, 0)),
            c => {
                cond = Cond::from_bits(c)?;
                Op::Branch {
                    target: pc.wrapping_add(sext(bits(w, 7, 0), 8) << 1),
                    link: false,
                    exchange: false,
                }
            }
        },
        0b11100 => Op::Branch {
            target: pc.wrapping_add(sext(bits(w, 10, 0), 11) << 1),
            link: false,
            exchange: false,
        },
        0b11110 => {
            let suffix = next? as u32;
            let exchange = match bits(suffix, 15, 11) {
                0b11111 => false,
                0b11101 if !bit(suffix, 0) => true,
                _ => return None,
            };
            let mut target = pc
                .wrapping_add(sext(bits(w, 10, 0), 11) << 12)
                .wrapping_add(bits(suffix, 10, 0) << 1);
            if exchange {
                target &= !3;
            }
            size = 4;
            raw = (w << 16) | suffix;
            Op::Branch {
                target,
                link: true,
                exchange,
            }
        }
        // a lone BL/BLX suffix
        _ => return None,
    };
    Some(Insn {
        addr,
        raw,
        size,
        thumb: true,
        cond,
        op,
    })
}

/// Format 4: two-register data processing, all flag-setting.
fn decode_alu(w: u32) -> Op {
    let (rd, rm) = (r3(w, 0), r3(w, 3));
    let shift_by_reg = |kind| {
        let shifted = Shifted {
            rm: rd,
            kind,
            amount: ShiftAmount::Reg(rm),
        };
        dp(DpOp::Mov, true, rd, 0, Operand2::Reg(shifted))
    };
    match bits(w, 9, 6) {
        0x0 => dp(DpOp::And, true, rd, rd, plain(rm)),
        0x1 => dp(DpOp::Eor, true, rd, rd, plain(rm)),
        0x2 => shift_by_reg(ShiftKind::Lsl),
        0x3 => shift_by_reg(ShiftKind::Lsr),
        0x4 => shift_by_reg(ShiftKind::Asr),
        0x5 => dp(DpOp::Adc, true, rd, rd, plain(rm)),
        0x6 => dp(DpOp::Sbc, true, rd, rd, plain(rm)),
        0x7 => shift_by_reg(ShiftKind::Ror),
        0x8 => dp(DpOp::Tst, true, 0, rd, plain(rm)),
        0x9 => dp(DpOp::Rsb, true, rd, rm, imm(0)),
        0xA => dp(DpOp::Cmp, true, 0, rd, plain(rm)),
        0xB => dp(DpOp::Cmn, true, 0, rd, plain(rm)),
        0xC => dp(DpOp::Orr, true, rd, rd, plain(rm)),
        0xD => Op::Mul {
            s: true,
            rd,
            rm,
            rs: rd,
            acc: None,
        },
        0xE => dp(DpOp::Bic, true, rd, rd, plain(rm)),
        _ => dp(DpOp::Mvn, true, rd, 0, plain(rm)),
    }
}

/// Format 5: hi-register ADD/CMP/MOV and BX/BLX.
fn decode_hi(w: u32) -> Option<Op> {
    let rd = (r3(w, 0) | ((bit(w, 7) as u8) << 3)) as Reg;
    let rm = bits(w, 6, 3) as Reg;
    Some(match bits(w, 9, 8) {
        0b00 => dp(DpOp::Add, false, rd, rd, plain(rm)),
        0b01 => dp(DpOp::Cmp, true, 0, rd, plain(rm)),
        0b10 => dp(DpOp::Mov, false, rd, 0, plain(rm)),
        _ => {
            if bits(w, 2, 0) != 0 {
                return None;
            }
            Op::BranchReg { rm, link: bit(w, 7) }
        }
    })
}

fn decode_misc(w: u32) -> Option<Op> {
    let (rd, rm) = (r3(w, 0), r3(w, 3));
    Some(match bits(w, 11, 8) {
        0b0000 => {
            let op = if bit(w, 7) { DpOp::Sub } else { DpOp::Add };
            dp(op, false, SP, SP, imm(bits(w, 6, 0) * 4))
        }
        0b0010 => {
            let (signed, size) = [
                (true, ExtendSize::Half),
                (true, ExtendSize::Byte),
                (false, ExtendSize::Half),
                (false, ExtendSize::Byte),
            ][bits(w, 7, 6) as usize];
            Op::Extend {
                signed,
                size,
                rd,
                rn: None,
                rm,
                rot: 0,
            }
        }
        0b0100 | 0b0101 => {
            let regs = (bits(w, 7, 0) | ((bit(w, 8) as u32) << LR)) as u16;
            if regs == 0 {
                return None;
            }
            Op::Multi {
                load: false,
                rn: SP,
                regs,
                mode: BlockMode::Db,
                writeback: true,
                psr: false,
            }
        }
        0b1100 | 0b1101 => {
            let regs = (bits(w, 7, 0) | ((bit(w, 8) as u32) << PC)) as u16;
            if regs == 0 {
                return None;
            }
            Op::Multi {
                load: true,
                rn: SP,
                regs,
                mode: BlockMode::Ia,
                writeback: true,
                psr: false,
            }
        }
        0b0110 if bits(w, 7, 4) == 0b0101 && bits(w, 2, 0) == 0 => Op::Setend { big: bit(w, 3) },
        0b0110 if bits(w, 7, 5) == 0b011 => Op::Cps,
        0b1010 => Op::Rev {
            kind: match bits(w, 7, 6) {
                0b00 => RevKind::Rev,
                0b01 => RevKind::Rev16,
                0b11 => RevKind::Revsh,
                _ => return None,
            },
            rd,
            rm,
        },
        0b1110 => Op::Bkpt(bits(w, 7, 0) as u16),
        // 0b1111 is the hint/IT space, which is ARMv6T2+
        _ => return None,
    })
}
