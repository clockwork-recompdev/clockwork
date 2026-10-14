//! Function discovery by recursive descent.
//!
//! Starting from the entry point, decodes each function's reachable instructions, queues
//! direct call targets as new functions, and records everything the recompiler can't
//! handle yet (undecodable instructions, unresolved indirect jumps, ARM/Thumb conflicts).

use crate::image::Image;
use crate::insn::*;
use crate::{arm, thumb};
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};

pub struct Function {
    pub entry: u32,
    pub thumb: bool,
    pub insns: BTreeMap<u32, Insn>,
    /// Indirect jumps resolved as jump tables: (jump address, targets).
    pub jump_tables: Vec<(u32, Vec<u32>)>,
    /// Reaches a return, or an exit we can't follow (so must be assumed to return).
    pub exits: bool,
    /// Functions this one tail-calls; it returns if any of them does.
    pub tail_calls: Vec<u32>,
    /// `bx rN` / `blx rN` whose register value discovery worked out.
    pub resolved: BTreeMap<u32, Resolved>,
    /// Computed jumps made with LR set to a constant first (`add lr, pc, #n; add pc, ...`):
    /// calls that resume at that address. Site -> resume address.
    pub manual_calls: BTreeMap<u32, u32>,
}

#[derive(Clone, Copy, Debug)]
pub enum Resolved {
    /// The register holds this constant (bit 0 set for Thumb).
    Target(u32),
    /// The register holds a return address popped from the stack.
    Return,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum IssueKind {
    /// Reached an encoding the decoder rejects (data, or a noreturn call we fell through).
    Undecodable,
    /// Non-return write to PC that isn't a recognized jump table.
    IndirectJump,
    /// A branch target outside .text.
    TargetOutsideText,
    /// The same address was reached as both ARM and Thumb.
    ModeConflict,
}

pub struct Issue {
    pub kind: IssueKind,
    pub addr: u32,
    pub function: u32,
}

/// A pointer-seeded candidate dropped because exploring it hit a problem.
pub struct Rejection {
    pub entry: u32,
    pub thumb: bool,
    pub kind: IssueKind,
    pub at: u32,
}

pub struct Analysis {
    pub functions: BTreeMap<u32, Function>,
    pub issues: Vec<Issue>,
    pub rejections: Vec<Rejection>,
    /// Words in .text loaded as data (literal pools, jump table entries).
    pub data_words: BTreeSet<u32>,
    /// Addresses in .text computed PC-relatively (`add rX, pc, #imm`), possibly code.
    pub pc_relative: BTreeSet<u32>,
    /// Call targets that are `pops_caller` helpers.
    pub pops_caller: BTreeSet<u32>,
}

/// ARM compiler runtime helpers (the RVCT floating-point exception handler) that take an
/// inline argument word after the `bl` (`ldr rX, [lr], #4`), pop the *caller's* saved
/// registers, and then either jump into small stubs just past that word (`bx r12` to
/// lr + 4k) or return straight to the caller's caller. To the caller, such a call never
/// returns: its path ends at the `bl`, and the stubs are separate frameless functions.
pub fn pops_caller(image: &Image, target: u32, thumb: bool) -> bool {
    !thumb
        && image.u32(target).and_then(|w| arm::decode(target, w)).is_some_and(|i| {
            matches!(
                i.op,
                Op::Mem {
                    load: true,
                    size: MemSize::Word,
                    addr: MemAddr::Base { rn: LR, offset: MemOffset::Imm(4), add: true, pre_index: false, .. },
                    ..
                }
            )
        })
}

/// Stubs a `pops_caller` helper may return into after the call at `site`.
const POPS_CALLER_STUBS: u32 = 4;

/// Rejection sets are keyed by address and mode: the same bytes can fail as ARM yet be a
/// real Thumb function.
fn key(addr: u32, thumb: bool) -> u32 {
    addr | thumb as u32
}

/// Thumb `push {.., lr}` (0xB5xx).
fn is_thumb_push_lr(hw: Option<u16>) -> bool {
    hw.is_some_and(|h| h & 0xFF00 == 0xB500)
}

fn is_push_lr(insn: Option<Insn>) -> bool {
    insn.is_some_and(|i| {
        i.cond == Cond::Al
            && matches!(i.op, Op::Multi { load: false, rn: SP, regs, writeback: true, .. } if regs & (1 << LR) != 0)
    })
}

pub(crate) enum Value {
    Const(u32),
    /// Loaded from the stack by a pop: in practice a saved return address.
    Popped,
}

/// How an instruction affects control flow.
pub(crate) enum Flow {
    Next,
    /// Conditional or unconditional jump inside the function (`also_next`: conditional).
    Jump {
        target: u32,
        also_next: bool,
    },
    Call {
        target: u32,
        thumb: bool,
    },
    IndirectCall,
    Return {
        also_next: bool,
    },
    IndirectJump {
        also_next: bool,
    },
    Stop,
}

fn writes_pc(op: &Op) -> bool {
    match op {
        Op::DataProc { op, rd, .. } => *rd == PC && !op.is_test(),
        Op::Mem { load: true, rt, .. } => *rt == PC,
        Op::Multi { load: true, regs, .. } => regs & (1 << PC) != 0,
        _ => false,
    }
}

pub(crate) fn flow(insn: &Insn) -> Flow {
    let conditional = insn.cond != Cond::Al;
    match insn.op {
        Op::Branch {
            target,
            link: true,
            exchange,
        } => Flow::Call {
            target,
            thumb: insn.thumb ^ exchange,
        },
        Op::Branch {
            target, link: false, ..
        } => Flow::Jump {
            target,
            also_next: conditional,
        },
        Op::BranchReg { link: true, .. } => Flow::IndirectCall,
        Op::BranchReg { rm: LR, link: false } => Flow::Return { also_next: conditional },
        Op::BranchReg { .. } => Flow::IndirectJump { also_next: conditional },
        Op::Udf | Op::Bkpt(_) => Flow::Stop,
        // svcExitProcess and svcBreak don't come back
        Op::Svc(0x03 | 0x3C) if !conditional => Flow::Stop,
        ref op if writes_pc(op) => {
            let is_return = match op {
                Op::DataProc {
                    op: DpOp::Mov,
                    op2: Operand2::Reg(s),
                    ..
                } => s.rm == LR && s.amount == ShiftAmount::Imm(0) && s.kind == ShiftKind::Lsl,
                Op::Multi { rn, .. } => *rn == SP,
                Op::Mem {
                    addr:
                        MemAddr::Base {
                            rn: SP,
                            add: true,
                            pre_index: false,
                            ..
                        },
                    ..
                } => true,
                _ => false,
            };
            if is_return {
                Flow::Return { also_next: conditional }
            } else {
                Flow::IndirectJump { also_next: conditional }
            }
        }
        _ => Flow::Next,
    }
}

pub struct Discoverer<'a> {
    image: &'a Image,
    analysis: Analysis,
    /// (entry, thumb) reached from the entry point by direct calls; always explored first.
    trusted: Vec<(u32, bool)>,
    /// (entry, thumb) from pointer seeding or gap filling, kept only if they explore cleanly.
    untrusted: Vec<(u32, bool)>,
    /// Untrusted entries that are direct call targets of accepted code (vs. speculative seeds).
    called: BTreeSet<u32>,
    /// Every decoded address and its mode, across all accepted functions.
    decoded_mode: HashMap<u32, bool>,
    /// Untrusted entries explored and dropped because they didn't decode cleanly.
    rejected: BTreeSet<u32>,
    rejected_thumb: usize,
    /// Seed candidates that failed the cheap plausibility check. Unlike `rejected`, a real
    /// call to one of these (e.g. a resolved `blx rN`) still gets it explored.
    implausible: BTreeSet<u32>,
    /// Functions known not to return: code after calls to them isn't followed.
    noreturn: &'a HashSet<u32>,
    trusted_count: usize,
}

