"""Move catalog: groups the many animation states and hit-data rows of a character into named *moves*.

Capcom's code splits one special move across many animation states (Warp = 3 states, Vajra = ~20 including the
shadow clones), shares one animation between the Light and Heavy version, and runs projectiles as separate
effect objects with their own animations. None of that is visible from `char_XX.c` alone, so the catalog is
derived from the decompiled game logic:

* `include/plXX_def.h`   names (`plXX_sp_*` move ids, `plXX_at_*` hit-data ids)
* `src/s_plXX.c`         `pl_sp_tbl[]` (move id -> routine), `com_*[]` (inputs), the routines themselves:
                         `atck_no = ...`, `pl_char_set( pl_w, Group, n )`, `shlXX_YY_set(...)` projectile spawns
* `src/shlXX_YY.c`, `src/plsXX_YY.c`  the projectile/effect routines and the animations they play

Corrections live in `tools/move_editor/move_overrides/char_XX.json` (see `load_overrides`). The derivation is
best-effort: whatever cannot be evaluated is skipped, never guessed.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Set, Tuple

from .constants import ANIM_GROUP_MAP, ANIM_GROUP_SYM_TO_ID, ATCK_NAME_TO_ID, STANDARD_ATCK_NAMES
from .csrc import (
    CFunc,
    CTable,
    Event,
    FunctionWalker,
    Unknown,
    Ver,
    c_eval,
    parse_functions,
    parse_tables,
    referenced_functions,
    split_top_level,
    strip_comments,
)

OVERRIDE_DIR = Path(__file__).resolve().parent / "move_overrides"

KEY_TO_DIGIT = {
    "KEY_UP": 8,
    "KEY_DOWN": 2,
    "KEY_FORWARD": 6,
    "KEY_BACK": 4,
}


# ---------------------------------------------------------------------------
# Data model
# ---------------------------------------------------------------------------


@dataclass
class Version:
    key: str  # "LP" "HP" "LK" "HK" "L" "H" "*"
    label: str
    combos: Tuple[Ver, ...]


@dataclass
class StateInfo:
    group: int
    anim: int
    label: str = ""
    funcs: List[str] = field(default_factory=list)
    versions: Set[str] = field(default_factory=set)
    hits: Dict[str, List[int]] = field(default_factory=dict)  # version key -> hit-data ids (primary first)
    effect: bool = False  # played by a projectile / effect object, not by the fighter
    tags: Tuple[Tuple[str, int], ...] = ()
    custom_label: bool = False

    @property
    def ref(self) -> Tuple[int, int]:
        return (self.group, self.anim)


@dataclass
class HitInfo:
    atck_no: int
    versions: Set[str] = field(default_factory=set)
    label: str = ""
    sym: str = ""
    note: str = ""
    variable_counter: bool = False
    effect: bool = False


@dataclass
class CodeTimer:
    func: str
    line: int
    field_name: str
    values: Dict[str, int]  # version key -> value
    text: str = ""


@dataclass
class MoveEntry:
    key: str  # root routine name, e.g. "sp_warp"
    name: str
    command: str = ""
    kind: str = "Special"  # Special | Hyper | Other
    sp_types: List[int] = field(default_factory=list)
    versions: List[Version] = field(default_factory=list)
    states: List[StateInfo] = field(default_factory=list)
    hits: List[HitInfo] = field(default_factory=list)
    timers: List[CodeTimer] = field(default_factory=list)
    notes: List[str] = field(default_factory=list)

    def version(self, key: str) -> Optional[Version]:
        return next((v for v in self.versions if v.key == key), None)

    def states_for(self, vkey: str) -> List[StateInfo]:
        return [s for s in self.states if not s.versions or vkey in s.versions or vkey == "*"]

    def hits_for(self, vkey: str) -> List[HitInfo]:
        return [h for h in self.hits if not h.versions or vkey in h.versions or vkey == "*"]

    def primary_hit(self, vkey: str) -> Optional[int]:
        """The move's own main hit-data row for this version (first non-projectile, non-variable-counter)."""
        cands = self.hits_for(vkey)
        for pred in (lambda h: not h.effect and not h.variable_counter, lambda h: not h.variable_counter, lambda h: True):
            for h in cands:
                if pred(h):
                    return h.atck_no
        return None

    def dropdown_label(self) -> str:
        cmd = f"  [{self.command}]" if self.command else ""
        return f"{self.kind}: {self.name}{cmd}"


