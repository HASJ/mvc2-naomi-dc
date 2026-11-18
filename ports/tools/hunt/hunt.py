"""Run one input script in the port and in Flycast (the arcade ROM), dump the game's work each game
frame in both, and show where the players first differ.

  python -I tools/hunt/hunt.py SCRIPT OUTDIR [--port-only] [--fc-only] [--compare-only]
"""
import os, re, shutil, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DECOMP = os.environ.get("MVC2_DECOMP", os.path.dirname(ROOT))
EXE = os.environ.get("HUNT_EXE", os.path.join(ROOT, "build-pc", "mvc2.exe"))
FC = os.environ.get("HUNT_FC", os.path.join(ROOT, "build", "fc"))
ROM = os.path.join(DECOMP, "roms", "mvsc2.7z")
NM = "C:/msys64/mingw32/bin/nm.exe"
NAMES = ["pl_work", "vs_sys_work", "sw_work", "sys_work", "sel_w", "nl_random_next", "work_buff", "rnd_hi"]
DUMP = [("pl_work", 0x5A4 * 6), ("vs_sys_work", 0xC0), ("sw_work", 0x28), ("sys_work", 0x100), ("rnd_hi", 4)]
if os.environ.get("HUNT_WORKS"):
    DUMP.append(("work_buff", 0x1D0 * 256))     # the effects' pool
PLW = 0x5A4


def naomi_syms():
    syms = {"rnd_hi": 0x0C283048}       # o_sub1.c's static, rnd_lo after it (from rnd_init's pool)
    for line in open(os.path.join(DECOMP, "build", "mvc2.map")):
        m = re.match(r"\s*_(\w+)\s+H'([0-9A-F]{8})\s+DAT", line)
        if m and m.group(1) in NAMES:
            syms[m.group(1)] = int(m.group(2), 16)
    return syms


def port_syms(exe=None):
    """The arcade program's copies: the executable has the Dreamcast's too, told apart by their
    distances from sys_work in arc.o."""
    def table(path):
        t = {}
        for line in subprocess.run([NM, path], capture_output=True, text=True).stdout.splitlines():
            p = line.split()
            if len(p) == 3 and p[2].lstrip("_") in NAMES:
                t.setdefault(p[2].lstrip("_"), []).append(int(p[0], 16))
        return t
    exe = exe or EXE
    arc_o = os.path.join(os.path.dirname(os.path.abspath(exe)), "arc.o")     # the exe's own build
    if not os.path.exists(arc_o):
        arc_o = os.path.join(ROOT, "build-pc", "arc.o")
    arc, exe = table(arc_o), table(exe)
    for base in exe["sys_work"]:
        syms = {}
        for n in NAMES:
            if n not in arc and len(exe.get(n, [])) == 1:
                syms[n] = exe[n][0]     # the library's, shared by both programs
                continue
            want = base + arc[n][0] - arc["sys_work"][0]
            if want in exe.get(n, []):
                syms[n] = want
        if len(syms) == len(NAMES):
            return syms
    sys.exit("port: the arcade's symbols not found")


def env_for(syms, script, trace):
    e = dict(os.environ)
    e["MVC2_HUNT"] = script
    e["MVC2_HUNT_SYMS"] = ",".join("%s=0x%X" % kv for kv in syms.items())
    e["MVC2_HUNT_DUMP"] = ",".join("%s:0x%X" % d for d in DUMP)
    e["MVC2_HUNT_TRACE"] = trace
    return e


def run_port(script, out, exe=None, sub="port"):
    script, out = os.path.abspath(script), os.path.abspath(out)
    exe = exe or EXE
    syms = port_syms(exe)
    missing = [n for n in NAMES if n not in syms]
    if missing:
        sys.exit("port: no symbols %s" % missing)
    work = os.path.join(out, sub)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    e = env_for(syms, script, os.path.join(work, "trace.bin"))
    e.update(MVC2_FRAMES=os.environ.get("HUNT_FRAMES", "30000"), MVC2_CLOCK="946684800", SDL_VIDEODRIVER="offscreen",
             PATH="C:/msys64/mingw32/bin;" + e.get("PATH", ""))
    if os.environ.get("HUNT_SHOT"):
        e["MVC2_SHOT"] = os.environ["HUNT_SHOT"]
    with open(os.path.join(work, "run.log"), "w") as log:
        r = subprocess.run([exe, "--arcade", os.path.join(DECOMP, "roms", "mvsc2")], cwd=work, env=e,
                           stdout=log, stderr=subprocess.STDOUT, timeout=1800)
    print("port: exit", r.returncode)