/// The result of exploring one function, committed only if the function is accepted.
struct Exploration {
    f: Function,
    callees: Vec<(u32, bool)>,
    /// possible stubs after calls to `pops_caller` helpers; always explored as untrusted
    stubs: Vec<(u32, bool)>,
    pops_caller: Vec<u32>,
    issues: Vec<Issue>,
    data_words: Vec<u32>,
    pc_relative: Vec<u32>,
}

impl<'a> Discoverer<'a> {
    pub fn new(image: &'a Image, noreturn: &'a HashSet<u32>) -> Self {
        Discoverer {
            image,
            analysis: Analysis {
                functions: BTreeMap::new(),
                issues: Vec::new(),
                rejections: Vec::new(),
                data_words: BTreeSet::new(),
                pc_relative: BTreeSet::new(),
                pops_caller: BTreeSet::new(),
            },
            trusted: Vec::new(),
            untrusted: Vec::new(),
            called: BTreeSet::new(),
            decoded_mode: HashMap::new(),
            rejected: BTreeSet::new(),
            rejected_thumb: 0,
            implausible: BTreeSet::new(),
            noreturn,
            trusted_count: 0,
        }
    }

    pub fn add_entry(&mut self, addr: u32, thumb: bool) {
        self.trusted.push((addr, thumb));
    }

    /// Functions reached from trusted entries (the entry point and its direct calls).
    pub fn trusted_count(&self) -> usize {
        self.trusted_count
    }

    /// (ARM, Thumb) candidate entries rejected so far, by either check.
    pub fn rejected_counts(&self) -> (usize, usize) {
        let thumb = self.rejected_thumb;
        (self.rejected.len() + self.implausible.len() - thumb, thumb)
    }