@dataclass
class CharCatalog:
    char_id: int
    moves: List[MoveEntry] = field(default_factory=list)
    hit_names: Dict[int, str] = field(default_factory=dict)  # atck_no -> def.h symbol
    hit_notes: Dict[int, str] = field(default_factory=dict)  # atck_no -> def.h comment
    overrides_used: bool = False

    def moves_for_state(self, group: int, anim: int) -> List[MoveEntry]:
        return [m for m in self.moves if any(s.ref == (group, anim) for s in m.states)]

    def moves_for_hit(self, atck_no: int) -> List[MoveEntry]:
        return [m for m in self.moves if any(h.atck_no == atck_no for h in m.hits)]

    def state_label(self, group: int, anim: int) -> str:
        for m in self.moves_for_state(group, anim):
            for s in m.states:
                if s.ref == (group, anim) and s.label:
                    return s.label
        return ""

    def state_info(self, group: int, anim: int) -> List[Tuple[MoveEntry, StateInfo]]:
        out: List[Tuple[MoveEntry, StateInfo]] = []
        for m in self.moves:
            for s in m.states:
                if s.ref == (group, anim):
                    out.append((m, s))
        return out

    def hit_label(self, atck_no: int) -> str:
        for m in self.moves_for_hit(atck_no):
            for h in m.hits:
                if h.atck_no == atck_no and h.label:
                    return h.label
        return ""

    def move_by_key(self, key: str) -> Optional[MoveEntry]:
        return next((m for m in self.moves if m.key == key), None)


# ---------------------------------------------------------------------------
# Naming helpers
# ---------------------------------------------------------------------------

_PHASE_WORDS = {
    "init": "start-up",
    "start": "start-up",
    "wait": "wait",
    "ride": "ride",
    "out": "vanish",
    "in": "reappear",
    "jump": "jump",
    "fall": "fall",
    "land": "landing",
    "dive": "dive",
    "dash": "dash",
    "slash": "slash",
    "move": "move",
    "hit": "on hit",
    "end": "end",
    "last": "last hit",
    "run": "run",
    "fly": "fly",
    "blink": "blink",
    "catch": "grab",
    "up": "rising",
    "down": "falling",
    "shot": "shot",
    "keep": "hold",
    "rush": "rush",
    "leap": "leap",
    "bomb": "bomb",
}


def humanize(name: str) -> str:
    return re.sub(r"\s+", " ", name.replace("_", " ")).strip()


def _title(name: str) -> str:
    h = humanize(name)
    return h[:1].upper() + h[1:] if h else h


def phase_label(func: str, move_key: str, effect_prefix: str = "") -> str:
    """`vajra_start` -> `start-up`, `shl05_05_up_leap` -> `rising leap`."""
    base = func
    if effect_prefix and base.startswith(effect_prefix):
        base = base[len(effect_prefix) :]
    stem = re.sub(r"^sp_", "", move_key)
    for strip in (stem + "_", stem):
        if base.startswith(strip) and base != strip:
            base = base[len(strip) :]
            break
    toks = [t for t in re.split(r"[_\s]+", base) if t]
    words: List[str] = []
    for t in toks:
        if t.isdigit():
            continue
        t2 = re.sub(r"\d+$", "", t)
        words.append(_PHASE_WORDS.get(t2, t2))
    return " ".join(w for w in words if w) or "state"


def short_comment(text: str, limit: int = 70) -> str:
    t = re.sub(r"\s+", " ", text).strip()
    t = re.split(r"[;(]", t)[0].strip()
    return (t[: limit - 1] + "…") if len(t) > limit else t


# ---------------------------------------------------------------------------
# Source parsing
# ---------------------------------------------------------------------------


@dataclass
class _Defs:
    values: Dict[str, int]
    sp_by_value: Dict[int, Tuple[str, str]]  # value -> (symbol, comment)
    at_by_value: Dict[int, Tuple[str, str]]