def run_fc(script, out, fc=None):
    fc = os.path.abspath(fc or FC)
    script, out = os.path.abspath(script), os.path.abspath(out)
    work = os.path.join(out, "fc")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    data = os.path.join(fc, "data")
    for f in os.listdir(data):
        if f.startswith("mvsc2"):
            os.remove(os.path.join(data, f))
    cfg = open(os.path.join(fc, "emu.cfg")).read()
    cfg = re.sub(r"LuaFileName = .*", "LuaFileName = hunt.lua", cfg)
    shutil.copy(os.path.join(HERE, "hunt.lua"), os.path.join(fc, "hunt_full.lua"))
    open(os.path.join(fc, "hunt.lua"), "w").write(
        'local ok, err = pcall(dofile, "hunt_full.lua")\n'
        'if not ok then local f = io.open("hunt_lua_err.txt", "w") f:write(tostring(err)) f:close() os.exit(1) end\n')
    open(os.path.join(fc, "emu.cfg"), "w").write(cfg)
    e = env_for(naomi_syms(), script, os.path.join(work, "trace.bin"))
    e["MVC2_HUNT_MAXVBL"] = os.environ.get("HUNT_MAXVBL", "60000")
    with open(os.path.join(work, "run.log"), "w") as log:
        si = subprocess.STARTUPINFO()
        si.dwFlags, si.wShowWindow = subprocess.STARTF_USESHOWWINDOW, 7     # minimised, without taking focus
        r = subprocess.run([os.path.join(fc, "flycast.exe"), ROM], cwd=fc, env=e, startupinfo=si,
                           stdout=log, stderr=subprocess.STDOUT, timeout=3600)
    print("fc: exit", r.returncode)
    err = os.path.join(fc, "hunt_lua_err.txt")
    if os.path.exists(err):
        print("fc lua:", open(err).read())
        os.remove(err)


def frames(path):
    size = 8 + sum(n for _, n in DUMP)
    data = open(path, "rb").read()
    out = []
    for i in range(0, len(data) - size + 1, size):
        rec = data[i:i + size]
        assert rec[:4] == b"HUNT"
        parts, at = {}, 8
        for name, n in DUMP:
            parts[name] = rec[at:at + n]
            at += n
        parts["host"] = struct.unpack_from("<I", rec, 4)[0]
        out.append(parts)
    return out


def fields():
    """PLAYER_WORK's fields: (offset, size, name, kind) with kind 'p' pointer, 'f' float, 'i' integer."""
    out = []
    text = open(os.path.join(DECOMP, "include", "pl_work.h")).read()
    body = text[text.index("typedef struct PLAYER_WORK {"):text.index("} PLAYER_WORK;")]
    for line in body.splitlines()[1:]:
        m = re.match(r"\s*(.+?)\s*;\s*/\*\s*([0-9a-f]{3})\b", line)
        if not m:
            continue
        decl, off = m.group(1), int(m.group(2), 16)
        name = re.findall(r"(\w+)\s*(?:\[[^\]]*\])*\s*(?:\)\s*\(.*\))?$", decl)
        name = re.search(r"\(\s*\*+\s*(\w+)", decl).group(1) if "(" in decl else (name[0] if name else decl)
        kind = "p" if "*" in decl else "f" if re.match(r"(Float|VEC\d|MATRIX)\b", decl) else "i"
        out.append([off, 0, name, kind])
    out = [f for f in out if f[2] != "tlocal"] + [[0x2a4 + 4 * k, 0, "tlocal[%d]" % k, "i"] for k in range(32)]
    out.sort()
    for i, f in enumerate(out):
        f[1] = (out[i + 1][0] if i + 1 < len(out) else PLW) - f[0]
    return out


