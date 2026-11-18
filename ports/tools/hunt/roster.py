"""Sweep characters: each one's battery of moves in Flycast and in the port (and, if given, a second port
build), each pass from the move after the last one that differed. Results in OUTDIR/results.jsonl.

  python -I tools/hunt/roster.py OUTDIR SIDE CHAR[,CHAR...] [--fc=dir,dir] [--stub=exe]
"""
import json, os, shutil, struct, sys, threading
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import battery  # noqa: E402
import hunt  # noqa: E402

SWEEP = ["qcf+lp", "qcf+hp", "qcf+lk", "qcf+hk", "dp+lp", "dp+hp", "dp+lk", "qcb+lp", "qcb+hp", "qcb+lk",
         "qcb+hk", "rdp+lp", "hcb+lp", "hcb+hk", "hcf+hp", "qcf+lp+hp", "qcf+lk+hk", "qcb+lp+hp", "qcb+lk+hk",
         "dp+lp+hp", "rdp+lp+hp", "hcb+lp+hp", "dash", "backdash", "jump.dash", "jump.dash.up", "jump.dash.down",
         "jump.dash.back", "jump.qcf+lp", "jump.qcf+hk", "jump.qcb+hp", "sjump.qcf+lp+hp", "sjump.qcf+lk+hk",
         "a1", "a2", "team", "charge.b.f+lp", "charge.d.u+hk", "mash.lp", "launch", "throw"]
NAMES = [l.strip() for l in open(os.path.join(hunt.DECOMP, "build", "fighter_names.txt"))]
port_lock = threading.Lock()


MINOR = ("com_w", "snapback_w", "vcounter_w", "dash_w", "sjump_w", "pl_", "pad_sw", "side_sw")


def field(line):
    return line.split()[4].rstrip(":")


def diffs(out, sub):
    """Against Flycast for one port trace: the differences past the command readers' state, and those in it
    (reported, but a pass goes on past them), each [(t, line)]."""
    rep = [(int(r.split()[0][1:]), r) for r in hunt.compare(out, sub=sub, quiet=True)]
    return [x for x in rep if not field(x[1]).startswith(MINOR)], [x for x in rep if field(x[1]).startswith(MINOR)]


def move_at(marks, t):
    name = marks[0][0]
    for n, start in marks:
        if start <= t + 1:
            name = n
    return name


def one_pass(char, side, moves, out, fc, stub, log):
    os.makedirs(out, exist_ok=True)
    team = [char, 0, 1]
    other = [2, 39, 27]
    script = os.path.join(out, "script.txt")
    text, marks = battery.battery(team if side == 0 else other, other if side == 0 else team, side, moves)
    open(script, "w").write(text)
    try:
        hunt.run_port(script, out)
        if stub:
            hunt.run_port(script, out, exe=stub, sub="stub")
        hunt.run_fc(script, out, fc)
        res = compare(out, marks, stub)
    finally:
        for sub in ("port", "stub", "fc"):
            t = os.path.join(out, sub, "trace.bin")
            if os.path.exists(t):
                os.remove(t)        # about 70 MB each; the script makes them again
    res.update({"char": NAMES[char], "side": side, "moves": moves, "dir": out})
    with port_lock:
        log.write(json.dumps(res) + "\n")
        log.flush()
    return res, marks


def compare(out, marks, stub):
    res = {}
    for sub in ["port"] + (["stub"] if stub else []):
        d, minor = diffs(out, sub)
        if d:
            t = d[0][0]
            res[sub] = {"t": t, "move": move_at(marks, t), "first": [x[1] for x in d[:25]]}
        else:
            res[sub] = None
        if minor:
            res[sub + "_minor"] = {"t": minor[0][0], "move": move_at(marks, minor[0][0]),
                                   "first": [x[1] for x in minor[:12]]}
    return res


def sweep_char(char, side, root, fc, stub, log):
    moves = list(SWEEP)
    n = 0
    while moves:
        out = os.path.join(root, "%s_%d_%d" % (NAMES[char], side, n))
        res, marks = one_pass(char, side, moves, out, fc, stub, log)
        hit = res["port"]
        print("%s side %d pass %d: port %s, stub %s" % (NAMES[char], side, n, hit and hit["move"],
                                                         res.get("stub") and res["stub"]["move"]), flush=True)
        if not hit:
            break
        moves = moves[moves.index(hit["move"]) + 1:]
        n += 1


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--"))
    root, side = os.path.abspath(args[0]), int(args[1])
    chars = [int(c, 0) for c in args[2].split(",")]
    fcs = [os.path.abspath(f) for f in opts.get("fc", os.path.join(hunt.ROOT, "build", "fc")).split(",")]
    stub = opts.get("stub")
    os.makedirs(root, exist_ok=True)
    log = open(os.path.join(root, "results.jsonl"), "a")
    free = list(fcs)
    lock = threading.Lock()

    def job(c):
        with lock:
            fc = free.pop()
        try:
            sweep_char(c, side, root, fc, stub, log)
        except Exception as e:
            print("%s: %r" % (NAMES[c], e), flush=True)
        finally:
            with lock:
                free.append(fc)

    with ThreadPoolExecutor(len(fcs)) as ex:
        list(ex.map(job, chars))


if __name__ == "__main__":
    main()