    pub fn run(&mut self) {
        loop {
            let (entry, thumb, trusted) = match self.trusted.pop() {
                Some((a, t)) => (a, t, true),
                None => match self.untrusted.pop() {
                    Some((a, t)) => (a, t, false),
                    None => break,
                },
            };
            if self.analysis.functions.contains_key(&entry) || self.rejected.contains(&key(entry, thumb)) {
                continue;
            }
            // A candidate on an instruction boundary of code already decoded in the same mode is
            // a likely entry point (e.g. a virtual function a tail jump got absorbed into), so it
            // becomes its own function; shared code is just emitted twice. Code decoded in the
            // other mode can't be a function start.
            if !trusted && !self.called.contains(&key(entry, thumb)) && self.decoded_mode.get(&entry) == Some(&!thumb) {
                continue;
            }
            let e = self.explore(entry, thumb);
            let clean = e.issues.iter().all(|i| i.kind == IssueKind::IndirectJump);
            if !trusted && !clean {
                if let Some(first) = e.issues.iter().find(|i| i.kind != IssueKind::IndirectJump) {
                    self.analysis.rejections.push(Rejection {
                        entry,
                        thumb,
                        kind: first.kind,
                        at: first.addr,
                    });
                }
                self.reject(entry, thumb);
                continue;
            }
            for &addr in e.f.insns.keys() {
                self.decoded_mode.insert(addr, thumb);
            }
            if !trusted {
                self.called.extend(e.callees.iter().map(|&(a, t)| key(a, t)));
            }
            if trusted {
                &mut self.trusted
            } else {
                &mut self.untrusted
            }
            .extend(e.callees);
            self.untrusted.extend(e.stubs);
            self.analysis.pops_caller.extend(e.pops_caller);
            self.analysis.issues.extend(e.issues);
            self.analysis.data_words.extend(e.data_words);
            self.analysis.pc_relative.extend(e.pc_relative);
            let end = e.f.insns.values().next_back().map(|i| i.addr + i.size as u32);
            self.analysis.functions.insert(entry, e.f);
            self.trusted_count += trusted as usize;
            if let Some(end) = end {
                self.queue_following(end, thumb);
            }
        }
    }

    /// Gap filling, chained: after a function ends, try the next unclaimed address (past any
    /// literal pool) as a provisional function. Thumb ones must start with `push {.., lr}`:
    /// ARM code and data read as Thumb decode too easily for the general check alone.
    fn queue_following(&mut self, end: u32, thumb: bool) {
        let mut next = if thumb { end } else { end.next_multiple_of(4) };
        if thumb && self.analysis.data_words.contains(&(next.next_multiple_of(4))) {
            next = next.next_multiple_of(4);
        }
        while self.analysis.data_words.contains(&next) {
            next += 4;
        }
        if thumb && !is_thumb_push_lr(self.image.u16(next)) {
            return;
        }
        if self.image.in_text(next)
            && !self.decoded_mode.contains_key(&next)
            && !self.rejected.contains(&key(next, thumb))
            && !self.implausible.contains(&key(next, thumb))
            && !self.analysis.functions.contains_key(&next)
            && self.plausible_entry(next, thumb, false)
        {
            self.untrusted.push((next, thumb));
        }
    }

    pub fn finish(mut self) -> Analysis {
        self.analysis.issues.sort_by_key(|i| (i.kind, i.addr));
        self.analysis.issues.dedup_by_key(|i| (i.kind, i.addr));
        self.analysis
    }

    fn reject(&mut self, addr: u32, thumb: bool) {
        if self.rejected.insert(key(addr, thumb)) && thumb {
            self.rejected_thumb += 1;
        }
    }

    fn decode(&self, addr: u32, thumb: bool) -> Option<Insn> {
        if thumb {
            thumb::decode(addr, self.image.u16(addr)?, self.image.u16(addr + 2))
        } else {
            arm::decode(addr, self.image.u32(addr)?)
        }
    }

