# Phase 1: recompiler

Tool: `tools/recomp` (Rust). Run `cargo run --release -p recomp -- analyze extracted` after `ctrx`.

## Stage 1: decoder (done)

`arm.rs` and `thumb.rs` decode ARMv6K + VFPv2 (ARM) and Thumb-1 into one form (`insn.rs`). Thumb is
decoded to ARM equivalents, and PC-relative immediate addresses are resolved at decode time.

Checked against `llvm-objdump` with `tools/check_decoder.py`, which decodes every word of .text as ARM
and every halfword as Thumb (so data and wrong-mode bytes are included):

| | Compared | Match / both undefined |
|---|---|---|
| ARM | 1,559,343 | 99.6% |
| Thumb | 2,795,230 | 98.0% |

Every remaining category of difference was reviewed. They are encodings the 3DS CPU doesn't have and we
deliberately reject (Thumb-2, ARMv6T2 loads like `ldrht`, non-VFP coprocessors, odd-register `ldrd`,
should-be-one/zero fields), or harmless spelling differences (`pld` operands).

## Stage 2: function discovery (done, 96.7%)

`discover.rs`, recursive descent with:

- **ARM/Thumb mode tracking** through `blx` and veneers (`add r12, pc, #1; bx r12`).
- **Value tracking** back through straight-line code to resolve `bx rN` (veneers, tail calls,
  `pop {rN}; bx rN` returns) and `blx rN` to constants.
- **Jump tables**: `cmp rX, #N` + `addls pc, pc, rX, lsl #2` / `ldrls|ldrlo pc, [pc, rX, lsl #2]`.
- **Noreturn detection**: Break/ExitProcess SVCs end a path; functions that can't reach a return are
  noreturn; discovery reruns until that set is stable, so code after `bl abort` isn't treated as code.
- **Provisional candidates**, kept only if they explore cleanly (no undecodable bytes, mode conflicts or
  calls outside .text). ARM ones must also start unconditional and be mostly unconditional. Sources:
  - absolute pointers in .rodata, .data, literal pools and unclaimed parts of .text
  - runs of 8+ self-relative pointers (the 294-entry static-initializer table at `0x80BDDC`)
  - PC-relative computed addresses (`add rX, pc, #imm`, Thumb ADR)
  - gap filling after ARM code or literal pools, and uncovered `push {.., lr}` prologues
  - ARM candidates are processed before Thumb ones; ARM code read as Thumb often decodes cleanly

Result (about 12 s):

| | |
|---|---|
| Functions | 27,075 (26,732 ARM, 343 Thumb); 1,907 by direct calls from the entry point |
| Instructions | 1.51M |
| Coverage of .text | 96.7% including 43.6k literal-pool / jump-table words |
| Jump tables resolved | 437 |
| Noreturn functions | 24 |
| Unresolved indirect jumps | 734 |

## Known gaps

- **3.3% of .text uncovered**: strings and tables stored in .text, ~330 `push {.., lr}` prologues no
  candidate source reaches (likely dead code or reached through something not yet modeled), and parts
  of hand-written assembly around `0x190000-0x1A9000` (an Exp-Golomb bitstream decoder, likely
  Mobiclip video).
- **734 unresolved indirect jumps**: `bx rN` / `add pc` / `ldr pc` whose target isn't a constant or a
  recognized table. Many are in the hand-written assembly. Each needs a pattern or a manual annotation.
- **Runtime safety net (planned)**: since there's no interpreter, the runtime will stop with the address
  if code ever jumps somewhere that wasn't recompiled, and that address goes into a manual entry list.

## Stage 3: C emitter (done for register semantics)

`emit.rs` writes one C function per discovered function (`recomp emit extracted build/generated`):

- Guest state in `Ctx` (`runtime/include/recomp.h`): r0-r15, NZCV/Q/GE, s0-s31, FPSCR, TLS.
- Memory through `recomp_mem + addr` (unaligned-safe `memcpy` accessors).
- Control flow inside a function is labels and `goto`; jump tables become `switch`.
- Calls are C calls with LR set; returns (`bx lr`, `pop {.., pc}`, `ldr pc, [sp], #4`,
  popped-register `bx`) are `return`; tail calls are `f(ctx); return;`.
- Unresolved register calls and jumps go through `recomp_call_indirect`, which looks the address up
  in the generated `lookup.c` table and stops with the address if it wasn't recompiled.
- **Flag liveness**: flags are computed only if a later instruction can read them. Calls count as
  defining all flags (EABI helpers like `__aeabi_cfcmple` return results in flags) and returns count
  as using them all.
- VFP ops are compiled with `FP_CONTRACT OFF` so `vmla` etc. round the product separately, like VFPv2.

Output: 27,082 functions, 100 files, 213 MB of C. A full `clang -O2` build takes 2.5 minutes on
8 cores, with 0 warnings and 0 errors; object code is 29 MB.

### Verification: differential testing against qemu

`tools/difftest.py extracted [instructions] [states] [seed]` samples instructions from the game's code
(evenly per mnemonic), runs each from random starting states as real ARM code under `qemu-arm` and as
emitted C natively, and compares registers, NZCVQ/GE flags, VFP registers and FPSCR flags.

- 3,000 instructions x 6 states = 18,000 tests, two seeds: **all pass**.
- Covers data processing with every flag behavior, shifts by immediate and register, all multiplies,
  saturating and packed-SIMD media ops, extends, `rev`, `pkh`, `sel`, `clz`, and VFP arithmetic,
  compares and conversions in single and double precision.
- A deliberately broken emitter (`bic` as `| ~`) produced 330 failures, so the check does detect bugs.

## Known limitations

- Memory instructions and control flow aren't covered by the differential test yet; they need a
  harness with a memory buffer and branches.
- Not modeled: FPSCR rounding modes other than round-to-nearest, flush-to-zero, default-NaN mode,
  floating-point exception flags. Need to check which modes the game sets.
- Every return is assumed to go back to its caller. `longjmp`-style code that pops a different
  return address needs special handling once we find it.
- Flags are treated as live at every return, so the last compare before a return computes all
  four flags. A possible optimization once correctness is settled.
- `ldm`/`stm` with `^`, `setend`, `cps`, and coprocessors other than the TLS register stop the
  program with a message. User-mode game code shouldn't use them.

## Next: Phase 2, runtime

The generated code links against runtime functions that don't exist yet: `recomp_svc`,
`recomp_call_indirect`, `recomp_unhandled`, `recomp_coproc`, and the guest memory setup.
