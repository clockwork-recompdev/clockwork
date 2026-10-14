#!/usr/bin/env python3
"""Differential test of the C emitter against real ARM semantics (qemu-arm).

Samples register-only instructions from the game's code, runs each from random starting
states both as ARM code under qemu-arm and as emitted C natively, and compares the
resulting registers, flags and VFP state.

usage: difftest.py <extracted_dir> [instructions=500] [states=4] [seed=1]
Needs: clang + ld.lld (ARM target), qemu-arm-static (or qemu-arm), a release build of recomp.
"""

import collections
import os
import shutil
import struct
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RECOMP = os.path.join(REPO, "target", "release", "recomp")
OUT = os.path.join(REPO, "build", "difftest")
WORDS = 48
CPSR, S0, FPSCR = 14, 15, 47
CPSR_MASK = 0xF80F0000  # NZCVQ + GE; mode/state bits aren't modeled
FPSCR_MASK = 0xF0000000  # NZCV; cumulative exception flags aren't modeled
REG_NAMES = [f"r{i}" for i in range(13)] + ["lr", "cpsr"] + [f"s{i}" for i in range(32)] + ["fpscr"]


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def is_nan32(w):
    return (w & 0x7F800000) == 0x7F800000 and (w & 0x7FFFFF) != 0


def is_nan64(lo, hi):
    return (hi & 0x7FF00000) == 0x7FF00000 and ((hi & 0xFFFFF) | lo) != 0


def diff(a, b):
    """Names of state fields that differ, treating any NaN as equal to any NaN."""
    out = []
    for i in range(WORDS):
        x, y = a[i], b[i]
        if i == CPSR:
            x, y = x & CPSR_MASK, y & CPSR_MASK
        elif i == FPSCR:
            x, y = x & FPSCR_MASK, y & FPSCR_MASK
        elif S0 <= i < S0 + 32 and x != y:
            k = i - S0
            pair = (k & ~1) + S0
            if is_nan32(x) and is_nan32(y):
                continue
            if is_nan64(a[pair], a[pair + 1]) and is_nan64(b[pair], b[pair + 1]):
                continue
        if x != y:
            out.append(f"{REG_NAMES[i]}: arm={x:08x} ours={y:08x}")
    return out


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    extracted = sys.argv[1]
    count, states, seed = (int(a) for a in (sys.argv[2:] + ["500", "4", "1"][len(sys.argv) - 2:])[:3])
    qemu = shutil.which("qemu-arm-static") or shutil.which("qemu-arm")
    if not qemu:
        sys.exit("qemu-arm not found")

    run([RECOMP, "difftest", extracted, OUT, str(count), str(states), str(seed)])
    run(["clang", "--target=armv6k-linux-gnueabihf", "-mfpu=vfpv2", "-nostdlib", "-static",
         "-fuse-ld=lld", os.path.join(OUT, "harness.S"),
         "-o", os.path.join(OUT, "harness")])
    run(["clang", "-std=c17", "-O1", "-ffp-contract=off", "-w", "-I", os.path.join(REPO, "runtime", "include"),
         os.path.join(OUT, "native.c"), os.path.join(REPO, "runtime", "src", "arm_ops.c"),
         "-lm", "-o", os.path.join(OUT, "native")])
    arm = run([qemu, os.path.join(OUT, "harness")], capture_output=True).stdout
    ours = run([os.path.join(OUT, "native")], capture_output=True).stdout

    with open(os.path.join(OUT, "tests.tsv")) as f:
        tests = [line.rstrip("\n").split("\t") for line in f]
    stride = WORDS * 4
    if len(arm) != len(ours) or len(arm) != len(tests) * stride:
        sys.exit(f"size mismatch: arm {len(arm)} ours {len(ours)} expected {len(tests) * stride}")

    failures = collections.defaultdict(list)
    for i, (idx, addr, raw, text) in enumerate(tests):
        a = struct.unpack_from(f"<{WORDS}I", arm, i * stride)
        b = struct.unpack_from(f"<{WORDS}I", ours, i * stride)
        d = diff(a, b)
        if d:
            mnemonic = text.split()[0]
            failures[mnemonic].append((addr, raw, text, d))

    failed = sum(len(v) for v in failures.values())
    print(f"{len(tests)} tests ({count} instructions x {states} states): {len(tests) - failed} passed, {failed} failed")
    for mnemonic, items in sorted(failures.items(), key=lambda kv: -len(kv[1])):
        print(f"\n[{len(items)}] {mnemonic}")
        for addr, raw, text, d in items[:3]:
            print(f"    {addr} {raw}  {text}")
            for line in d[:4]:
                print(f"        {line}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