    fn explore(&self, entry: u32, thumb: bool) -> Exploration {
        let mut e = Exploration {
            f: Function {
                entry,
                thumb,
                insns: BTreeMap::new(),
                jump_tables: Vec::new(),
                exits: false,
                tail_calls: Vec::new(),
                resolved: BTreeMap::new(),
                manual_calls: BTreeMap::new(),
            },
            callees: Vec::new(),
            stubs: Vec::new(),
            pops_caller: Vec::new(),
            issues: Vec::new(),
            data_words: Vec::new(),
            pc_relative: Vec::new(),
        };
        let issue = |e: &mut Exploration, kind, addr| {
            e.issues.push(Issue {
                kind,
                addr,
                function: entry,
            })
        };
        let mut pending = vec![entry];
        let mut indirect: Vec<u32> = Vec::new();

        loop {
            while let Some(addr) = pending.pop() {
                if e.f.insns.contains_key(&addr) {
                    continue;
                }
                if !self.image.in_text(addr) {
                    issue(&mut e, IssueKind::TargetOutsideText, addr);
                    continue;
                }
                // stop at another function's entry: we fell or jumped into it, i.e. a tail call
                if addr != entry && self.analysis.functions.contains_key(&addr) {
                    e.f.tail_calls.push(addr);
                    continue;
                }
                if self.decoded_mode.get(&addr).is_some_and(|&mode| mode != thumb) {
                    issue(&mut e, IssueKind::ModeConflict, addr);
                    continue;
                }
                let Some(insn) = self.decode(addr, thumb) else {
                    issue(&mut e, IssueKind::Undecodable, addr);
                    continue;
                };
                e.f.insns.insert(addr, insn);
                self.literal_words(&insn, &mut e.data_words);
                if let Some(v) = self.pc_relative_value(&insn) {
                    e.pc_relative.push(v);
                }
                let next = addr + insn.size as u32;
                let call = |e: &mut Exploration, target: u32, thumb: bool| {
                    if self.image.in_text(target) {
                        e.callees.push((target, thumb));
                    } else {
                        issue(e, IssueKind::TargetOutsideText, target);
                    }
                };

                match flow(&insn) {
                    Flow::Next => pending.push(next),
                    Flow::IndirectCall => {
                        indirect.push(addr);
                        pending.push(next);
                    }
                    Flow::Call {
                        target,
                        thumb: callee_thumb,
                    } => {
                        call(&mut e, target, callee_thumb);
                        let pops = pops_caller(self.image, target, callee_thumb);
                        if pops {
                            e.pops_caller.push(target);
                            for k in 0..POPS_CALLER_STUBS {
                                e.stubs.push((next + 4 + 4 * k, thumb));
                            }
                        }
                        // a conditional call to a noreturn function (`blne abort`) may be skipped
                        if insn.cond != Cond::Al || !(pops || self.noreturn.contains(&target)) {
                            pending.push(next);
                        }
                    }
                    Flow::Jump { target, also_next } => {
                        if also_next {
                            pending.push(next);
                        }
                        // hand-written assembly: `add lr, pc, #n; b target` is a call that
                        // resumes just after the branch
                        if let Some(Value::Const(r)) = self.resolve(&e.f, addr, LR, 0)
                            && (r & 1 != 0) == thumb
                            && (r & !1) > addr
                            && (r & !1) - addr <= 64
                        {
                            let ret = r & !1;
                            call(&mut e, target, thumb);
                            e.f.manual_calls.insert(addr, ret);
                            pending.push(ret);
                            continue;
                        }
                        if self.analysis.functions.contains_key(&target) && target != entry {
                            e.f.tail_calls.push(target);
                            continue;
                        }
                        pending.push(target);
                    }
                    Flow::Return { also_next } => {
                        e.f.exits = true;
                        if also_next {
                            pending.push(next);
                        }
                    }
                    Flow::IndirectJump { also_next } => {
                        indirect.push(addr);
                        if also_next {
                            pending.push(next);
                        }
                    }
                    Flow::Stop => {}
                }
            }

            // Resolve register calls/jumps now that the function's code is known: during the
            // depth-first walk, the instructions that set a register may not have been decoded
            // yet. A jump table found here adds code, so walk again until nothing changes.
            indirect.sort_unstable();
            indirect.dedup();
            let mut unresolved = Vec::new();
            for site in std::mem::take(&mut indirect) {
                let insn = e.f.insns[&site];
                let resolved = match insn.op {
                    Op::BranchReg { rm, .. } => self.resolve(&e.f, site, rm, 0),
                    // `mov pc, rN`
                    Op::DataProc { op: DpOp::Mov, rd: PC, op2: Operand2::Reg(sh), .. }
                        if sh.kind == ShiftKind::Lsl && sh.amount == ShiftAmount::Imm(0) =>
                    {
                        self.resolve(&e.f, site, sh.rm, 0)
                    }
                    // long-branch veneer: `ldr pc, =target`
                    Op::Mem {
                        load: true,
                        size: MemSize::Word,
                        addr: MemAddr::Abs(lit),
                        ..
                    } => self.image.u32(lit).map(Value::Const),
                    _ => None,
                };
                let is_call = matches!(flow(&insn), Flow::IndirectCall);
                match resolved {
                    Some(Value::Const(t)) if self.image.in_text(t & !1) => {
                        e.callees.push((t & !1, t & 1 != 0));
                        e.f.resolved.insert(site, Resolved::Target(t));
                        if !is_call {
                            e.f.tail_calls.push(t & !1);
                        }
                    }
                    Some(Value::Const(t)) if !is_call => {
                        issue(&mut e, IssueKind::TargetOutsideText, t);
                    }
                    Some(Value::Popped) if !is_call => {
                        e.f.exits = true;
                        e.f.resolved.insert(site, Resolved::Return);
                    }
                    _ if is_call => {}
                    _ => match self.jump_table(&e.f, &insn, &mut e.data_words) {
                        Some(targets) => {
                            pending.extend(targets.iter().copied());
                            e.f.jump_tables.push((site, targets));
                        }
                        None => {
                            // hand-written assembly: targets from a table of offsets relative to a
                            // PC-derived base, and calls with a hand-set return address
                            let table = self.relative_table_targets(&e.f, &insn);
                            if let Some(targets) = &table {
                                e.callees.extend(targets.iter().map(|&t| (t, insn.thumb)));
                                e.f.tail_calls.extend(targets.iter().copied());
                            }
                            let ret = match self.resolve(&e.f, site, LR, 0) {
                                Some(Value::Const(r)) if self.image.in_text(r & !1) && (r & 1 != 0) == insn.thumb => {
                                    Some(r & !1)
                                }
                                _ => None,
                            };
                            if let Some(r) = ret {
                                e.f.manual_calls.insert(site, r);
                                pending.push(r);
                            }
                            if table.is_none() && ret.is_none() {
                                unresolved.push(site);
                            }
                        }
                    },
                }
            }
            if pending.is_empty() {
                for site in unresolved {
                    issue(&mut e, IssueKind::IndirectJump, site);
                    e.f.exits = true;
                }
                break;
            }
            // new code from jump tables may also make these resolvable
            indirect = unresolved;
        }

        // addresses built from PC in registers (`ldr r1, =offset; add r1, pc`): possible
        // function pointers stored for later, e.g. callbacks
        for insn in e.f.insns.values() {
            let Op::DataProc { op: DpOp::Add, rd, rn, op2: Operand2::Reg(sh), .. } = insn.op else { continue };
            if rd == PC || (rn != PC && sh.rm != PC) {
                continue;
            }
            let after = insn.addr + insn.size as u32;
            if let Some(Value::Const(v)) = self.resolve(&e.f, after, rd, 0)
                && self.image.in_text(v & !1)
            {
                e.pc_relative.push(v);
            }
        }
        e
    }

