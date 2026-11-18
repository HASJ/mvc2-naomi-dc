"""Write src/sel_move_name.c: the name of each character's special moves, which the select screen's
assist pillars show.

The pillars ask the game which move each assist type does (the character's TBL_SP_ASSIST routine
sets pl_sp_type) and look the move up here, so a changed assist shows its new move with no edit.
Run this again after adding or renaming a move in include/plXX_def.h:

    python tools/sel_move_names.py          # write src/sel_move_name.c
    python tools/sel_move_names.py --check  # list the assists' moves with no name; fail if the file is stale

A name comes from the move's comment in plXX_def.h, cleaned up: words in brackets and anything
after a colon or comma go, "X in the air" becomes "Air X", normals become "Standing HP" and the like. NAMES below overrides it where the
comment describes the move instead of naming it.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "src" / "sel_move_name.c"
CHARS = 0x3B

# names for moves whose comment is a description, or empty
NAMES = {
    "pl05_sp_formation_b_shot": "Formation B",
    "pl08_sp_leap": "Leaping Slash",
    "pl0a_sp_leap": "Power Leap",
    "pl0a_sp_team": "Team Hyper",
    "pl0e_sp_gamma": "Launcher",
    "pl10_sp_throw": "Throw",
    "pl10_sp_kick": "Kick",
    "pl10_sp_fire": "Gustaff Fire",
    "pl11_sp_heal": "Heal",
    "pl13_sp_throw": "Carry Throw",
    "pl13_sp_power": "Power Up",
    "pl14_sp_peach": "Peach",
    "pl15_sp_bite": "Bite",
    "pl18_sp_punch": "Dark Fist",
    "pl18_sp_thunder": "Dark Thunder",
    "pl18_sp_ball": "Dark Balls",
    "pl18_sp_pillar": "Dark Pillars",
    "pl18_sp_sweep": "Sweeping Blow",
    "pl18_sp_rush": "Charging Rush",
    "pl19_sp_bubble": "Bubbles",
    "pl19_sp_storm": "Shot Storm",
    "pl19_sp_beam": "Beam",
    "pl19_sp_blast": "Side Blast",
    "pl1a_sp_storm": "Shot Storm",
    "pl1a_sp_stomp": "Stomp",
    "pl1a_sp_jump": "Jumping Fire",
    "pl1a_sp_wave": "Floor Wave",
    "pl1a_sp_spray": "Shot Spray",
    "pl1a_sp_blast": "Blast",
    "pl1a_sp_throw": "Leaping Throw",
    "pl1a_sp_flame": "Dark Flames",
    "pl1c_sp_buster": "Mega Buster",
    "pl2b_sp_06": "Berserker Claw",
    "pl30_sp_counter_a": "Variable Counter",
    "pl30_sp_counter_g": "Variable Counter",
    "pl31_sp_swords": "Dancing Sword",
    "pl35_sp_gamma": "Launcher",
}

# moves never shown: changes of form, wins and losses
SKIP = {"pl18_sp_change", "pl19_sp_change", "pl1a_sp_win", "pl1a_sp_win_voice", "pl1a_sp_sink", "pl1a_sp_fade"}

NORMAL = re.compile(r"^(standing|crouching|jumping) (light|medium|heavy) (punch|kick)\b", re.I)
DEFINE = re.compile(r"^#define\s+(pl([0-9a-f]{2})_sp_\w+)\s+(\S+)\s*(?:/\*\s*(.*?)\s*\*/)?")


def clean(comment: str) -> str | None:
    m = NORMAL.match(comment)
    if m:
        return "%s %s%s" % (m.group(1).capitalize(), m.group(2)[0].upper(), "P" if m.group(3).lower() == "punch" else "K")
    if re.fullmatch(r"(the )?taunt", comment, re.I):
        return "Taunt"
    if re.fullmatch(r"(the )?snapback", comment, re.I):
        return "Snapback"
    name = comment.split(":")[0].split(",")[0]
    name = re.sub(r"\s*\([^)]*\)", "", name).strip()
    name = re.sub(r"\s+(with both \w+|at its strongest)$", "", name)
    m = re.fullmatch(r"(.+?),? in the air", name)
    if m:
        name = "Air " + m.group(1)
    if not name or not name[0].isupper():
        return None
    return name


def read_moves() -> dict[int, list[tuple[str, str, int]]]:
    moves: dict[int, list[tuple[str, str, int]]] = {}
    for ch in range(CHARS):
        path = ROOT / "include" / ("pl%02x_def.h" % ch)
        rows = moves.setdefault(ch, [])
        for line in path.read_text(encoding="latin-1").splitlines():
            m = DEFINE.match(line)
            if not m or m.group(1) in SKIP:
                continue
            macro, comment = m.group(1), m.group(4) or ""
            name = NAMES.get(macro) or clean(comment)
            if name:
                rows.append((macro, name, int(m.group(3), 0)))
    return moves


def c_string(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def render(moves: dict[int, list[tuple[str, str, int]]]) -> str:
    out = [
        "/* The name of each character's special moves (its pl_sp_type), which the select screen's assist",
        "   pillars show. Written by tools/sel_move_names.py from the comments of include/plXX_def.h: run it",
        "   again after adding, renumbering or renaming a move. (The headers can't all be included at once.) */",
        '#include "player.h"',
        "",
        "typedef struct SEL_MOVE_NAME {",
        "\tSint16\tsp;		/* the move's pl_sp_type, -1 at the end */",
        "\tconst char\t*name;",
        "} SEL_MOVE_NAME;",
        "",
    ]
    for ch in range(CHARS):
        out.append("static const SEL_MOVE_NAME sel_move_name_%02x[] = {" % ch)
        for macro, name, value in moves[ch]:
            out.append("\t{ %d, %s },\t/* %s */" % (value, c_string(name), macro))
        out.append("\t{ -1, 0 }")
        out.append("} ;")
    out.append("")
    out.append("static const SEL_MOVE_NAME *const sel_move_name_tbl[%d] = {" % CHARS)
    for ch in range(0, CHARS, 6):
        out.append("\t" + " ".join("sel_move_name_%02x," % c for c in range(ch, min(ch + 6, CHARS))))
    out.append("} ;")
    out += [
        "",
        "/****************************************************************/",
        "/*			The name of character type's move sp, or 0			*/",
        "/****************************************************************/",
        "const char *sel_move_name( Sint32 type, Sint32 sp )",
        "{",
        "\tconst SEL_MOVE_NAME *n ;",
        "",
        "\tif( type < 0 || type >= %d ) return 0 ;" % CHARS,
        "\tfor( n = sel_move_name_tbl[type] ; n->sp >= 0 ; n ++ ){",
        "\t\tif( n->sp == sp ) return n->name ;",
        "\t}",
        "\treturn 0 ;",
        "}",
        "",
        "/*==============================================================*/",
        "/*	End of File.												*/",
        "/*==============================================================*/",
        '/* "sel_move_name.c" End of File	*/',
        "",
    ]
    return "\n".join(out)


def assist_moves() -> dict[int, list[str]]:
    """Each character's assist routine's pl_sp_type macros, read from src/s_plXX.c."""
    found: dict[int, list[str]] = {}
    for ch in range(CHARS):
        path = ROOT / "src" / ("s_pl%02x.c" % ch)
        src = path.read_text(encoding="latin-1") if path.exists() else ""
        m = re.search(r"void\s+pl%02x_sp_assist\s*\([^)]*\)\s*\{" % ch, src)
        if not m:
            continue
        depth, i = 1, m.end()
        while depth and i < len(src):
            depth += {"{": 1, "}": -1}.get(src[i], 0)
            i += 1
        found[ch] = sorted(set(re.findall(r"pl%02x_sp_\w+" % ch, src[m.end():i])))
    return found


def main() -> int:
    moves = read_moves()
    if "--check" in sys.argv:
        named = {macro for rows in moves.values() for macro, _, _ in rows}
        missing = [(ch, macro) for ch, macros in assist_moves().items() for macro in macros if macro not in named]
        for ch, macro in missing:
            print("no name: %02x %s" % (ch, macro))
        stale = not OUT.exists() or OUT.read_text(encoding="latin-1") != render(moves)
        if stale:
            print("%s is out of date: run tools/sel_move_names.py" % OUT.relative_to(ROOT))
        return 1 if missing or stale else 0
    OUT.write_text(render(moves), encoding="latin-1", newline="\n")
    print("wrote %s (%d moves)" % (OUT.relative_to(ROOT), sum(len(r) for r in moves.values())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
