"""Computer-against-computer fights in the port alone, looking for a computer player that does nothing:
the point player standing still (act_st 0, no stick or buttons from the computer) for IDLE frames or more.

  python -I tools/hunt/idle.py OUTDIR FRAMES SEED [EXE]
"""
import os, random, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cpu  # noqa: E402
import hunt  # noqa: E402

IDLE = 300
PLW = 0x5A4


def scan(trace):
    a = hunt.frames(trace)
    found = []
    for p in range(6):
        run = 0
        for i, f in enumerate(a):
            w = f["pl_work"][p * PLW:(p + 1) * PLW]
            point = w[0] and w[0x411] == 0
            sw = struct.unpack_from("<H", w, 0x4dc)[0]
            if point and w[0x1d0] == 0 and sw == 0 and w[0x525]:
                run += 1
                if run == IDLE:
                    found.append((p, w[1], i - IDLE + 1, w[0x43c], w[0x43d], w[0x440]))
            else:
                if run >= IDLE:
                    found[-1] = found[-1] + (run,)
                run = 0
    return found


def main():
    out, frames, seed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    exe = sys.argv[4] if len(sys.argv) > 4 else None
    names = [l.strip() for l in open(os.path.join(hunt.DECOMP, "build", "fighter_names.txt"))]
    rnd = random.Random(seed)
    chars = [c for c in range(59) if c not in (24, 25, 26)]
    t1, t2 = rnd.sample(chars, 3), rnd.sample(chars, 3)
    os.makedirs(out, exist_ok=True)
    script = os.path.join(out, "script.txt")
    open(script, "w").write(cpu.cpu_fight(t1, t2, frames))
    hunt.run_port(script, out, exe=exe)
    trace = os.path.join(out, "port", "trace.bin")
    print("teams", [names[c] for c in t1], [names[c] for c in t2])
    for f in scan(trace):
        print("  idle: pl%d %s from t%d (cpu_routine %d.%d cmd %d)%s" % (
            f[0], names[f[1]], f[2], f[3], f[4], f[5], " for %d frames" % f[6] if len(f) > 6 else " to the end"))
    os.remove(trace)


if __name__ == "__main__":
    main()