def _parse_defs(root: Path, char_id: int) -> _Defs:
    values: Dict[str, int] = dict(ATCK_NAME_TO_ID)
    values.update({sym: grp for sym, grp in ANIM_GROUP_SYM_TO_ID.items()})
    sp: Dict[int, Tuple[str, str]] = {}
    at: Dict[int, Tuple[str, str]] = {}
    p = root / "include" / f"pl{char_id:02x}_def.h"
    if p.exists():
        for line in p.read_text(encoding="utf-8", errors="replace").splitlines():
            m = re.match(r"^\s*#\s*define\s+(\w+)\s+(0x[0-9a-fA-F]+|\d+)\s*(?:/\*(.*?)\*/)?", line)
            if not m:
                continue
            sym, val, cmt = m.group(1), int(m.group(2), 0), (m.group(3) or "").strip()
            values[sym] = val
            if "_sp_" in sym:
                sp[val] = (sym, cmt)
            elif "_at_" in sym:
                at[val] = (sym, cmt)
    # keys / flags used by command arrays
    values.update({"KEY_UP": 0x2000, "KEY_DOWN": 0x1000, "KEY_FORWARD": 0x0800, "KEY_BACK": 0x0400})
    return _Defs(values, sp, at)


_COM_RE = re.compile(r"(?:static\s+)?Uint16\s+(com_\w+)\s*\[\s*\]\s*=\s*\{([^}]*)\}")
_BUTTON_NAMES = {
    "KEY_PUNCH": ("P", {0}),
    "KEY_KICK": ("K", {1}),
    "KEY_ATCK": ("P/K", {0, 1}),
    "KEY_LP": ("LP", {0}),
    "KEY_HP": ("HP", {0}),
    "KEY_LK": ("LK", {1}),
    "KEY_HK": ("HK", {1}),
    "KEY_LIGHT": ("LP/LK", {0, 1}),
}


def _parse_commands(stripped: str, defs: _Defs) -> Dict[int, Tuple[str, Set[int], bool, bool]]:
    """sp_type -> (notation, valid atck_pk values, is_hyper, is_air)."""
    out: Dict[int, Tuple[str, Set[int], bool, bool]] = {}
    for m in _COM_RE.finditer(stripped):
        items = [t.strip() for t in m.group(2).split(",") if t.strip()]
        if len(items) < 5:
            continue
        window, flags, spsym = items[0], items[1], items[2]
        sp_type = defs.values.get(spsym)
        if sp_type is None:
            try:
                sp_type = int(spsym, 0)
            except ValueError:
                continue
        keys = items[4:]
        digits: List[str] = []
        buttons = ""
        pks: Set[int] = {0, 1}
        for k in keys:
            parts = [p.strip() for p in k.split("|")]
            btn = [p for p in parts if p in _BUTTON_NAMES]
            dirs = [p for p in parts if p in KEY_TO_DIGIT]
            if btn:
                name, pk = _BUTTON_NAMES[btn[0]]
                buttons = name
                pks = set(pk)
                continue
            if dirs:
                s = set(dirs)
                d = None
                if s == {"KEY_DOWN", "KEY_FORWARD"}:
                    d = 3
                elif s == {"KEY_DOWN", "KEY_BACK"}:
                    d = 1
                elif s == {"KEY_UP", "KEY_FORWARD"}:
                    d = 9
                elif s == {"KEY_UP", "KEY_BACK"}:
                    d = 7
                elif len(s) == 1:
                    d = KEY_TO_DIGIT[dirs[0]]
                if d is not None:
                    digits.append(str(d))
        try:
            charge = int(window, 0) > 8
        except ValueError:
            charge = False
        if charge and digits:
            motion = "[" + digits[0] + "]" + "".join(digits[1:])
        else:
            motion = "".join(digits)
        if buttons == "P/K":
            buttons = "any button"
        notation = (f"{motion}+{buttons}" if motion and buttons else motion or buttons).strip()
        is_hyper = "COM_HYPER" in flags
        is_air = "COM_AIR" in flags
        if is_air and notation:
            notation = f"air {notation}"
        out.setdefault(sp_type, (notation, pks, is_hyper, is_air))
    return out


def _parse_sp_table(raw: str, stripped: str) -> Tuple[List[List[str]], List[str]]:
    """Rows of routine names indexed by pl_sp_type, plus the trailing comments of the first row."""
    tables = parse_tables(stripped, raw)
    tbl = tables.get("pl_sp_tbl") or tables.get("sp_atck_jmp")
    if tbl is None:
        m = re.search(r"\b(\w+)\s*(?:\[[^\]]*\]\s*)?\[\s*(?:\w+\s*->\s*)?pl_sp_type\s*\]", stripped)
        if m:
            tbl = tables.get(m.group(1))
    if tbl is None:
        return [], []
    cmts = tbl.entry_comments[0] if tbl.entry_comments and tbl.entry_comments[0] else []
    return tbl.rows, cmts


def _clean_table_comment(cmt: str) -> str:
    return re.sub(r"^\s*\d+\s*", "", cmt).strip()


