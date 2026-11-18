"""The start of a hunt script: two coins, both players in, each side's team poked in at the select, and on
to the fight. Prints it; the moves follow it.

  python -I tools/hunt/menu.py P1A,P1B,P1C P2A,P2B,P2C   (character numbers, as build/fighter_names.txt)
"""
import sys

PLW = 0x5A4


def select(team1, team2):
    s = ["# power on to the attract", "wait sys_work+0x14 4 64", "frames 60",
         "coin", "frames 20", "coin", "frames 20",
         "hold 4 start -", "frames 30", "hold 4 - start",
         "# the select: the opening over",
         "frames 20"]
    flags = [0, 0]
    for n in range(3):
        for side, team in ((0, team1), (1, team2)):
            w = (n * 2 + side) * PLW
            s += ["wait sel_w+0x%x 1 %d" % (0x34 + side, n + 1), "frames 10",
                  "poke pl_work+0x%x 1 %d" % (w + 0x52C, team[n]), "poke pl_work+0x%x 1 %d" % (w + 1, team[n])]
            keys = ["lp", "-"] if side == 0 else ["-", "lp"]
            flags[side] |= 1 << n
            s += ["until sel_w+0x%x 1 0x%x %s %s" % (2 + side, flags[side], keys[0], keys[1])]
            flags[side] |= 16 << n
            s += ["until sel_w+0x%x 1 0x%x %s %s" % (2 + side, flags[side], keys[0], keys[1])]
    s += ["# speed, stage and the rest: tap until the fight is on",
          "until vs_sys_work+0 1 2 lp lp",
          "# the entrances: the same random numbers and counters from here in both, and no keys till the fight", "poke nl_random_next 4 0x2468ace1",
          "poke rnd_hi 2 0x5501", "poke sys_work+0x1c 4 0x10000", "poke sys_work+0x80 1 0", "poke sys_work+0x20 4 0",
          "poke sys_work+0x10 4 0", "poke sys_work+0x90 4 0", "wait vs_sys_work+0 1 4", "frames 40", "trace on"]
    return "\n".join(s) + "\n"


if __name__ == "__main__":
    t1 = [int(x, 0) for x in sys.argv[1].split(",")]
    t2 = [int(x, 0) for x in sys.argv[2].split(",")]
    sys.stdout.write(select(t1, t2))