    /// What `reg` holds just before `at`, found by walking back through straight-line code.
    fn resolve(&self, f: &Function, at: u32, reg: Reg, depth: u32) -> Option<Value> {
        if depth > 8 {
            return None;
        }
        let mut expect = at;
        for (&addr, insn) in f.insns.range(..at).rev().take(24) {
            if addr + insn.size as u32 != expect {
                return None; // gap: not straight-line code
            }
            // an unconditional branch/return before us means we're only reached by a jump
            let falls_through = match flow(insn) {
                Flow::Jump { also_next, .. } | Flow::Return { also_next } | Flow::IndirectJump { also_next } => {
                    also_next
                }
                Flow::Stop => false,
                _ => true,
            };
            if !falls_through {
                return None;
            }
            expect = addr;
            if insn.writes() & (1 << reg) == 0 {
                continue;
            }
            if insn.cond != Cond::Al {
                return None;
            }
            let read = |r: Reg| -> Option<u32> {
                if r == PC {
                    Some(insn.pc_value())
                } else {
                    match self.resolve(f, addr, r, depth + 1)? {
                        Value::Const(v) => Some(v),
                        Value::Popped => None,
                    }
                }
            };
            let plain = |op2: &Operand2| match op2 {
                Operand2::Reg(s) if s.kind == ShiftKind::Lsl && s.amount == ShiftAmount::Imm(0) => Some(s.rm),
                _ => None,
            };
            return match &insn.op {
                Op::DataProc {
                    op: DpOp::Mov,
                    op2: Operand2::Imm { value, .. },
                    ..
                } => Some(Value::Const(*value)),
                // a copy passes the value through, including "the return address"
                Op::DataProc { op: DpOp::Mov, op2, .. } => match plain(op2)? {
                    PC => Some(Value::Const(insn.pc_value())),
                    rm => self.resolve(f, addr, rm, depth + 1),
                },
                Op::DataProc {
                    op: op @ (DpOp::Add | DpOp::Sub),
                    rn,
                    op2,
                    ..
                } => {
                    let lhs = read(*rn)?;
                    let rhs = match op2 {
                        Operand2::Imm { value, .. } => *value,
                        _ => read(plain(op2)?)?,
                    };
                    Some(Value::Const(if *op == DpOp::Add {
                        lhs.wrapping_add(rhs)
                    } else {
                        lhs.wrapping_sub(rhs)
                    }))
                }
                Op::Mem {
                    load: true,
                    size: MemSize::Word,
                    rt,
                    addr: MemAddr::Abs(a),
                    ..
                } if *rt == reg => Some(Value::Const(self.image.u32(*a)?)),
                Op::Multi {
                    load: true,
                    rn: SP,
                    regs,
                    ..
                } if regs & (1 << reg) != 0 => Some(Value::Popped),
                Op::Mem {
                    load: true,
                    rt,
                    addr:
                        MemAddr::Base {
                            rn: SP,
                            add: true,
                            pre_index: false,
                            ..
                        },
                    ..
                } if *rt == reg => Some(Value::Popped),
                _ => None,
            };
        }
        // LR untouched since the function was entered: it's still the return address
        if reg == LR && expect == f.entry {
            return Some(Value::Popped);
        }
        None
    }