# ---------------------------------------------------------------------------
# Move derivation
# ---------------------------------------------------------------------------


@dataclass
class _Src:
    path: Path
    stripped: str
    funcs: Dict[str, CFunc]
    tables: Dict[str, CTable]


_src_cache: Dict[Tuple[str, float], _Src] = {}


def _load_src(path: Path) -> Optional[_Src]:
    if not path.exists():
        return None
    key = (str(path), path.stat().st_mtime)
    hit = _src_cache.get(key)
    if hit is not None:
        return hit
    raw = path.read_text(encoding="utf-8", errors="replace")
    stripped = strip_comments(raw)
    src = _Src(path, stripped, parse_functions(stripped), parse_tables(stripped, raw))
    _src_cache[key] = src
    return src


@dataclass
class _Collected:
    events: List[Event] = field(default_factory=list)
    effect_events: List[Tuple[Event, str]] = field(default_factory=list)  # (event, effect prefix)
    funcs_seen: List[str] = field(default_factory=list)
    texts: Dict[str, str] = field(default_factory=dict)  # function -> body text (for version usage)


def _dfs_collect(
    roots: Sequence[str],
    src: _Src,
    defs: _Defs,
    combos: Sequence[Ver],
    extra: Optional[Dict[str, Sequence[int]]] = None,
    bindings: Optional[Dict[str, Sequence[int]]] = None,
    effect_prefix: str = "",
    depth: int = 0,
    visited_effects: Optional[Set[Tuple]] = None,
) -> _Collected:
    walker = FunctionWalker(defs.values, combos, extra)
    out = _Collected()
    visited: Set[str] = set()
    visited_effects = visited_effects if visited_effects is not None else set()

    def visit(name: str) -> None:
        if name in visited or name not in src.funcs:
            return
        visited.add(name)
        fn = src.funcs[name]
        out.funcs_seen.append(name)
        out.texts[name] = fn.text
        evs = walker.walk(fn)
        for ev in evs:
            if ev.kind == "spawn" and depth < 3:
                _resolve_spawn(ev, src, defs, out, depth, visited_effects)
            if effect_prefix:
                out.effect_events.append((ev, effect_prefix))
            else:
                out.events.append(ev)
        for child in referenced_functions(fn, src.funcs, src.tables, bindings):
            visit(child)

    for r in roots:
        visit(r)
    return out


def _resolve_spawn(
    ev: Event, src: _Src, defs: _Defs, out: _Collected, depth: int, visited: Set[Tuple]
) -> None:
    m = re.match(r"^((?:shl|pls)[0-9a-fA-F]{2}_[0-9a-fA-F]{2})", ev.name)
    if not m:
        return
    prefix = m.group(1)
    eff_src = _load_src(src.path.parent / f"{prefix}.c")
    if eff_src is None or ev.name not in eff_src.funcs:
        return
    key = (ev.name, ev.args, ev.ver)
    if key in visited:
        return
    visited.add(key)
    set_fn = eff_src.funcs[ev.name]
    text = set_fn.text
    # which `type` / `sub_type` values does this spawn create?
    types: Optional[List[int]] = None
    lits: List[int] = []
    unresolved = False
    for tm in re.finditer(r"->\s*type\s*=\s*([^;]+);", text):
        rhs = tm.group(1).strip()
        if re.fullmatch(r"0[xX][0-9a-fA-F]+|\d+", rhs):
            lits.append(int(rhs, 0))
        elif ev.args:
            lits.append(ev.args[0])
        else:
            unresolved = True
    if lits and not unresolved:
        types = sorted(set(lits))
    subs = sorted({int(x, 0) for x in re.findall(r"->\s*sub_type\s*=\s*(0[xX][0-9a-fA-F]+|\d+)\s*;", text)})
    extra: Dict[str, Sequence[int]] = {}
    bindings: Dict[str, Sequence[int]] = {}
    if types is not None:
        extra["type"] = types
        bindings["type"] = types
    if subs:
        extra["sub_type"] = subs
        bindings["sub_type"] = subs
    wf = re.search(r"work_func\s*=\s*(\w+)\s*;", text)
    if not wf:
        return
    sub = _dfs_collect(
        [wf.group(1)],
        eff_src,
        defs,
        [ev.ver],
        extra=extra,
        bindings=bindings,
        effect_prefix=prefix,
        depth=depth + 1,
        visited_effects=visited,
    )
    out.effect_events.extend(sub.effect_events)
    for fnm in sub.funcs_seen:
        out.funcs_seen.append(fnm)
    out.texts.update(sub.texts)


