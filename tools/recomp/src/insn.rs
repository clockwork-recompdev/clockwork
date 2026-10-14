//! Decoded instruction form shared by the ARM and Thumb decoders.
//!
//! Thumb instructions are decoded into their ARM equivalents (e.g. Thumb `lsls r0, r1, #2`
//! becomes `movs r0, r1, lsl #2`), so analysis and code generation only handle one form.
//! PC-relative immediate addressing is resolved to absolute addresses at decode time.

pub type Reg = u8;
pub const SP: Reg = 13;
pub const LR: Reg = 14;
pub const PC: Reg = 15;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Cond {
    Eq,
    Ne,
    Cs,
    Cc,
    Mi,
    Pl,
    Vs,
    Vc,
    Hi,
    Ls,
    Ge,
    Lt,
    Gt,
    Le,
    Al,
}

impl Cond {
    /// `None` for 0b1111, which isn't a condition (ARM unconditional space / Thumb SVC).
    pub fn from_bits(bits: u32) -> Option<Cond> {
        use Cond::*;
        Some(match bits & 0xF {
            0 => Eq,
            1 => Ne,
            2 => Cs,
            3 => Cc,
            4 => Mi,
            5 => Pl,
            6 => Vs,
            7 => Vc,
            8 => Hi,
            9 => Ls,
            10 => Ge,
            11 => Lt,
            12 => Gt,
            13 => Le,
            14 => Al,
            _ => return None,
        })
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum ShiftKind {
    Lsl,
    Lsr,
    Asr,
    Ror,
    Rrx,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum ShiftAmount {
    /// Normalized: LSR/ASR #0 in the encoding are stored as 32, ROR #0 becomes `Rrx`.
    Imm(u8),
    Reg(Reg),
}

/// A register, optionally shifted. `Lsl` by `Imm(0)` means unshifted.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Shifted {
    pub rm: Reg,
    pub kind: ShiftKind,
    pub amount: ShiftAmount,
}

impl Shifted {
    pub fn plain(rm: Reg) -> Self {
        Shifted {
            rm,
            kind: ShiftKind::Lsl,
            amount: ShiftAmount::Imm(0),
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Operand2 {
    /// `rotated`: the encoding had a non-zero rotation, so flag-setting logical ops
    /// take carry from bit 31 of `value` instead of leaving it unchanged.
    Imm {
        value: u32,
        rotated: bool,
    },
    Reg(Shifted),
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum DpOp {
    And,
    Eor,
    Sub,
    Rsb,
    Add,
    Adc,
    Sbc,
    Rsc,
    Tst,
    Teq,
    Cmp,
    Cmn,
    Orr,
    Mov,
    Bic,
    Mvn,
}

impl DpOp {
    pub fn from_bits(bits: u32) -> DpOp {
        use DpOp::*;
        [
            And, Eor, Sub, Rsb, Add, Adc, Sbc, Rsc, Tst, Teq, Cmp, Cmn, Orr, Mov, Bic, Mvn,
        ][(bits & 0xF) as usize]
    }
    /// Compare/test ops write no register.
    pub fn is_test(self) -> bool {
        matches!(self, DpOp::Tst | DpOp::Teq | DpOp::Cmp | DpOp::Cmn)
    }
    /// Mov/Mvn ignore rn.
    pub fn is_move(self) -> bool {
        matches!(self, DpOp::Mov | DpOp::Mvn)
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum MemSize {
    Word,
    Byte,
    Half,
    SignedByte,
    SignedHalf,
    Dual,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum MemOffset {
    Imm(u32),
    Reg(Shifted),
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum MemAddr {
    /// `[rn, ±offset]` (pre-indexed), `[rn, ±offset]!` (writeback), or `[rn], ±offset` (post).
    Base {
        rn: Reg,
        offset: MemOffset,
        add: bool,
        pre_index: bool,
        writeback: bool,
    },
    /// A PC-relative immediate address, already resolved.
    Abs(u32),
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum BlockMode {
    Ia,
    Ib,
    Da,
    Db,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum MulLongKind {
    Umull,
    Umlal,
    Smull,
    Smlal,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum SatArith {
    Qadd,
    Qsub,
    Qdadd,
    Qdsub,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum ParallelPrefix {
    S,
    Q,
    Sh,
    U,
    Uq,
    Uh,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum ParallelOp {
    Add16,
    Asx,
    Sax,
    Sub16,
    Add8,
    Sub8,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum ExtendSize {
    Byte,
    Half,
    Byte16,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum RevKind {
    Rev,
    Rev16,
    Revsh,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum DualMulKind {
    Smlad,
    Smlsd,
    Smlald,
    Smlsld,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Hint {
    Nop,
    Yield,
    Wfe,
    Wfi,
    Sev,
}

/// VFP register. Singles are s0-s31, doubles d0-d15 (VFPv2).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum FReg {
    S(u8),
    D(u8),
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum VfpBinary {
    /// d = d + n*m
    Mla,
    /// d = d - n*m
    Mls,
    /// d = n*m - d
    Nmls,
    /// d = -d - n*m
    Nmla,
    Mul,
    /// d = -(n*m)
    Nmul,
    Add,
    Sub,
    Div,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum VfpUnary {
    Mov,
    Abs,
    Neg,
    Sqrt,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum NumType {
    F32,
    F64,
    S32,
    U32,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Op {
    DataProc {
        op: DpOp,
        s: bool,
        rd: Reg,
        rn: Reg,
        op2: Operand2,
    },
    /// rd = rm * rs (+ acc)
    Mul {
        s: bool,
        rd: Reg,
        rm: Reg,
        rs: Reg,
        acc: Option<Reg>,
    },
    MulLong {
        kind: MulLongKind,
        s: bool,
        rdlo: Reg,
        rdhi: Reg,
        rm: Reg,
        rs: Reg,
    },
    Umaal {
        rdlo: Reg,
        rdhi: Reg,
        rm: Reg,
        rs: Reg,
    },
    /// SMULxy / SMLAxy: 16x16 from the chosen halves of rm (`m_top`) and rs (`s_top`).
    SmulHalf {
        rd: Reg,
        rm: Reg,
        rs: Reg,
        m_top: bool,
        s_top: bool,
        acc: Option<Reg>,
    },
    /// SMULWy / SMLAWy: (rm * half of rs) >> 16.
    SmulWord {
        rd: Reg,
        rm: Reg,
        rs: Reg,
        s_top: bool,
        acc: Option<Reg>,
    },
    SmlalHalf {
        rdlo: Reg,
        rdhi: Reg,
        rm: Reg,
        rs: Reg,
        m_top: bool,
        s_top: bool,
    },
    /// SMUAD/SMUSD when `acc` is None.
    DualMul {
        kind: DualMulKind,
        swap: bool,
        rd: Reg,
        rn: Reg,
        rm: Reg,
        acc: Option<Reg>,
    },
    DualMulLong {
        kind: DualMulKind,
        swap: bool,
        rdlo: Reg,
        rdhi: Reg,
        rn: Reg,
        rm: Reg,
    },
    /// SMMUL / SMMLA / SMMLS: top 32 bits of rn*rm (±acc), optionally rounded.
    MulHigh {
        sub: bool,
        round: bool,
        rd: Reg,
        rn: Reg,
        rm: Reg,
        acc: Option<Reg>,
    },
    SatArith {
        op: SatArith,
        rd: Reg,
        rm: Reg,
        rn: Reg,
    },
    Clz {
        rd: Reg,
        rm: Reg,
    },
    /// SSAT/USAT: saturate (rn shifted) to `bits` bits.
    Sat {
        signed: bool,
        rd: Reg,
        bits: u8,
        rn: Shifted,
    },
    Sat16 {
        signed: bool,
        rd: Reg,
        bits: u8,
        rn: Reg,
    },
    /// SXTB/UXTH/SXTAB16/...: extend (rm ror rot), plus rn if present.
    Extend {
        signed: bool,
        size: ExtendSize,
        rd: Reg,
        rn: Option<Reg>,
        rm: Reg,
        rot: u8,
    },
    Rev {
        kind: RevKind,
        rd: Reg,
        rm: Reg,
    },
    /// PKHBT (tb=false): rn[15:0] | (rm lsl sh)[31:16]; PKHTB: rn[31:16] | (rm asr sh)[15:0].
    Pkh {
        tb: bool,
        rd: Reg,
        rn: Reg,
        rm: Reg,
        shift: u8,
    },
    Sel {
        rd: Reg,
        rn: Reg,
        rm: Reg,
    },
    Parallel {
        prefix: ParallelPrefix,
        op: ParallelOp,
        rd: Reg,
        rn: Reg,
        rm: Reg,
    },
    Usad8 {
        rd: Reg,
        rn: Reg,
        rm: Reg,
        acc: Option<Reg>,
    },

    /// Single load/store. For `Dual`, the second register is rt+1. `user`: LDRT/STRT.
    Mem {
        load: bool,
        size: MemSize,
        rt: Reg,
        addr: MemAddr,
        user: bool,
    },
    /// LDM/STM. `psr`: the ^ form (user-bank registers / SPSR restore); not used by user code.
    Multi {
        load: bool,
        rn: Reg,
        regs: u16,
        mode: BlockMode,
        writeback: bool,
        psr: bool,
    },
    Swp {
        byte: bool,
        rt: Reg,
        rt2: Reg,
        rn: Reg,
    },
    /// LDREX family. `Dual` loads rt, rt+1.
    Ldrex {
        size: MemSize,
        rt: Reg,
        rn: Reg,
    },
    /// STREX family: rd = status (0 on success).
    Strex {
        size: MemSize,
        rd: Reg,
        rt: Reg,
        rn: Reg,
    },
    Clrex,
    Pld,

    /// Direct branch. `exchange`: BLX immediate, switches ARM<->Thumb.
    Branch {
        target: u32,
        link: bool,
        exchange: bool,
    },
    /// BX / BLX register: mode from bit 0 of rm.
    BranchReg {
        rm: Reg,
        link: bool,
    },
    Svc(u32),
    Bkpt(u16),
    Udf,
    Hint(Hint),

    Mrs {
        rd: Reg,
        spsr: bool,
    },
    /// MSR. `mask` is the field mask (bit 3 = flags f, bit 0 = control c).
    Msr {
        spsr: bool,
        mask: u8,
        src: Operand2,
    },
    Cps,
    Setend {
        big: bool,
    },
    /// MRC (load=true, coproc -> rt) / MCR. 3DS user code reads the TLS pointer via
    /// `mrc p15, 0, rt, c13, c0, 3`.
    CoprocReg {
        load: bool,
        coproc: u8,
        opc1: u8,
        rt: Reg,
        crn: u8,
        crm: u8,
        opc2: u8,
    },

    VfpBinary {
        op: VfpBinary,
        d: FReg,
        n: FReg,
        m: FReg,
    },
    VfpUnary {
        op: VfpUnary,
        d: FReg,
        m: FReg,
    },
    /// VCMP / VCMPE (`quiet`=false raises on quiet NaN). `m` None: compare with +0.
    VfpCmp {
        quiet: bool,
        d: FReg,
        m: Option<FReg>,
    },
    /// `round_zero`: float->int conversions that truncate (FTOSIZ) instead of using FPSCR mode.
    VfpCvt {
        from: NumType,
        to: NumType,
        d: FReg,
        m: FReg,
        round_zero: bool,
    },
    /// Core register <-> single. `to_core`: VMOV rt, sN.
    VmovSingle {
        to_core: bool,
        rt: Reg,
        sn: u8,
    },
    /// Two core registers <-> two consecutive singles (sm, sm+1) or one double.
    VmovPair {
        to_core: bool,
        rt: Reg,
        rt2: Reg,
        m: FReg,
    },
    /// Core register <-> half of a double (`high`: dN[1]).
    VmovHalf {
        to_core: bool,
        rt: Reg,
        dn: u8,
        high: bool,
    },
    /// VMRS (to_core) / VMSR. `reg`: 0 FPSID, 1 FPSCR, 8 FPEXC. VMRS with rt=15 copies FPSCR flags to APSR.
    Vsys {
        to_core: bool,
        rt: Reg,
        reg: u8,
    },
    VMem {
        load: bool,
        d: FReg,
        addr: MemAddr,
    },
    /// VLDM/VSTM (incl. VPUSH/VPOP): `count` registers starting at `first`.
    VMulti {
        load: bool,
        first: FReg,
        count: u8,
        rn: Reg,
        mode: BlockMode,
        writeback: bool,
    },
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Insn {
    pub addr: u32,
    pub raw: u32,
    /// 2 or 4 bytes. Thumb BL/BLX pairs are one 4-byte instruction.
    pub size: u8,
    pub thumb: bool,
    pub cond: Cond,
    pub op: Op,
}

impl Insn {
    /// What reading PC as a register yields: address + 8 in ARM, + 4 in Thumb.
    pub fn pc_value(&self) -> u32 {
        self.addr.wrapping_add(if self.thumb { 4 } else { 8 })
    }
}

impl Insn {
    /// Bitmask of core registers this instruction may write. Calls clobber the
    /// caller-saved registers (r0-r3, r12) and LR per the ARM procedure call standard.
    pub fn writes(&self) -> u16 {
        let r = |reg: Reg| 1u16 << reg;
        let writeback = |addr: &MemAddr| match addr {
            MemAddr::Base {
                rn, writeback: true, ..
            } => r(*rn),
            _ => 0,
        };
        match &self.op {
            Op::DataProc { op, rd, .. } => {
                if op.is_test() {
                    0
                } else {
                    r(*rd)
                }
            }
            Op::Mul { rd, .. }
            | Op::SmulHalf { rd, .. }
            | Op::SmulWord { rd, .. }
            | Op::DualMul { rd, .. }
            | Op::MulHigh { rd, .. }
            | Op::SatArith { rd, .. }
            | Op::Clz { rd, .. }
            | Op::Sat { rd, .. }
            | Op::Sat16 { rd, .. }
            | Op::Extend { rd, .. }
            | Op::Rev { rd, .. }
            | Op::Pkh { rd, .. }
            | Op::Sel { rd, .. }
            | Op::Parallel { rd, .. }
            | Op::Usad8 { rd, .. }
            | Op::Mrs { rd, .. }
            | Op::Strex { rd, .. } => r(*rd),
            Op::MulLong { rdlo, rdhi, .. }
            | Op::Umaal { rdlo, rdhi, .. }
            | Op::SmlalHalf { rdlo, rdhi, .. }
            | Op::DualMulLong { rdlo, rdhi, .. } => r(*rdlo) | r(*rdhi),
            Op::Mem {
                load, size, rt, addr, ..
            } => {
                let loaded = match (load, size) {
                    (false, _) => 0,
                    (true, MemSize::Dual) => r(*rt) | r(rt + 1),
                    (true, _) => r(*rt),
                };
                loaded | writeback(addr)
            }
            Op::Multi {
                load,
                rn,
                regs,
                writeback,
                ..
            } => (if *load { *regs } else { 0 }) | if *writeback { r(*rn) } else { 0 },
            Op::Swp { rt, .. } => r(*rt),
            Op::Ldrex { size, rt, .. } => {
                if *size == MemSize::Dual {
                    r(*rt) | r(rt + 1)
                } else {
                    r(*rt)
                }
            }
            Op::Branch { link: true, .. } | Op::BranchReg { link: true, .. } => 0b1111 | r(12) | r(LR),
            Op::CoprocReg { load: true, rt, .. } => r(*rt),
            Op::VmovSingle { to_core: true, rt, .. } | Op::VmovHalf { to_core: true, rt, .. } => r(*rt),
            Op::VmovPair {
                to_core: true, rt, rt2, ..
            } => r(*rt) | r(*rt2),
            Op::Vsys { to_core: true, rt, .. } if *rt != PC => r(*rt),
            Op::VMem { addr, .. } => writeback(addr),
            Op::VMulti {
                rn, writeback: true, ..
            } => r(*rn),
            _ => 0,
        }
    }
}