    /// Queues likely function pointers of one mode found in .rodata, .data, literal pools
    /// and unclaimed parts of .text. Returns how many new entries were queued.
    pub fn seed_pointers(&mut self, want_thumb: bool) -> usize {
        let img = self.image;
        let mut candidates: Vec<u32> = Vec::new();
        for seg in [&img.rodata, &img.data] {
            candidates.extend((seg.addr..seg.addr + seg.size).step_by(4).filter_map(|a| img.u32(a)));
        }
        candidates.extend(self.analysis.data_words.iter().filter_map(|&a| img.u32(a)));
        // data embedded in .text that no code has claimed (e.g. static-initializer tables)
        candidates.extend(
            (img.text.addr..img.text.addr + img.text.size)
                .step_by(4)
                .filter(|a| !self.decoded_mode.contains_key(a) && !self.decoded_mode.contains_key(&(a + 2)))
                .filter_map(|a| img.u32(a)),
        );
        candidates.extend(self.relative_pointer_runs());
        candidates.extend(self.analysis.pc_relative.iter().copied());
        if want_thumb {
            // uncovered Thumb `push {.., lr}` prologues that aren't inside known ARM code
            candidates.extend(
                (img.text.addr..img.text.addr + img.text.size)
                    .step_by(2)
                    .filter(|&a| is_thumb_push_lr(img.u16(a)))
                    .filter(|a| {
                        !self.decoded_mode.contains_key(a)
                            && !self.analysis.data_words.contains(&(a & !3))
                            && self.decoded_mode.get(&(a & !3)) != Some(&false)
                    })
                    .map(|a| a | 1),
            );
        } else {
            // gap filling: unclaimed ARM-aligned bytes right after ARM code or a literal
            // pool we already know about; and uncovered `push {.., lr}` prologues
            candidates.extend(
                (img.text.addr + 4..img.text.addr + img.text.size)
                    .step_by(4)
                    .filter(|a| !self.decoded_mode.contains_key(a) && !self.analysis.data_words.contains(a))
                    .filter(|&a| {
                        self.decoded_mode.get(&(a - 4)) == Some(&false)
                            || self.analysis.data_words.contains(&(a - 4))
                            || is_push_lr(img.u32(a).and_then(|w| arm::decode(a, w)))
                    }),
            );
        }
        candidates.sort_unstable();
        candidates.dedup();
        let tables = self.pointer_table_entries();

        let mut added = 0;
        for v in candidates {
            let (addr, thumb) = (v & !1, v & 1 != 0);
            if thumb != want_thumb || !img.in_text(addr) || (!thumb && addr % 4 != 0) {
                continue;
            }
            if self.analysis.functions.contains_key(&addr)
                || self.decoded_mode.get(&addr) == Some(&!thumb)
                || self.rejected.contains(&key(addr, thumb))
                || self.implausible.contains(&key(addr, thumb))
            {
                continue;
            }
            if self.plausible_entry(addr, thumb, tables.contains(&v)) {
                self.untrusted.push((addr, thumb));
                added += 1;
            } else {
                self.implausible.insert(key(addr, thumb));
                if thumb {
                    self.rejected_thumb += 1;
                }
            }
        }
        added
    }

    /// Values in runs of 3+ consecutive words in .rodata/.data that all point into .text, like
    /// vtables and callback tables. Being in such a run is strong evidence on its own.
    fn pointer_table_entries(&self) -> HashSet<u32> {
        let img = self.image;
        let mut out = HashSet::new();
        for seg in [&img.rodata, &img.data] {
            let mut run = Vec::new();
            for a in (seg.addr..seg.addr + seg.size + 4).step_by(4) {
                match img.u32(a).filter(|&w| a < seg.addr + seg.size && img.in_text(w & !1)) {
                    Some(w) => run.push(w),
                    None => {
                        if run.len() >= 3 {
                            out.extend(run.iter().copied());
                        }
                        run.clear();
                    }
                }
            }
        }
        out
    }

    /// Targets of self-relative pointer tables (each word is an offset from its own address,
    /// as in armcc's static-initializer table). Only runs of 8+ consecutive words that all
    /// land in .text count, which random data essentially never produces.
    fn relative_pointer_runs(&self) -> Vec<u32> {
        let img = self.image;
        let mut out = Vec::new();
        for seg in [&img.rodata, &img.data] {
            let mut run = Vec::new();
            for a in (seg.addr..seg.addr + seg.size).step_by(4) {
                let target = img.u32(a).map(|w| a.wrapping_add(w));
                match target {
                    Some(t) if img.in_text(t & !1) => run.push(t),
                    _ => {
                        if run.len() >= 8 {
                            out.append(&mut run);
                        }
                        run.clear();
                    }
                }
            }
            if run.len() >= 8 {
                out.append(&mut run);
            }
        }
        out
    }