def _version_set(uses_level: bool, uses_pk: bool, pks: Set[int]) -> Tuple[List[Version], List[Ver]]:
    """The strengths this move can be performed with, and the (level, pk) combos to simulate."""
    pk_vals = sorted(pks) if pks else [0, 1]
    combos: List[Ver] = [(lv, pk) for lv in (0, 1) for pk in pk_vals]
    versions: List[Version] = []
    if not uses_level and not uses_pk:
        versions.append(Version("*", "All versions", tuple(combos)))
        return versions, combos
    pk_name = {0: "P", 1: "K"}
    if uses_pk and len(pk_vals) > 1:
        # punch and kick are different versions
        if uses_level:
            for pk in pk_vals:
                for lv in (0, 1):
                    key = ("H" if lv else "L") + pk_name[pk]
                    label = f"{'Heavy' if lv else 'Light'} {'Punch' if pk == 0 else 'Kick'} ({key})"
                    versions.append(Version(key, label, ((lv, pk),)))
        else:
            for pk in pk_vals:
                versions.append(Version(pk_name[pk], "Punch" if pk == 0 else "Kick", tuple(c for c in combos if c[1] == pk)))
        return versions, combos
    # strength only (or the button type is fixed by the command)
    for lv in (0, 1):
        cs = tuple(c for c in combos if c[0] == lv)
        if len(pk_vals) == 1:
            btn = ("H" if lv else "L") + pk_name[pk_vals[0]]
            label = f"{'Heavy' if lv else 'Light'} {'Punch' if pk_vals[0] == 0 else 'Kick'} ({btn})"
            versions.append(Version(btn, label, cs))
        else:
            versions.append(Version("H" if lv else "L", "Heavy (HP/HK)" if lv else "Light (LP/LK)", cs))
    return versions, combos


def _version_key_for_combo(versions: Sequence[Version], combo: Ver) -> str:
    for v in versions:
        if combo in v.combos:
            return v.key
    return "*"


def _derive_move(
    key: str,
    roots: Sequence[str],
    src: _Src,
    defs: _Defs,
    sp_types: List[int],
    command: Tuple[str, Set[int], bool, bool],
) -> MoveEntry:
    notation, pks, is_hyper, _is_air = command
    # first pass over all four strengths to see whether the move cares about the buttons at all
    probe_combos: List[Ver] = [(lv, pk) for lv in (0, 1) for pk in sorted(pks or {0, 1})]
    texts_probe = _dfs_collect(roots, src, defs, probe_combos)
    all_text = "\n".join(texts_probe.texts.values()) + "\n".join(t for t in texts_probe.texts.values())
    # include projectile code as well
    uses_level = "atck_level" in all_text
    uses_pk = "atck_pk" in all_text
    versions, combos = _version_set(uses_level, uses_pk, pks)
    col = texts_probe if combos == probe_combos else _dfs_collect(roots, src, defs, combos)

    move = MoveEntry(key=key, name="", command=notation, sp_types=list(sp_types), versions=versions)
    move.kind = "Hyper" if is_hyper else "Special"

    states: Dict[Tuple[int, int], StateInfo] = {}
    hit_infos: Dict[int, HitInfo] = {}
    timers: Dict[Tuple[str, int, str], CodeTimer] = {}
    vercode_lines: Dict[Tuple[str, int], str] = {}

    def process(events: List[Tuple[Event, bool, str]]) -> None:
        # per (func, version) hit ids assigned inside that function
        fn_atcks: Dict[Tuple[str, str, Ver], List[Tuple[int, int]]] = {}
        for ev, is_eff, pref in events:
            if ev.kind == "atck":
                fn_atcks.setdefault((pref, ev.func, ev.ver), []).append((ev.line, ev.value))
        carried: Dict[Tuple[str, Ver], int] = {}
        for ev, is_eff, pref in events:
            vkey = _version_key_for_combo(versions, ev.ver)
            if ev.kind == "atck":
                carried[(pref, ev.ver)] = ev.value
                hi = hit_infos.setdefault(ev.value, HitInfo(ev.value, effect=True))
                hi.versions.add(vkey)
                if not is_eff:
                    hi.effect = False  # also assigned by the fighter himself
            elif ev.kind == "char" and ev.group >= 0:
                if ev.obj not in ("pl_w", "ew", "pw", "w", "") and not is_eff:
                    continue
                ref = (ev.group, ev.value)
                st = states.get(ref)
                if st is None:
                    st = StateInfo(ev.group, ev.value, effect=is_eff)
                    states[ref] = st
                if ev.func not in st.funcs:
                    st.funcs.append(ev.func)
                if ev.tag and not st.tags:
                    st.tags = ev.tag
                st.versions.add(vkey)
                own = [v for _, v in fn_atcks.get((pref, ev.func, ev.ver), [])]
                if not own and (pref, ev.ver) in carried:
                    own = [carried[(pref, ev.ver)]]
                if own:
                    ordered = _order_hits(own, defs)
                    cur = st.hits.setdefault(vkey, [])
                    for h in ordered:
                        if h not in cur:
                            cur.append(h)
            elif ev.kind == "timer":
                k = (ev.func, ev.line, ev.name)
                t = timers.setdefault(k, CodeTimer(ev.func, ev.line, ev.name, {}, ev.text))
                t.values[vkey] = ev.value
            elif ev.kind == "vercode" and not is_eff:
                vercode_lines[(ev.func, ev.line)] = ev.text

    tagged: List[Tuple[Event, bool, str]] = [(e, False, "") for e in col.events]
    tagged += [(e, True, p) for e, p in col.effect_events]
    # keep fighter events first (in walk order), then projectile events (in the order they were spawned)
    process(tagged)

    for hid, hi in hit_infos.items():
        sym, cmt = defs.at_by_value.get(hid, ("", ""))
        hi.sym = sym
        hi.note = cmt
        hi.variable_counter = "_vc_" in sym or "variable counter" in cmt.lower()
        move.hits.append(hi)
    # primary hit first
    move.hits.sort(key=lambda h: (h.variable_counter, h.effect))
    move.states = list(states.values())
    move.timers = sorted(timers.values(), key=lambda t: (t.line, t.field_name))
    if vercode_lines:
        for (fn, ln), text in sorted(vercode_lines.items(), key=lambda kv: kv[0][1])[:4]:
            move.notes.append(f"Light/Heavy (or Punch/Kick) also change behavior in code, e.g. {src.path.name}:{ln} `{text.strip().rstrip(';').strip()}`")
    return move


