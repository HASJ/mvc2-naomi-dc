#!/usr/bin/env python3
"""psp_order.py ELF MAP OUT RUN_ELF:SAMPLES...: the PSP's hot routines first in .text (port/psp/hot_text.ld), from
the sampler's runs (port/psp/psp_prof.c), each run's ELF the one it ran. ELF and MAP are a build with
-ffunction-sections (-Wl,-Map) whose input sections the order names; routines go by samples a byte, the busiest
first, until --cover of the samples is placed."""
import argparse
import bisect
import collections
import re
import struct
import subprocess


def symbols(elf, nm):
    """(start, size, name) of the routines, by address"""
    out = subprocess.run([nm, "-n", "-S", "--defined-only", elf], capture_output=True, text=True, check=True).stdout
    syms = []
    for line in out.splitlines():
        f = line.split()
        if len(f) == 4 and f[2] in "tTwW":
            syms.append((int(f[0], 16), int(f[1], 16), f[3]))
    return syms


def hot(elf, samples, nm):
    """each routine's share of the run's samples, by (name, size)"""
    data = open(samples, "rb").read()
    magic, _, _, n = struct.unpack_from("<4I", data)
    if magic != 0x53325643:
        raise SystemExit(f"{samples}: not a samples file")
    syms = symbols(elf, nm)
    starts = [s[0] for s in syms]
    counts = collections.Counter()
    for w in struct.unpack_from(f"<{n}I", data, 16):
        i = bisect.bisect_right(starts, w) - 1
        if w < 0xFFFF0000 and i >= 0 and w < syms[i][0] + syms[i][1]:
            counts[syms[i][2], syms[i][1]] += 1
    total = sum(counts.values()) or 1
    return {k: v / total for k, v in counts.items()}


def sections(path):
    """the map's input sections in .text: (address, size, section, file)"""
    out, name = [], None
    pat = re.compile(r"^ (\.text\S*)?\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$")
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.rstrip("\n")
        if re.match(r"^ \.text\S*$", line):
            name = line.strip()
            continue
        m = pat.match(line)
        if m and (m.group(1) or name):
            sec = m.group(1) or name
            if int(m.group(3), 16) > 0 and sec.startswith(".text"):
                out.append((int(m.group(2), 16), int(m.group(3), 16), sec, m.group(4).strip()))
        name = None
    return sorted(out)


def pattern(sec, path):
    """the order script's name for an input section"""
    m = re.match(r"^(.*/)?([^/(]+\.a)\(([^)]+)\)$", path)
    if m:
        return f"*{m.group(2)}:{m.group(3)}({sec})"
    return f"*{path.rsplit('/', 1)[-1]}({sec})"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("map")
    ap.add_argument("out")
    ap.add_argument("runs", nargs="+", help="ELF:samples.bin, each run with the ELF it ran")
    ap.add_argument("--cover", type=float, default=0.98, help="the share of the samples placed")
    ap.add_argument("--nm", default="psp-nm")
    a = ap.parse_args()
    share = collections.Counter()
    for run in a.runs:
        elf, samples = run.rsplit(":", 1)
        for k, v in hot(elf, samples, a.nm).items():
            share[k] += v / len(a.runs)
    syms = collections.defaultdict(list)
    for start, size, name in symbols(a.elf, a.nm):
        syms[name].append((start, size))
    secs = sections(a.map)
    starts = [s[0] for s in secs]
    placed, lines, done, total = set(), [], 0.0, sum(share.values())
    for (name, size), v in sorted(share.items(), key=lambda kv: -kv[1] / max(kv[0][1], 4)):
        if done >= a.cover * total:
            break
        done += v
        for start, _ in sorted(syms.get(name, []), key=lambda s: abs(s[1] - size))[:1]:
            i = bisect.bisect_right(starts, start) - 1
            if i >= 0 and start < secs[i][0] + secs[i][1]:
                p = pattern(secs[i][2], secs[i][3])
                if p not in placed:
                    placed.add(p)
                    lines.append(f"    {p}    /* {name} */")
    with open(a.out, "w", newline="\n") as f:
        f.write("/* the routines the PSP runs most, together at the start of .text (port/tools/psp_order.py) */\n")
        f.write(".text : {\n" + "\n".join(lines) + "\n}\n")
    print(f"{len(lines)} sections for {100 * done / total:.1f}% of the samples")


if __name__ == "__main__":
    main()