    /// A pointer target looks like code if it decodes cleanly up to its first block end
    /// without running into code already decoded in the other mode.
    ///
    /// ARM data decodes as *something* most of the time, so ARM candidates must also start
    /// unconditionally and be at least half unconditional. Short compiled functions with
    /// conditional moves can be ~70% AL; random words are AL only ~1 time in 15. `in_table`
    /// (the pointer came from a vtable-like run) skips the ratio: tiny predicated accessors
    /// are common there.
    fn plausible_entry(&self, start: u32, thumb: bool, in_table: bool) -> bool {
        let mut addr = start;
        let (mut total, mut unconditional) = (0, 0);
        let ok = |total: u32, unconditional: u32| thumb || in_table || unconditional * 2 >= total;
        for _ in 0..48 {
            if let Some(&mode) = self.decoded_mode.get(&addr) {
                return mode == thumb && ok(total, unconditional);
            }
            let Some(insn) = self.decode(addr, thumb) else {
                return false;
            };
            if addr == start && insn.cond != Cond::Al {
                return false;
            }
            total += 1;
            unconditional += (insn.cond == Cond::Al) as u32;
            match flow(&insn) {
                Flow::Next | Flow::Call { .. } | Flow::IndirectCall => {}
                Flow::Jump { also_next: true, .. } | Flow::Return { also_next: true } => {}
                Flow::IndirectJump { also_next: true } => {}
                Flow::Stop => return false,
                _ => return ok(total, unconditional),
            }
            addr += insn.size as u32;
        }
        ok(total, unconditional)
    }

    /// An address in .text computed from PC: ARM `add/sub rX, pc, #imm`, or Thumb ADR
    /// (decoded as a `mov` of the resolved address; Thumb MOV immediates are 8-bit).
    fn pc_relative_value(&self, insn: &Insn) -> Option<u32> {
        let v = match insn.op {
            Op::DataProc {
                op: DpOp::Add,
                rn: PC,
                op2: Operand2::Imm { value, .. },
                ..
            } => insn.pc_value().wrapping_add(value),
            Op::DataProc {
                op: DpOp::Sub,
                rn: PC,
                op2: Operand2::Imm { value, .. },
                ..
            } => insn.pc_value().wrapping_sub(value),
            Op::DataProc {
                op: DpOp::Mov,
                s: false,
                op2: Operand2::Imm { value, .. },
                ..
            } if insn.thumb => value,
            _ => return None,
        };
        self.image.in_text(v & !1).then_some(v)
    }

    /// Records PC-relative literal loads in .text so coverage counts literal pools.
    fn literal_words(&self, insn: &Insn, out: &mut Vec<u32>) {
        let (addr, words) = match insn.op {
            Op::Mem {
                addr: MemAddr::Abs(a),
                size,
                ..
            } => (a, if size == MemSize::Dual { 2 } else { 1 }),
            Op::VMem {
                addr: MemAddr::Abs(a),
                d,
                ..
            } => (a, if matches!(d, FReg::D(_)) { 2 } else { 1 }),
            _ => return,
        };
        for i in 0..words {
            let a = addr + i * 4;
            if self.image.in_text(a) {
                out.push(a & !3);
            }
        }
    }

    /// `add pc, rA, rB` where one register is a constant base (from PC) and the other was loaded
    /// from `[base, rI(, lsl #2)]`: a table of offsets relative to the base. The length isn't
    /// bounded in the code, so entries are read while they give nearby, decodable code.
    fn relative_table_targets(&self, f: &Function, insn: &Insn) -> Option<Vec<u32>> {
        let Op::DataProc { op: DpOp::Add, rd: PC, rn, op2: Operand2::Reg(sh), .. } = insn.op else {
            return None;
        };
        if sh.kind != ShiftKind::Lsl || sh.amount != ShiftAmount::Imm(0) || insn.cond != Cond::Al || insn.thumb {
            return None;
        }
        let constant = |r: Reg| match self.resolve(f, insn.addr, r, 0) {
            Some(Value::Const(v)) => Some(v),
            _ => None,
        };
        let (base, loaded) = match (constant(rn), constant(sh.rm)) {
            (Some(b), None) => (b, sh.rm),
            (None, Some(b)) => (b, rn),
            _ => return None,
        };
        // the offset register must come from a load indexed off the same base
        let load = f.insns.range(..insn.addr).rev().take(8).map(|(_, i)| i).find(|i| i.writes() & (1 << loaded) != 0)?;
        let Op::Mem { load: true, size: MemSize::Word, addr: MemAddr::Base { rn: lb, offset: MemOffset::Reg(_), add: true, pre_index: true, writeback: false }, .. } = load.op else {
            return None;
        };
        if constant(lb).is_none_or(|v| v != base) && self.resolve(f, load.addr, lb, 0).is_none_or(|v| !matches!(v, Value::Const(c) if c == base)) {
            return None;
        }
        let mut targets = Vec::new();
        for i in 0..256u32 {
            let Some(off) = self.image.u32(base + i * 4) else { break };
            let t = base.wrapping_add(off);
            let near = t.abs_diff(base) < 0x10000;
            if !near || t % 4 != 0 || !self.image.in_text(t) || self.decode(t, false).is_none() {
                break;
            }
            targets.push(t);
        }
        (!targets.is_empty()).then_some(targets)
    }

