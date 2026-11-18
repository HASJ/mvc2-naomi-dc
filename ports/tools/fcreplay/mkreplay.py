"""Write a Flycast Dojo replay (.flyreplay) of made-up input, for testing the runner and statcheck: random
characters at the select, then random play with motions, hyper combos, assists and dashes. Fights run on
through the next selects.

  python -I tools/fcreplay/mkreplay.py OUT.flyreplay [FRAMES] [SEED]
"""
import os, random, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from flyreplay import DC_BITS  # noqa: E402

U, D, L, R = DC_BITS["up"], DC_BITS["down"], DC_BITS["left"], DC_BITS["right"]
BUTTONS = [DC_BITS[k] for k in ("lp", "hp", "lk", "hk", "a1", "a2")]
MOTIONS = [[D, D | R, R], [D, D | L, L], [R, D, D | R], [L, D, D | L], [R, D | R, D, D | L, L], [L, D | L, D, D | R, R]]


def msg(cmd, body, seq=0):
    return struct.pack("<III", len(body), seq, cmd) + body


def string(s):
    b = s.encode() + b"\0"
    return struct.pack("<I", len(b)) + b


def player(rnd, frames):
    """One side's keys, frame by frame: in bursts, so the select moves and moves come out."""
    out = []
    while len(out) < frames:
        r = rnd.random()
        if r < 0.25:
            m, b = rnd.choice(MOTIONS), rnd.choice(BUTTONS)
            b2 = rnd.choice(BUTTONS) if rnd.random() < 0.3 else 0
            for k in m:
                out += [k] * rnd.randint(1, 3)
            out += [m[-1] | b | b2] * rnd.randint(2, 4)
        elif r < 0.35:
            out += [rnd.choice(BUTTONS) | rnd.choice(BUTTONS)] * rnd.randint(2, 4)     # dash, assists, team moves
        elif r < 0.55:
            out += [rnd.choice([U, D, L, R, U | L, U | R, D | L, D | R])] * rnd.randint(2, 40)
        elif r < 0.75:
            out += [rnd.choice(BUTTONS) | rnd.choice([0, D, L, R, U])] * rnd.randint(1, 4)
        elif r < 0.78:
            out += [DC_BITS["start"]] * 3
        else:
            out += [0] * rnd.randint(1, 30)
    return out[:frames]


def main():
    path = sys.argv[1]
    frames = int(sys.argv[2]) if len(sys.argv) > 2 else 20000
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    rnd = random.Random(seed)
    p1, p2 = player(rnd, frames), player(rnd, frames)
    start = struct.pack("<I", 2) + b"".join(string(s) for s in ("mvsc2", "p1", "p2", "", "")) + \
        struct.pack("<III", 0, 0, 1) + string("") + string("")
    data = msg(3, start)
    for at in range(0, frames, 120):
        body = struct.pack("<I", 28)
        for n in range(at, min(at + 120, frames)):
            body += struct.pack("<I", n) + struct.pack("<I", p1[n]) + bytes(8) + struct.pack("<I", p2[n]) + bytes(8)
        data += msg(6, body)
    open(path, "wb").write(data)


if __name__ == "__main__":
    main()
