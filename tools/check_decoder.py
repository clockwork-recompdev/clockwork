#!/usr/bin/env python3
"""Cross-checks the recomp decoder against llvm-objdump over the game's whole .text.

Both disassemble every ARM word and every Thumb halfword (linear sweep, so data and the
wrong instruction set get decoded too, which is good coverage). For each address it
compares the mnemonic, the registers mentioned, and branch targets, then prints mismatch
counts by mnemonic with examples.

usage: check_decoder.py <extracted_dir> arm|thumb [max_examples]
Needs llvm-objcopy, llvm-objdump, and a release build of recomp.
"""

import collections
import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RECOMP = os.path.join(REPO, "target", "release", "recomp")
REG_RE = re.compile(r"\b(r\d+|sp|lr|pc|[sd]\d+)\b")
HEX_RE = re.compile(r"0x[0-9a-f]+")

# llvm aliases that our formatter spells differently
MNEMONIC_ALIASES = {"adr": "mov", "ldmia": "ldm", "stmia": "stm", "cpy": "mov"}


def text_segment(extracted):
    with open(os.path.join(extracted, "exheader.bin"), "rb") as f:
        hdr = f.read(0x20)
    addr = int.from_bytes(hdr[0x10:0x14], "little")
    size = int.from_bytes(hdr[0x18:0x1C], "little")
    return addr, size


def llvm_lines(extracted, mode, addr, size, tmp):
    text = os.path.join(tmp, "text.bin")
    with open(os.path.join(extracted, "code.bin"), "rb") as src, open(text, "wb") as dst:
        dst.write(src.read(size))
    elf = os.path.join(tmp, "text.elf")
    subprocess.run(
        ["llvm-objcopy", "-I", "binary", "-O", "elf32-littlearm",
         "--rename-section", ".data=.text,code,alloc,load,readonly", text, elf],
        check=True,
    )
    triple = "armv6k-none-eabi" if mode == "arm" else "thumbv6k-none-eabi"
    out = subprocess.run(
        ["llvm-objdump", "-d", f"--triple={triple}", "--mattr=+vfp2", f"--adjust-vma={addr:#x}",
         "--no-show-raw-insn", elf],
        check=True, capture_output=True, text=True,
    ).stdout
    result = {}
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+(.*)$", line)
        if not m:
            continue
        body = re.sub(r"<_binary[^>]*>", "", m.group(2)).split("@")[0].strip()
        result[int(m.group(1), 16)] = body
    return result


def ours_lines(extracted, mode):
    out = subprocess.run([RECOMP, "disasm", extracted, mode], check=True, capture_output=True, text=True).stdout
    result = {}
    for line in out.splitlines():
        a, _, body = line.partition(": ")
        result[int(a, 16)] = body.strip()
    return result


def split(body):
    parts = body.split(None, 1)
    if not parts:
        return "", ""
    return parts[0], (parts[1].strip() if len(parts) > 1 else "")


def regs(ops):
    return sorted(REG_RE.findall(ops))


def main():
    if len(sys.argv) < 3 or sys.argv[2] not in ("arm", "thumb"):
        sys.exit(__doc__)
    extracted, mode = sys.argv[1], sys.argv[2]
    max_examples = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    addr, size = text_segment(extracted)

    with tempfile.TemporaryDirectory() as tmp:
        theirs = llvm_lines(extracted, mode, addr, size, tmp)
    ours = ours_lines(extracted, mode)

    stats = collections.Counter()
    problems = collections.defaultdict(list)
    for a, t in theirs.items():
        o = ours.get(a)
        if o is None:
            stats["address only in llvm"] += 1
            continue
        t_unknown = t.startswith("<unknown>")
        o_unknown = o.startswith("<undefined>")
        if t_unknown and o_unknown:
            stats["both undefined"] += 1
            continue
        tm, tops = split(t)
        om, oops = split(o)
        if t_unknown or o_unknown:
            key = f"undefined-in-{'llvm' if t_unknown else 'ours'}:{om if t_unknown else tm}"
            problems[key].append((a, t, o))
            continue
        tm = MNEMONIC_ALIASES.get(tm, tm)
        if tm != om:
            problems[f"mnemonic:{tm} vs {om}"].append((a, t, o))
            continue
        # PC-relative immediates are resolved to absolute addresses on our side
        t_regs = regs(re.sub(r"\[pc, #-?\w+\]|\[pc\]", "", tops))
        o_regs = regs(oops)
        if mode == "thumb":
            # llvm prints Thumb two-operand forms (adds r0, r1 = adds r0, r0, r1)
            t_regs, o_regs = sorted(set(t_regs)), sorted(set(o_regs))
        if t_regs != o_regs:
            problems[f"registers:{tm}"].append((a, t, o))
            continue
        if tm.rstrip("abcdefghilmnopqrstuvwxyz") == "" and tm[:1] == "b" and not tops.startswith(("r", "sp", "lr", "pc")):
            th, oh = HEX_RE.findall(tops), HEX_RE.findall(oops)
            if th[:1] != oh[:1]:
                problems[f"target:{tm}"].append((a, t, o))
                continue
        stats["match"] += 1

    total = sum(stats.values()) + sum(len(v) for v in problems.values())
    print(f"{mode}: {total} addresses compared")
    for k, v in stats.most_common():
        print(f"  {k}: {v}")
    print(f"  mismatches: {sum(len(v) for v in problems.values())}")
    for key, items in sorted(problems.items(), key=lambda kv: -len(kv[1])):
        print(f"\n[{len(items)}] {key}")
        for a, t, o in items[:max_examples]:
            print(f"    {a:x}: llvm: {t}\n    {' ' * len(f'{a:x}')}  ours: {o}")


if __name__ == "__main__":
    main()