    /// Recognizes ARM switch idioms guarded by `cmp rX, #N` with condition LS (or LO):
    ///   `addls pc, pc, rX, lsl #2`  followed by N+1 `b` instructions
    ///   `ldrls pc, [pc, rX, lsl #2]` followed by N+1 address words
    /// (the instruction after the jump is the default case, reached by fallthrough).
    fn jump_table(&self, f: &Function, insn: &Insn, data: &mut Vec<u32>) -> Option<Vec<u32>> {
        // LS after `cmp rX, #N` allows 0..=N; LO allows 0..N
        let inclusive = match insn.cond {
            Cond::Ls => true,
            Cond::Cc => false,
            _ => return None,
        };
        if insn.thumb {
            return None;
        }
        let index = match insn.op {
            Op::DataProc {
                op: DpOp::Add,
                rn: PC,
                op2:
                    Operand2::Reg(Shifted {
                        rm,
                        kind: ShiftKind::Lsl,
                        amount: ShiftAmount::Imm(2),
                    }),
                ..
            } => rm,
            Op::Mem {
                load: true,
                size: MemSize::Word,
                addr:
                    MemAddr::Base {
                        rn: PC,
                        offset:
                            MemOffset::Reg(Shifted {
                                rm,
                                kind: ShiftKind::Lsl,
                                amount: ShiftAmount::Imm(2),
                            }),
                        add: true,
                        pre_index: true,
                        writeback: false,
                    },
                ..
            } => rm,
            _ => return None,
        };
        let is_add = matches!(insn.op, Op::DataProc { .. });

        // the bounding `cmp index, #N` is the last flag-setting instruction before the jump;
        // the compiler may schedule unrelated loads/stores in between
        let mut max = None;
        for (_, prev) in f.insns.range(..insn.addr).rev().take(16) {
            if prev.writes() & (1 << index) != 0 || !matches!(flow(prev), Flow::Next) {
                break;
            }
            match prev.op {
                Op::DataProc {
                    op: DpOp::Cmp,
                    rn,
                    op2: Operand2::Imm { value, .. },
                    ..
                } if rn == index && prev.cond == Cond::Al => {
                    max = Some(value);
                    break;
                }
                Op::DataProc { op, s, .. } if s || op.is_test() => break,
                Op::Mul { .. } | Op::Mrs { .. } | Op::Msr { .. } => break,
                _ => {}
            }
        }
        let max = max?;
        if max > 4096 || (!inclusive && max == 0) {
            return None;
        }
        let count = if inclusive { max + 1 } else { max };

        let table = insn.pc_value();
        let mut targets = Vec::with_capacity(count as usize);
        let mut slots = Vec::new();
        for i in 0..count {
            let slot = table + i * 4;
            if is_add {
                // each slot is a branch instruction, decoded as part of the function
                targets.push(slot);
            } else {
                let target = self.image.u32(slot)?;
                if target & 1 != 0 {
                    return None; // interworking into Thumb; not this idiom
                }
                slots.push(slot);
                targets.push(target);
            }
        }
        data.extend(slots);
        Some(targets)
    }
}

/// Discovery summary for reporting.
pub struct Discovery {
    pub analysis: Analysis,
    /// Functions reachable by direct calls (and resolved veneers) from the entry point.
    pub direct: usize,
    pub rejected_arm: usize,
    pub rejected_thumb: usize,
    pub noreturn: HashSet<u32>,
    /// Discovery passes run until the noreturn set stopped changing.
    pub passes: usize,
}

/// Full discovery: direct calls from the entry point, then pointer seeding, repeated with
/// an updated set of noreturn functions until that set is stable.
pub fn discover(image: &Image) -> Discovery {
    let mut noreturn = HashSet::new();
    let mut passes = 0;
    loop {
        passes += 1;
        let mut d = Discoverer::new(image, &noreturn);
        d.add_entry(image.text.addr, false);
        d.run();

        // ARM candidates first: their evidence is stronger, and ARM code read as Thumb
        // often decodes cleanly, so Thumb candidates must not claim bytes ARM code owns
        loop {
            while d.seed_pointers(false) > 0 {
                d.run();
            }
            if d.seed_pointers(true) == 0 {
                break;
            }
            d.run();
        }
        let (rejected_arm, rejected_thumb) = d.rejected_counts();
        let direct = d.trusted_count();
        let analysis = d.finish();
        let found = noreturn_functions(&analysis.functions);
        if found == noreturn || passes == 10 {
            return Discovery {
                analysis,
                direct,
                rejected_arm,
                rejected_thumb,
                noreturn,
                passes,
            };
        }
        noreturn = found;
    }
}

/// A function returns if it reaches a return (or an exit we can't see through), or
/// tail-calls a function that returns. Unknown tail-call targets are assumed to return.
fn noreturn_functions(functions: &BTreeMap<u32, Function>) -> HashSet<u32> {
    let mut returns: HashSet<u32> = functions.values().filter(|f| f.exits).map(|f| f.entry).collect();
    loop {
        let before = returns.len();
        for f in functions.values() {
            if !returns.contains(&f.entry)
                && f.tail_calls
                    .iter()
                    .any(|t| returns.contains(t) || !functions.contains_key(t))
            {
                returns.insert(f.entry);
            }
        }
        if returns.len() == before {
            break;
        }
    }
    functions.keys().copied().filter(|e| !returns.contains(e)).collect()
}
