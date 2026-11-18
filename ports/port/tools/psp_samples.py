#!/usr/bin/env python3
"""psp_samples.py ELF samples.bin [--every N] [--lines ROUTINE ...]: the PSP sampler's hot list (port/psp/psp_prof.c),
by routine and by source file (from the objects beside the ELF), for the whole run and for each N hundred frames
(or each slow frame kept by MVC2_SAMPLE_SLOW); with --lines, those routines by source line (a build with
MVC2_PSP_LINES=ON)."""
import argparse
import bisect
import collections
import os
import struct
import subprocess


def symbols(elf, nm):
    out = subprocess.run([nm, "-n", "-S", "--defined-only", elf], capture_output=True, text=True, check=True).stdout
    starts, names = [], []
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 3 and f[-2] in "tTwW":
            starts.append(int(f[0], 16))
            names.append(f[-1])
    return starts, names


def files(elf, nm):
    """each routine's source file, from the objects (*.c.obj) in the build folder beside the ELF"""
    objs = []
    for root, _, names in os.walk(os.path.dirname(os.path.abspath(elf))):
        objs += [os.path.join(root, f) for f in names if f.endswith(".c.obj")]
    where = {}
    for i in range(0, len(objs), 200):
        out = subprocess.run([nm, "-A", "--defined-only"] + objs[i:i + 200], capture_output=True, text=True).stdout
        for line in out.splitlines():
            f = line.split()
            if len(f) == 3 and f[1] in "tT":
                where.setdefault(f[2], os.path.basename(f[0].rsplit(":", 1)[0])[:-4])
    return where


def table(title, counts, total, top):
    print(title)
    for name, n in counts.most_common(top):
        print(f"  {100.0 * n / total:5.1f}%  {n:6}  {name}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("samples")
    ap.add_argument("--every", type=int, default=0, help="a table each N hundred frames too")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--nm", default="psp-nm")
    ap.add_argument("--lines", nargs="*", default=[], help="these routines by source line")
    ap.add_argument("--addr2line", default="psp-addr2line")
    a = ap.parse_args()
    data = open(a.samples, "rb").read()
    magic, text0, text1, n = struct.unpack_from("<4I", data)
    if magic != 0x53325643:
        raise SystemExit("not a samples file")
    words = struct.unpack_from(f"<{n}I", data, 16)
    starts, names = symbols(a.elf, a.nm)
    where = files(a.elf, a.nm)
    total = collections.Counter()
    at = collections.defaultdict(collections.Counter)
    by_file = collections.Counter()
    parts = []
    part, mark = collections.Counter(), None
    for w in words:
        if 0xFFFE0000 <= w < 0xFFFF0000:
            if part:
                parts.append((mark, part))
            part, mark = collections.Counter(), f"{w & 0xFFFF} (slow)"
            continue
        if w >= 0xFFFF0000:
            if a.every and (w & 0xFFFF) % a.every == 0:
                if part:
                    parts.append((mark, part))
                part, mark = collections.Counter(), (w & 0xFFFF) * 100
            continue
        if 0x88000000 <= w < 0xC0000000:
            name = "(kernel)"
        elif text0 <= w < text1:
            i = bisect.bisect_right(starts, w) - 1
            name = names[i] if i >= 0 else "?"
        else:
            name = f"(outside the program {w >> 20 << 20:08X})"
        total[name] += 1
        if name in a.lines:
            at[name][w] += 1
        by_file[where.get(name, name)] += 1
        part[name] += 1
    if part and (a.every or isinstance(mark, str)):
        parts.append((mark, part))
    count = sum(total.values())
    print(f"{count} samples, program text {text0:08X}-{text1:08X}")
    table("by routine:", total, count, a.top)
    table("by source file:", by_file, count, a.top)
    for name in a.lines:
        addrs = sorted(at[name])
        out = subprocess.run([a.addr2line, "-e", a.elf] + [f"{x:x}" for x in addrs], capture_output=True,
                             text=True).stdout.splitlines()
        lines = collections.Counter()
        for x, line in zip(addrs, out):
            lines[os.path.basename(line)] += at[name][x]
        table(f"{name} by line:", lines, max(1, sum(lines.values())), a.top)
    for mark, p in parts:
        table(f"from frame {mark}:", p, sum(p.values()), 15)


if __name__ == "__main__":
    main()