def show(fld, raw):
    if fld[3] == "f" and fld[1] % 4 == 0:
        return " ".join("%.4f" % v for v in struct.unpack("<%df" % (fld[1] // 4), raw))
    if fld[1] in (1, 2, 4):
        return str(int.from_bytes(raw, "little", signed=True))
    return raw.hex()


BASES = None


def same(fld, a, b):
    if fld[3] == "p":
        return True
    if a == b:
        return True
    if fld[1] == 4 and BASES:       # a pointer kept in an integer (tlocal): the same work in both
        x, y = struct.unpack("<II", a + b)
        for (pb, nb, n) in BASES:
            if 0 <= x - pb < n and x - pb == y - nb:
                return True
        if 0x400000 <= x < 0x4000000 and 0x0C000000 <= y < 0x0D000000:
            return True     # another pointer into the program's data (a table): can't be told apart
    if fld[3] == "f" and fld[1] % 4 == 0:
        va = struct.unpack("<%df" % (fld[1] // 4), a)
        vb = struct.unpack("<%df" % (fld[1] // 4), b)
        return all(abs(x - y) <= 1e-3 * max(1.0, abs(x)) or x != x for x, y in zip(va, vb))
    return False


IGNORE = {"b", "route_no", "play_time", "score", "old_score", "old_score_hi", "unk_55d", "beat_flag", "cpu_level_pts"}


def compare(out, limit=60, sub="port", quiet=False):
    """Each field's first difference, by player, in frame order."""
    a, b = frames(os.path.join(out, sub, "trace.bin")), frames(os.path.join(out, "fc", "trace.bin"))
    if not quiet:
        print("frames: port %d, fc %d" % (len(a), len(b)))
    global BASES
    if BASES is None:
        ps, ns = port_syms(), naomi_syms()
        BASES = [(ps["pl_work"], ns["pl_work"], PLW * 6), (ps["work_buff"], ns["work_buff"], 0x1D0 * 256)]
    fl = [f for f in fields() if f[2] not in IGNORE]
    seen, report = set(), []
    base = set()        # what already differs as the trace starts (values picked before both were in step)
    for i in range(min(len(a), len(b))):
        fa = struct.unpack_from("<h", a[i]["vs_sys_work"], 0x10)[0]
        fb = struct.unpack_from("<h", b[i]["vs_sys_work"], 0x10)[0]
        if fa != fb and "fight_frame" not in seen:
            seen.add("fight_frame")
            report.append("t%d fight_frame port %d fc %d" % (i, fa, fb))
        for p in range(6 + (256 if "work_buff" in a[i] else 0)):
            if p < 6:
                pa, pb = a[i]["pl_work"][p * PLW:(p + 1) * PLW], b[i]["pl_work"][p * PLW:(p + 1) * PLW]
            else:
                q = (p - 6) * 0x1D0
                pa, pb = a[i]["work_buff"][q:q + 0x1D0] + bytes(PLW), b[i]["work_buff"][q:q + 0x1D0] + bytes(PLW)
            if pa[0] == 0 and pb[0] == 0:
                continue
            for f in fl:
                x, y = pa[f[0]:f[0] + f[1]], pb[f[0]:f[0] + f[1]]
                if (p, f[2]) not in seen and not same(f, x, y):
                    seen.add((p, f[2]))
                    if i == 0:
                        base.add((p, f[2]))
                        continue
                    who = "pl%d" % p if p < 6 else "w%d(ef %04x)" % (p - 6, struct.unpack_from("<H", pb, 0x26)[0])
                    report.append("t%d ff %d %s %s: port %s fc %s" % (i, fa, who, f[2], show(f, x), show(f, y)))
    if not quiet:
        print(chr(10).join(report[:limit]) if report else "no differences in %d frames" % min(len(a), len(b)))
    return report


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    script, out = os.path.abspath(args[0]), os.path.abspath(args[1])
    os.makedirs(out, exist_ok=True)
    if "--compare-only" not in sys.argv:
        if "--fc-only" not in sys.argv:
            run_port(script, out)
        if "--port-only" not in sys.argv:
            run_fc(script, out)
    if os.path.exists(os.path.join(out, "port", "trace.bin")) and os.path.exists(os.path.join(out, "fc", "trace.bin")):
        compare(out)


if __name__ == "__main__":
    main()
