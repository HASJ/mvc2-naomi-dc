"""A battery of moves for one character, as a hunt script: each move from the same refilled state (vitals,
gauge, clock), with a fixed number of frames after it. Writes SCRIPT and SCRIPT.moves (each move's first
trace frame).

  python -I tools/hunt/battery.py SCRIPT P1TEAM P2TEAM SIDE [MOVE ...]
SIDE 0 or 1: the side that moves; the other stands. Moves are names from MOVES, or all of them.
"""
import json, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from menu import select  # noqa: E402

PLW = 0x5A4
GAP = 170

# f: forward, b: back, d: down, u: up; a step is "keys:frames"
MOTIONS = {
    "qcf": ["d:2", "d+f:2"], "qcb": ["d:2", "d+b:2"], "dp": ["f:2", "d:2", "d+f:1"],
    "rdp": ["b:2", "d:2", "d+b:1"], "hcf": ["b:2", "d+b:2", "d:2", "d+f:2"], "hcb": ["f:2", "d+f:2", "d:2", "d+b:2"],
}
MOVES = {}
for m in ("qcf", "qcb", "dp", "rdp", "hcf", "hcb"):
    for b in ("lp", "hp", "lk", "hk"):
        MOVES["%s+%s" % (m, b)] = MOTIONS[m] + ["f+%s:3" % b if m in ("qcf", "hcf", "dp") else
                                               "b+%s:3" % b if m in ("qcb", "hcb", "rdp") else b]
for m in ("qcf", "qcb", "dp", "rdp", "hcb"):
    for bb in ("lp+hp", "lk+hk"):
        last = "f" if m in ("qcf", "dp") else "b"
        MOVES["%s+%s" % (m, bb)] = MOTIONS[m] + ["%s+%s:3" % (last, bb)]
MOVES.update({
    "dash": ["lp+hp:3"], "backdash": ["b:2", "-:2", "b:2"],
    "jump.dash": ["u:3", "-:10", "lp+hp:3"], "jump.dash.back": ["u:3", "-:10", "b+lp+hp:3"],
    "jump.dash.up": ["u:3", "-:10", "u+lp+hp:3"], "jump.dash.down": ["u:3", "-:10", "d+lp+hp:3"],
    "jump.qcf+lp": ["u:3", "-:8"] + MOTIONS["qcf"] + ["f+lp:3"],
    "jump.qcf+hk": ["u:3", "-:8"] + MOTIONS["qcf"] + ["f+hk:3"],
    "jump.qcb+hp": ["u:3", "-:8"] + MOTIONS["qcb"] + ["b+hp:3"],
    "sjump.qcf+lp+hp": ["d:2", "u:3", "-:14"] + MOTIONS["qcf"] + ["f+lp+hp:3"],
    "sjump.qcf+lk+hk": ["d:2", "u:3", "-:14"] + MOTIONS["qcf"] + ["f+lk+hk:3"],
    "fly": ["qcb+lk+hk"], "a1": ["a1:3"], "a2": ["a2:3"], "team": ["a1+a2:3"],
    "charge.b.f+lp": ["b:50", "f+lp:3"], "charge.b.f+hk": ["b:50", "f+hk:3"], "charge.d.u+hk": ["d:50", "u+hk:3"],
    "mash.lp": ["lp:2", "-:2"] * 8, "launch": ["d+hp:3"], "throw": ["f:30", "f+hp:3"],
})
MOVES["fly"] = MOTIONS["qcb"] + ["b+lk+hk:3"]


def keys(step, side):
    """A step's keys for one side: forward and back by the side (side 0 starts on the left)."""
    k = []
    for t in step.split("+"):
        if t == "f":
            k.append("right" if side == 0 else "left")
        elif t == "b":
            k.append("left" if side == 0 else "right")
        elif t in ("u", "d"):
            k.append("up" if t == "u" else "down")
        elif t != "-":
            k.append(t)
    return "+".join(k) or "-"


def refill():
    s = []
    for p in range(6):
        s += ["poke pl_work+0x%x 2 144" % (p * PLW + 0x420), "poke pl_work+0x%x 2 144" % (p * PLW + 0x424)]
    s += ["poke vs_sys_work+0x5a 1 5", "poke vs_sys_work+0x5b 1 5", "poke vs_sys_work+0x40 1 99"]
    return s


def battery(team1, team2, side, moves):
    lines = select(team1, team2).splitlines() + ["frames 20"]
    t, marks = 20, []
    for name in moves:
        lines += ["# " + name] + refill()
        marks.append((name, t))
        for step in MOVES[name]:
            k, n = step.rsplit(":", 1) if ":" in step else (step, "3")
            n = int(n)
            ks = keys(k, side)
            lines.append("hold %d %s %s" % (n, ks if side == 0 else "-", ks if side == 1 else "-"))
            t += n
        lines.append("frames %d" % GAP)
        t += GAP
    lines.append("quit")
    return "\n".join(lines) + "\n", marks


def main():
    path, t1, t2, side = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
    moves = sys.argv[5:] or list(MOVES)
    text, marks = battery([int(x, 0) for x in t1.split(",")], [int(x, 0) for x in t2.split(",")], side, moves)
    open(path, "w").write(text)
    json.dump(marks, open(path + ".moves", "w"))
    print("%d moves, %d frames" % (len(marks), marks[-1][1] + GAP))


if __name__ == "__main__":
    main()