def _order_hits(hits: Sequence[int], defs: _Defs) -> List[int]:
    """Primary first: the last assigned id that is not a variable-counter / alternate-trigger row."""
    uniq: List[int] = []
    for h in hits:
        if h not in uniq:
            uniq.append(h)

    def is_alt(h: int) -> bool:
        sym, cmt = defs.at_by_value.get(h, ("", ""))
        return "_vc_" in sym or "variable counter" in cmt.lower() or "not 0" in cmt.lower()

    primary = [h for h in uniq if not is_alt(h)]
    alts = [h for h in uniq if is_alt(h)]
    primary_last = primary[-1:] if primary else []
    return primary_last + [h for h in primary[:-1]] + alts


# ---------------------------------------------------------------------------
# Naming moves, states and hits
# ---------------------------------------------------------------------------


def _name_move(move: MoveEntry, defs: _Defs, cmts: List[str], sp_rows_first: List[str]) -> None:
    name = ""
    for t in move.sp_types:
        sym_cmt = defs.sp_by_value.get(t)
        if sym_cmt and sym_cmt[1]:
            name = short_comment(sym_cmt[1], 48)
            break
    if not name:
        for t in move.sp_types:
            if t < len(cmts) and cmts[t]:
                name = short_comment(_clean_table_comment(cmts[t]), 48)
                if name:
                    break
    if not name:
        for t in move.sp_types:
            sym_cmt = defs.sp_by_value.get(t)
            if sym_cmt:
                name = _title(re.sub(r"^pl[0-9a-f]{2}_sp_", "", sym_cmt[0]))
                break
    if not name:
        name = _title(re.sub(r"^sp_", "", move.key))
    move.name = name


def _label_states(move: MoveEntry) -> None:
    for st in move.states:
        if st.custom_label:
            continue
        if st.group < 21 and not st.effect:
            gsym = ANIM_GROUP_MAP.get(st.group, (f"Group {st.group}", ""))[1]
            st.label = f"{move.name}: {gsym} #{st.anim} (common state)"
            continue
        phases: List[str] = []
        for fn in st.funcs[:2]:
            pref = ""
            m = re.match(r"^((?:shl|pls)[0-9a-fA-F]{2}_[0-9a-fA-F]{2})_?", fn)
            if m:
                pref = m.group(1) + "_"
            ph = phase_label(fn, move.key, pref)
            if ph and ph not in phases:
                phases.append(ph)
        phase = " / ".join(phases) if phases else "state"
        tag = ""
        if st.tags:
            tag = " (" + ", ".join(f"{k.replace('_', ' ')} {v}" for k, v in st.tags) + ")"
        who = "projectile/effect" if st.effect else ""
        suffix = f" [{who}]" if who else ""
        st.label = f"{move.name}: {phase}{tag}{suffix}"


