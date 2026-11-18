"""Computer against computer: both teams run by the game's computer from the fight's start, vitals and
clock refilled every 600 frames. Writes a hunt script.

  python -I tools/hunt/cpu.py SCRIPT P1TEAM P2TEAM FRAMES
"""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import battery  # noqa: E402
import menu  # noqa: E402


def cpu_fight(team1, team2, frames):
    s = menu.select(team1, team2).splitlines()
    s += ["poke pl_work+0x%x 1 1" % (p * battery.PLW + 0x525) for p in range(6)]
    for _ in range(frames // 600):
        s += battery.refill() + ["frames 600"]
    return "\n".join(s + ["quit"]) + "\n"


if __name__ == "__main__":
    t1, t2 = ([int(x, 0) for x in a.split(",")] for a in sys.argv[2:4])
    open(sys.argv[1], "w").write(cpu_fight(t1, t2, int(sys.argv[4])))