def _label_hits(move: MoveEntry, char_hit_syms: Dict[int, Tuple[str, str]]) -> None:
    ver_keys = [v.key for v in move.versions]
    for h in move.hits:
        parts = [move.name]
        if h.versions and len(h.versions) < len(ver_keys):
            labels = [move.version(k).label if move.version(k) else k for k in ver_keys if k in h.versions]
            parts.append(" / ".join(labels))
        if h.variable_counter:
            parts.append("Variable Counter")
        if h.effect:
            parts.append("projectile")
        if h.sym and not h.variable_counter:
            tail = re.sub(r"^pl[0-9a-f]{2}_at_", "", h.sym)
            if tail and humanize(tail).lower() not in move.name.lower():
                parts.append(humanize(tail))
        h.label = " – ".join(parts)


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------


def build_catalog(
    root: Path,
    char_id: int,
    actions: Optional[Iterable[Tuple[int, int, str, str]]] = None,
    use_overrides: bool = True,
) -> CharCatalog:
    """Derive the move catalog for character `char_id`.

    `actions` optionally lists the editor's `(group, anim_idx, seq_name, comment)` states; states in the
    special/super/effect groups that no move claims are gathered in a final "Unassigned" entry.
    """
    defs = _parse_defs(root, char_id)
    cat = CharCatalog(char_id)
    cat.hit_names = {v: s for v, (s, _) in defs.at_by_value.items()}
    cat.hit_notes = {v: c for v, (_, c) in defs.at_by_value.items()}

    src = _load_src(root / "src" / f"s_pl{char_id:02x}.c")
    if src is not None:
        raw = (root / "src" / f"s_pl{char_id:02x}.c").read_text(encoding="utf-8", errors="replace")
        rows, cmts = _parse_sp_table(raw, src.stripped)
        commands = _parse_commands(src.stripped, defs)
        if rows:
            groups: Dict[str, List[int]] = {}
            order: List[str] = []
            for idx, fname in enumerate(rows[0]):
                if fname not in groups:
                    groups[fname] = []
                    order.append(fname)
                groups[fname].append(idx)
            for fname in order:
                sp_types = groups[fname]
                roots = [fname]
                for extra_row in rows[1:]:
                    for t in sp_types:
                        if t < len(extra_row) and extra_row[t] != fname and extra_row[t] not in roots:
                            roots.append(extra_row[t])
                cmd = next((commands[t] for t in sp_types if t in commands), ("", {0, 1}, False, False))
                try:
                    move = _derive_move(fname, roots, src, defs, sp_types, cmd)
                except Exception as exc:  # best effort: never let one odd routine break the editor
                    move = MoveEntry(key=fname, name=_title(re.sub(r"^sp_", "", fname)), sp_types=sp_types)
                    move.notes.append(f"Could not analyse routine `{fname}`: {exc}")
                if not move.name:
                    _name_move(move, defs, cmts, rows[0])
                move.kind = move.kind if move.kind == "Hyper" else (
                    "Hyper" if any("(hyper)" in (defs.sp_by_value.get(t, ("", ""))[1]).lower() for t in sp_types) else "Special"
                )
                _name_move(move, defs, cmts, rows[0])
                _label_states(move)
                _label_hits(move, defs.at_by_value)
                cat.moves.append(move)

    if use_overrides:
        apply_overrides(cat, char_id)
    if actions is not None:
        _add_unassigned(cat, list(actions))
    return cat


def _add_unassigned(cat: CharCatalog, actions: List[Tuple[int, int, str, str]]) -> None:
    claimed = {s.ref for m in cat.moves for s in m.states}
    leftovers = [a for a in actions if a[0] in (21, 22, 23, 24, 25) and (a[0], a[1]) not in claimed]
    if not leftovers:
        return
    mv = MoveEntry(key="__unassigned__", name="Unassigned special / super / effect states", kind="Other")
    mv.versions = [Version("*", "All versions", ((0, 0), (1, 0), (0, 1), (1, 1)))]
    for g, i, seq, cmt in leftovers:
        label = ""
        first = (cmt or "").splitlines()[0].strip() if cmt else ""
        if first:
            label = f"{ANIM_GROUP_MAP.get(g, ('', ''))[0]} {i}: {first.split(':', 1)[-1].strip() if ':' in first else ''}".strip()
        mv.states.append(StateInfo(g, i, label=label or f"{ANIM_GROUP_MAP.get(g, ('Grp', ''))[0]} #{i} (no known owner)", versions=set()))
    cat.moves.append(mv)


# ---------------------------------------------------------------------------
# Overrides
# ---------------------------------------------------------------------------


def override_path(char_id: int) -> Path:
    return OVERRIDE_DIR / f"char_{char_id:02x}.json"


def load_overrides(char_id: int) -> dict:
    p = override_path(char_id)
    if not p.exists():
        return {}
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def _ref(v) -> Tuple[int, int]:
    if isinstance(v, str):
        g, i = v.split(":")
        return int(g), int(i)
    return int(v[0]), int(v[1])


def apply_overrides(cat: CharCatalog, char_id: int) -> None:
    """Apply `move_overrides/char_XX.json`.

    ```
    {
      "state_labels": {"21:29": "Shadow clone 0: leaps"},
      "hit_labels":   {"0x4b": "Vajra: dive hit"},
      "moves": {
        "sp_warp": {"name": "Warp", "command": "41236+any", "notes": ["..."],
                     "states_remove": ["1:3"], "states_add": [["21", 2]],
                     "state_order": ["21:2", "21:3"], "hits_remove": [75]}
      },
      "extra_moves": [{"key": "x", "name": "...", "command": "", "kind": "Special",
                       "states": [["21", 4, "label"]], "hits": [[48, "label"]]}]
    }
    ```
    """
    data = load_overrides(char_id)
    if not data:
        return
    cat.overrides_used = True
    for key, mo in (data.get("moves") or {}).items():
        move = cat.move_by_key(key)
        if move is None:
            continue
        if mo.get("name"):
            move.name = mo["name"]
            for st in move.states:
                st.custom_label = False
            _label_states(move)
            _label_hits(move, {})
        if "command" in mo:
            move.command = mo["command"]
        if mo.get("kind"):
            move.kind = mo["kind"]
        for n in mo.get("notes", []):
            move.notes.append(n)
        rem = {_ref(r) for r in mo.get("states_remove", [])}
        move.states = [s for s in move.states if s.ref not in rem]
        for add in mo.get("states_add", []):
            ref = _ref(add)
            if not any(s.ref == ref for s in move.states):
                move.states.append(StateInfo(ref[0], ref[1], label=add[2] if len(add) > 2 and not isinstance(add, str) else "", custom_label=True))
        order = [_ref(r) for r in mo.get("state_order", [])]
        if order:
            pos = {r: i for i, r in enumerate(order)}
            move.states.sort(key=lambda s: pos.get(s.ref, len(pos) + 1))
        hrem = {int(str(h), 0) for h in mo.get("hits_remove", [])}
        move.hits = [h for h in move.hits if h.atck_no not in hrem]
        for h in mo.get("hits_add", []):
            no = int(str(h[0]), 0)
            if not any(x.atck_no == no for x in move.hits):
                move.hits.append(HitInfo(no, label=h[1] if len(h) > 1 else ""))
    for em in data.get("extra_moves") or []:
        mv = MoveEntry(key=em["key"], name=em["name"], command=em.get("command", ""), kind=em.get("kind", "Special"))
        mv.versions = [Version("*", "All versions", ((0, 0), (1, 0), (0, 1), (1, 1)))]
        for s in em.get("states", []):
            g, i = _ref(s)
            mv.states.append(StateInfo(g, i, label=s[2] if len(s) > 2 else "", custom_label=True))
        for h in em.get("hits", []):
            mv.hits.append(HitInfo(int(str(h[0]), 0), label=h[1] if len(h) > 1 else ""))
        cat.moves.append(mv)
    for ref_s, label in (data.get("state_labels") or {}).items():
        g, i = _ref(ref_s)
        for m in cat.moves:
            for s in m.states:
                if s.ref == (g, i):
                    s.label = label
                    s.custom_label = True
    for hid, label in (data.get("hit_labels") or {}).items():
        no = int(str(hid), 0)
        for m in cat.moves:
            for h in m.hits:
                if h.atck_no == no:
                    h.label = label
