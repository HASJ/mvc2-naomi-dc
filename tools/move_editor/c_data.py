"""C source parser, symbolic serializer, and binary compiler for mvc2-naomi-dc data/* and src/*."""

from __future__ import annotations

import re
import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from .constants import (
    ANIM_GROUP_MAP,
    ANIM_GROUP_SYM_TO_ID,
    ATCK_NAME_TO_ID,
    BOX_MASK,
    CHAR_BY_ID,
    CHAR_SYM_TO_ID,
    CHARACTERS,
    NORMAL_ANIM_TO_ATCK,
    PUSH_BOX_BASE,
    STANDARD_ATCK_NAMES,
    TBL_SLUG_TO_GROUP,
)

# Symbolic expression evaluators matching include/hit_equ.h, pl_data.h, char_tbl.h, pl_work.h
C_CONSTANTS: Dict[str, int] = {
    "ZU": 0,
    "BU_O": 1,
    "AS": 4,
    "FLY_SCREEN": 0x20,
    "NO_VEC": 0x40,
    "NO_ZU": 0x80,
    "A_NOM": 0,
    "A_HI": 0x02,
    "A_LO": 0x04,
    "A_GUARD_NG": 0x08,
    "A_SPECIAL": 0x10,
    "A_HYPER": 0x20,
    "K_S": 0x10,
    "K_M": 0x20,
    "K_L": 0x30,
    "K_LOW": 0x80,
    "K_LS": 0x90,
    "K_LM": 0xA0,
    "K_LL": 0xB0,
    "K_BACK_MAX": 0x08,
    "MARK_N1": 0,
    "MARK_N2": 1,
    "MARK_N3": 2,
    "SC100": 4,
    "SC200": 8,
    "SC300": 12,
    "SD_NOR_S": 5,
    "SD_NOR_M": 6,
    "SD_NOR_L": 7,
    "SD_KIK_S": 9,
    "SD_KIK_M": 10,
    "SD_KIK_L": 11,
    "YK_GD030": 0,
    "YK_GD060": 1,
    "NO_CHIP": 0x02,
    "NO_WEIGHT": 0x04,
    "NO_SCALE": 0x08,
    "NO_COMBO": 0x10,
    "NO_KO": 0x20,
    "BLACK_COMBO": 0x40,
    "AERIAL_FINISH": 0x80,
    "END_OF_DATA": 0x80,
    "ATK": 0x80,
    "ATE": 0x40,
    "CG_ATCK_NG": 0x20,
    "CSE": 0x80,
}


def eval_c_int(expr: str, extra: Optional[Dict[str, int]] = None) -> int:
    """Evaluate a restricted C integer/bitwise-OR expression from data/*.c."""
    s = expr.strip()
    if not s:
        return 0
    if "|" not in s and s in C_CONSTANTS:
        return C_CONSTANTS[s]
    if extra and s in extra:
        return extra[s]
    total = 0
    for part in s.split("|"):
        tok = part.strip()
        if not tok:
            continue
        if tok in C_CONSTANTS:
            total |= C_CONSTANTS[tok]
        elif extra and tok in extra:
            total |= extra[tok]
        else:
            total |= int(tok, 0)
    return total


def to_s8(val: int) -> int:
    val &= 0xFF
    return val - 256 if val >= 128 else val


def to_u8(val: int) -> int:
    return val & 0xFF


def to_s16(val: int) -> int:
    val &= 0xFFFF
    return val - 65536 if val >= 32768 else val


def _strip_c_comments(text: str) -> str:
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def _split_top_level_commas(text: str) -> List[str]:
    items: List[str] = []
    depth_paren = 0
    depth_brace = 0
    start = 0
    for idx, ch in enumerate(text):
        if ch == "(":
            depth_paren += 1
        elif ch == ")":
            depth_paren -= 1
        elif ch == "{":
            depth_brace += 1
        elif ch == "}":
            depth_brace -= 1
        elif ch == "," and depth_paren == 0 and depth_brace == 0:
            items.append(text[start:idx].strip())
            start = idx + 1
    tail = text[start:].strip()
    if tail:
        items.append(tail)
    return items


@dataclass
class CGFrame:
    prm: int
    ftype: int
    ctr: int
    end: int
    spr: int
    prio: int
    shadow: int
    atck: int
    flag1: int
    se: int
    zoom: int
    unk: int
    flag2: int
    rival: int
    box: int
    kind: str = "CG_FRAME"  # "CG_FRAME" or "EF_FRAME"

    def clone(self) -> "CGFrame":
        return CGFrame(
            self.prm,
            self.ftype,
            self.ctr,
            self.end,
            self.spr,
            self.prio,
            self.shadow,
            self.atck,
            self.flag1,
            self.se,
            self.zoom,
            self.unk,
            self.flag2,
            self.rival,
            self.box,
            self.kind,
        )

    def to_bytes(self) -> bytes:
        if self.kind == "EF_FRAME":
            return struct.pack(
                "<BBBBi",
                self.prm & 0xFF,
                self.ftype & 0xFF,
                self.ctr & 0xFF,
                self.end & 0xFF,
                int(self.spr),
            )
        return struct.pack(
            "<BBBBiBBBBBBBBHH",
            self.prm & 0xFF,
            self.ftype & 0xFF,
            self.ctr & 0xFF,
            self.end & 0xFF,
            int(self.spr),
            self.prio & 0xFF,
            self.shadow & 0xFF,
            self.atck & 0xFF,
            self.flag1 & 0xFF,
            self.se & 0xFF,
            self.zoom & 0xFF,
            self.unk & 0xFF,
            self.flag2 & 0xFF,
            self.rival & 0xFFFF,
            self.box & 0xFFFF,
        )

    def byte_size(self) -> int:
        return 8 if self.kind == "EF_FRAME" else 20


@dataclass
class CGNext:
    target: str  # symbol name or "0"

    def byte_size(self) -> int:
        return 4


@dataclass
class RawWord:
    value: int

    def byte_size(self) -> int:
        return 4


SeqItem = CGFrame | CGNext | RawWord


@dataclass
class CharSequence:
    name: str
    items: List[SeqItem]
    comment: str = ""  # preceding comment block (excluding /* */ wrapper)

    def frames(self) -> List[CGFrame]:
        return [it for it in self.items if isinstance(it, CGFrame)]

    def byte_size(self) -> int:
        return sum(it.byte_size() for it in self.items)


@dataclass
class FrameBox:
    atck_box: int = 0
    atck_box2: int = 0
    body_box0: int = 0
    body_box1: int = 0
    body_box2: int = 0
    body_box3: int = 0
    catch_box: int = 0
    catch_box2: int = 0

    def clone(self) -> "FrameBox":
        return FrameBox(
            self.atck_box,
            self.atck_box2,
            self.body_box0,
            self.body_box1,
            self.body_box2,
            self.body_box3,
            self.catch_box,
            self.catch_box2,
        )

    def to_bytes(self) -> bytes:
        return struct.pack(
            "<8H",
            self.atck_box & 0xFFFF,
            self.atck_box2 & 0xFFFF,
            self.body_box0 & 0xFFFF,
            self.body_box1 & 0xFFFF,
            self.body_box2 & 0xFFFF,
            self.body_box3 & 0xFFFF,
            self.catch_box & 0xFFFF,
            self.catch_box2 & 0xFFFF,
        )


@dataclass
class HitBox:
    x: int = 0
    w: int = 0
    y: int = 0
    h: int = 0

    def clone(self) -> "HitBox":
        return HitBox(self.x, self.w, self.y, self.h)

    def is_zero(self) -> bool:
        return self.x == 0 and self.w == 0 and self.y == 0 and self.h == 0

    def to_bytes(self) -> bytes:
        return struct.pack("<4h", int(self.x), int(self.w), int(self.y), int(self.h))


@dataclass
class HitData:
    damage: int = 0
    damage_type: int = 0
    atck_kind: int = 0
    damage_kind: int = 0
    piyo: int = 0
    hit_stop: int = 0
    sandbag_time: int = 0
    sleep_time: int = 0
    vector: int = 0  # signed 8-bit (-128..127)
    power: int = 0
    atck_range: int = 0
    hit_mark: int = 0
    score: int = 0
    hit_se: int = 0
    shc: int = 0
    jug: int = 0
    range_box: int = 0
    range_box2: int = 0
    hit_flag: int = 0
    unk_13: int = 0
    guard_time: int = 0
    unk_15: int = 0
    catch_ng_time: int = 0
    abyss_jug: int = 0
    hit_cnt: int = 0
    cpu_answer_no: int = 0
    unk_1a_0: int = 0
    unk_1a_1: int = 0
    comment: str = ""

    def is_empty(self) -> bool:
        return (
            self.damage == 0
            and self.damage_type == 0
            and self.atck_kind == 0
            and self.damage_kind == 0
            and self.piyo == 0
            and self.hit_stop == 0
            and self.sandbag_time == 0
            and self.sleep_time == 0
            and self.vector == 0
            and self.power == 0
            and self.atck_range == 0
            and self.hit_mark == 0
            and self.score == 0
            and self.hit_se == 0
            and self.shc == 0
            and self.jug == 0
            and self.range_box == 0
            and self.range_box2 == 0
            and self.hit_flag == 0
            and self.unk_13 == 0
            and self.guard_time == 0
            and self.unk_15 == 0
            and self.catch_ng_time == 0
            and self.abyss_jug == 0
            and self.hit_cnt == 0
            and self.cpu_answer_no == 0
            and self.unk_1a_0 == 0
            and self.unk_1a_1 == 0
        )

    def to_bytes(self) -> bytes:
        return struct.pack(
            "<28B",
            self.damage & 0xFF,
            self.damage_type & 0xFF,
            self.atck_kind & 0xFF,
            self.damage_kind & 0xFF,
            self.piyo & 0xFF,
            self.hit_stop & 0xFF,
            self.sandbag_time & 0xFF,
            self.sleep_time & 0xFF,
            self.vector & 0xFF,
            self.power & 0xFF,
            self.atck_range & 0xFF,
            self.hit_mark & 0xFF,
            self.score & 0xFF,
            self.hit_se & 0xFF,
            self.shc & 0xFF,
            self.jug & 0xFF,
            self.range_box & 0xFF,
            self.range_box2 & 0xFF,
            self.hit_flag & 0xFF,
            self.unk_13 & 0xFF,
            self.guard_time & 0xFF,
            self.unk_15 & 0xFF,
            self.catch_ng_time & 0xFF,
            self.abyss_jug & 0xFF,
            self.hit_cnt & 0xFF,
            self.cpu_answer_no & 0xFF,
            self.unk_1a_0 & 0xFF,
            self.unk_1a_1 & 0xFF,
        )


@dataclass
class WhiffGaugeEntry:
    func_name: str
    file_path: Path
    line_no: int
    amount: int
    move_hint: str
    group: Optional[int] = None
    anim_indices: List[int] = field(default_factory=list)


@dataclass
class BounceRule:
    pl_type: int
    atck_no: int
    need_air: int
    prop: int
    no_fly_screen: int
    height: float = 0.0
    comment: str = ""


@dataclass
class CancelRule:
    pl_type: int
    act_kind: int  # 0 = atck_no, 1 = pl_sp_type
    move_id: int
    flags: int  # 0x01=Jump, 0x02=Special, 0x04=Hyper
    comment: str = ""


@dataclass
class ActionEntry:
    group: int
    anim_idx: int
    seq_name: str
    display_label: str
    atck_candidates: List[int] = field(default_factory=list)
    whiff_entries: List[WhiffGaugeEntry] = field(default_factory=list)
    routine_hints: List[str] = field(default_factory=list)


@dataclass
class CharSourceData:
    char_id: int
    header_comment: str
    spr_prm: List[Tuple[int, int, int, int, int, int, int, int]]
    char_tbl: List[str]
    group_tables: Dict[str, List[str]]
    sequences: Dict[str, CharSequence]
    # Exact order in C file after char_tbl_XX: list of ("tbl", name) or ("seq", name)
    decl_order: List[Tuple[str, str]]
    frame_boxes: List[FrameBox]
    hit_boxes: List[HitBox]
    hit_data: List[HitData]
    atck_names: Dict[int, str]
    sp_names: Dict[int, str]
    whiff_gauges: List[WhiffGaugeEntry]
    actions: List[ActionEntry]
    projectile_atck_map: Dict[Tuple[int, int], List[int]]  # (group, anim_idx) -> [atck_no...]
    orig_hit_dt_count: int = 0


def count_frame_box_users(data: CharSourceData, fb_idx: int) -> List[str]:
    """Return list of unique animation sequence names referencing frame_box index `fb_idx`."""
    users: List[str] = []
    for sym in data.sequences:
        seq = data.sequences[sym]
        for fr in seq.frames():
            if fr.box == fb_idx:
                users.append(sym)
                break
    return users


def count_hit_box_users(data: CharSourceData, hb_idx: int) -> List[int]:
    """Return list of frame_box indices referencing hit_box index `hb_idx` (low 13 bits)."""
    if hb_idx <= 0:
        return []
    users: List[int] = []
    for idx, fb in enumerate(data.frame_boxes):
        slots = [
            fb.atck_box & BOX_MASK,
            fb.atck_box2 & BOX_MASK,
            fb.body_box0 & BOX_MASK,
            fb.body_box1 & BOX_MASK,
            fb.body_box2 & BOX_MASK,
            fb.body_box3 & BOX_MASK,
            fb.catch_box & BOX_MASK,
        ]
        if 0 < fb.catch_box2 < 0x8000:
            slots.append(fb.catch_box2 & BOX_MASK)
        if hb_idx in slots:
            users.append(idx)
    return users


def allocate_hit_box(data: CharSourceData, initial: Optional[HitBox] = None) -> int:
    """Allocate an unused HIT_BOX slot (preferring free 0 slots in 1..251 away from PUSH_BOX 252..255)."""
    used: set[int] = {0}
    for fb in data.frame_boxes:
        slot_vals = [
            fb.atck_box,
            fb.atck_box2,
            fb.body_box0,
            fb.body_box1,
            fb.body_box2,
            fb.body_box3,
            fb.catch_box,
        ]
        if 0 < fb.catch_box2 < 0x8000:
            slot_vals.append(fb.catch_box2)
        used.update(
            x & BOX_MASK
            for x in slot_vals
            if (x & BOX_MASK) > 0
        )
    for hd in data.hit_data:
        if hd.range_box:
            used.add(hd.range_box)
        if hd.range_box2:
            used.add(hd.range_box2)

    box = initial.clone() if initial is not None else HitBox(-40, 25, -55, 20)
    # Prefer reusing an empty (0,0,0,0) slot in the reserved block 168..239 or 1..251
    for i in range(min(240, len(data.hit_boxes)) - 1, 0, -1):
        if i not in used and data.hit_boxes[i].is_zero():
            data.hit_boxes[i] = box
            return i
    for i in range(1, min(PUSH_BOX_BASE, len(data.hit_boxes))):
        if i not in used and data.hit_boxes[i].is_zero():
            data.hit_boxes[i] = box
            return i
    # If all 1..251 are used, append past PUSH_BOX (252..255)
    while len(data.hit_boxes) < PUSH_BOX_BASE + 4:
        data.hit_boxes.append(HitBox())
    data.hit_boxes.append(box)
    return len(data.hit_boxes) - 1


def clone_frame_box(data: CharSourceData, fb_idx: int) -> int:
    """Duplicate frame_box row `fb_idx` and return the new index."""
    src = data.frame_boxes[fb_idx].clone() if 0 <= fb_idx < len(data.frame_boxes) else FrameBox()
    data.frame_boxes.append(src)
    return len(data.frame_boxes) - 1


# ---------------------------------------------------------------------------
# Parsing helpers
# ---------------------------------------------------------------------------


def parse_pl_def_h(root: Path, char_id: int) -> Tuple[Dict[int, str], Dict[int, str], Dict[str, int]]:
    """Parse include/plXX_def.h for sp names, atck_no names, and symbol values."""
    atck_names = dict(STANDARD_ATCK_NAMES)
    sp_names: Dict[int, str] = {}
    defs: Dict[str, int] = dict(ATCK_NAME_TO_ID)
    p = root / "include" / f"pl{char_id:02x}_def.h"
    if not p.exists():
        return atck_names, sp_names, defs
    text = p.read_text(encoding="utf-8", errors="replace")
    for line in text.splitlines():
        m = re.match(r"^\s*#\s*define\s+(\w+)\s+(0x[0-9a-fA-F]+|\d+)\s*(?:/\*(.*?)\*/)?", line)
        if not m:
            continue
        sym, val_s, cmt = m.group(1), m.group(2), (m.group(3) or "").strip()
        val = int(val_s, 0)
        defs[sym] = val
        if "_sp_" in sym:
            sp_names[val] = f"{sym} ({cmt})" if cmt else sym
        elif "_at_" in sym:
            atck_names[val] = f"{sym}: {cmt}" if cmt else sym
    return atck_names, sp_names, defs


def _parse_char_c(path: Path, char_id: int):
    raw = path.read_text(encoding="utf-8", errors="replace")
    m_hdr = re.match(r"^\s*/\*(.*?)\*/", raw, re.S)
    header_comment = m_hdr.group(1) if m_hdr else ""

    # Parse spr_prm_XX
    m_spr = re.search(
        r"const\s+SPR_PRM\s+spr_prm_[0-9a-f]+\s*\[\s*\d*\s*\]\s*=\s*\{(.*?)\};",
        raw,
        re.S,
    )
    if not m_spr:
        raise ValueError(f"Could not find spr_prm_{char_id:02x} in {path}")
    spr_prm: List[Tuple[int, int, int, int, int, int, int, int]] = []
    for rm in re.finditer(r"\{([^}]*)\}", m_spr.group(1)):
        vals = [int(v.strip(), 0) for v in rm.group(1).split(",") if v.strip()]
        if len(vals) == 8:
            spr_prm.append(tuple(v & 0xFFFF for v in vals))  # type: ignore[arg-type]

    # Parse char_tbl_XX
    m_ctbl = re.search(
        r"const\s+Sint32\s+\*const\s+\*const\s+char_tbl_[0-9a-f]+\s*\[\s*\]\s*=\s*\{(.*?)\};",
        raw,
        re.S,
    )
    if not m_ctbl:
        raise ValueError(f"Could not find char_tbl_{char_id:02x} in {path}")
    ctbl_body = _strip_c_comments(m_ctbl.group(1))
    char_tbl = [tok.strip() for tok in ctbl_body.split(",") if tok.strip()]

    # Parse remaining definitions after char_tbl_XX in exact file order
    tail = raw[m_ctbl.end() :]
    decl_pat = re.compile(
        r"(?P<comments>(?:\s*/\*.*?\*/\s*)*)"
        r"const\s+Sint32\s+(?P<ptr>\*const\s+)?(?P<name>\w+)\s*\[\s*\]\s*=\s*\{(?P<body>.*?)\};",
        re.S,
    )
    group_tables: Dict[str, List[str]] = {}
    sequences: Dict[str, CharSequence] = {}
    decl_order: List[Tuple[str, str]] = []

    frame_pat = re.compile(
        r"(CG_FRAME|EF_FRAME|CG_NEXT)\s*\(([^)]*)\)|(0x[0-9a-fA-F]+|-?\d+)"
    )

    for m in decl_pat.finditer(tail):
        name = m.group("name")
        is_ptr = bool(m.group("ptr"))
        body = m.group("body")
        comments_raw = m.group("comments") or ""
        cmt_blocks = re.findall(r"/\*(.*?)\*/", comments_raw, re.S)
        # Filter out column header banner /* prm type ctr ... */
        meaningful_cmts = [c.strip() for c in cmt_blocks if "Shadow" not in c and "prm  type" not in c]
        seq_cmt = "\n".join(meaningful_cmts).strip()

        clean_body = _strip_c_comments(body)
        if is_ptr:
            ptrs = [t.strip() for t in clean_body.split(",") if t.strip()]
            group_tables[name] = ptrs
            decl_order.append(("tbl", name))
        else:
            items: List[SeqItem] = []
            for fm in frame_pat.finditer(clean_body):
                kind = fm.group(1)
                args_s = fm.group(2)
                raw_tok = fm.group(3)
                if kind == "CG_FRAME":
                    args = [eval_c_int(a) for a in args_s.split(",")]
                    items.append(
                        CGFrame(
                            args[0],
                            args[1],
                            args[2],
                            args[3],
                            args[4],
                            args[5],
                            args[6],
                            args[7],
                            args[8],
                            args[9],
                            args[10],
                            args[11],
                            args[12],
                            args[13],
                            args[14],
                            kind="CG_FRAME",
                        )
                    )
                elif kind == "EF_FRAME":
                    args = [eval_c_int(a) for a in args_s.split(",")]
                    items.append(
                        CGFrame(
                            args[0],
                            args[1],
                            args[2],
                            args[3],
                            args[4],
                            0,
                            0,
                            0,
                            0,
                            0,
                            0,
                            0,
                            0,
                            0,
                            0,
                            kind="EF_FRAME",
                        )
                    )
                elif kind == "CG_NEXT":
                    items.append(CGNext(args_s.strip()))
                elif raw_tok is not None:
                    items.append(RawWord(int(raw_tok, 0) & 0xFFFFFFFF))
            sequences[name] = CharSequence(name=name, items=items, comment=seq_cmt)
            decl_order.append(("seq", name))

    return header_comment, spr_prm, char_tbl, group_tables, sequences, decl_order


def _parse_frame_box_c(path: Path) -> List[FrameBox]:
    text = _strip_c_comments(path.read_text(encoding="utf-8", errors="replace"))
    m = re.search(r"const\s+FRAME_BOX\s+\w+\s*\[\s*\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        raise ValueError(f"Could not find FRAME_BOX array in {path}")
    boxes: List[FrameBox] = []
    for rm in re.finditer(r"\{([^}]*)\}", m.group(1)):
        vals = [eval_c_int(v) for v in rm.group(1).split(",") if v.strip()]
        while len(vals) < 8:
            vals.append(0)
        boxes.append(FrameBox(*vals[:8]))
    return boxes


def _parse_hit_c(path: Path) -> List[HitBox]:
    text = _strip_c_comments(path.read_text(encoding="utf-8", errors="replace"))
    m = re.search(r"const\s+HIT_BOX\s+\w+\s*\[\s*\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        raise ValueError(f"Could not find HIT_BOX array in {path}")
    boxes: List[HitBox] = []
    for rm in re.finditer(r"\{([^}]*)\}", m.group(1)):
        vals = [int(v.strip(), 0) for v in rm.group(1).split(",") if v.strip()]
        while len(vals) < 4:
            vals.append(0)
        boxes.append(HitBox(*vals[:4]))
    return boxes


def _parse_hit_dt_c(path: Path) -> List[HitData]:
    raw = path.read_text(encoding="utf-8", errors="replace")
    m = re.search(r"const\s+HIT_DATA\s+\w+\s*\[\s*\]\s*=\s*\{(.*?)\};", raw, re.S)
    if not m:
        raise ValueError(f"Could not find HIT_DATA array in {path}")
    body = m.group(1)
    # Each row starts at top-level '{ ... }' and may have a trailing /* ... */ comment on the line
    rows: List[HitData] = []
    pos = 0
    n = len(body)
    while pos < n:
        ch = body[pos]
        if ch == "{":
            # Scan matching '}'
            depth = 1
            start = pos + 1
            pos += 1
            while pos < n and depth > 0:
                if body[pos] == "{":
                    depth += 1
                elif body[pos] == "}":
                    depth -= 1
                pos += 1
            inner = body[start : pos - 1]
            # Look for comment on the same line after '}'
            eol = body.find("\n", pos)
            if eol == -1:
                eol = n
            rest_line = body[pos:eol]
            cm = re.search(r"/\*(.*?)\*/", rest_line)
            cmt = cm.group(1).strip() if cm else ""

            # Flatten nested braces and evaluate all fields
            flat = _strip_c_comments(inner).replace("{", "").replace("}", "")
            vals = [eval_c_int(v) for v in flat.split(",") if v.strip()]
            if len(vals) <= 1 and (not vals or vals[0] == 0):
                rows.append(HitData(comment=cmt))
            else:
                while len(vals) < 28:
                    vals.append(0)
                hd = HitData(
                    damage=vals[0] & 0xFF,
                    damage_type=vals[1] & 0xFF,
                    atck_kind=vals[2] & 0xFF,
                    damage_kind=vals[3] & 0xFF,
                    piyo=vals[4] & 0xFF,
                    hit_stop=vals[5] & 0xFF,
                    sandbag_time=vals[6] & 0xFF,
                    sleep_time=vals[7] & 0xFF,
                    vector=to_s8(vals[8]),
                    power=vals[9] & 0xFF,
                    atck_range=vals[10] & 0xFF,
                    hit_mark=vals[11] & 0xFF,
                    score=vals[12] & 0xFF,
                    hit_se=vals[13] & 0xFF,
                    shc=vals[14] & 0xFF,
                    jug=vals[15] & 0xFF,
                    range_box=vals[16] & 0xFF,
                    range_box2=vals[17] & 0xFF,
                    hit_flag=vals[18] & 0xFF,
                    unk_13=vals[19] & 0xFF,
                    guard_time=vals[20] & 0xFF,
                    unk_15=vals[21] & 0xFF,
                    catch_ng_time=vals[22] & 0xFF,
                    abyss_jug=vals[23] & 0xFF,
                    hit_cnt=vals[24] & 0xFF,
                    cpu_answer_no=vals[25] & 0xFF,
                    unk_1a_0=vals[26] & 0xFF,
                    unk_1a_1=vals[27] & 0xFF,
                    comment=cmt,
                )
                rows.append(hd)
        else:
            pos += 1
    return rows


def _scan_s_pl_and_shl(
    root: Path,
    char_id: int,
    defs: Dict[str, int],
) -> Tuple[List[WhiffGaugeEntry], Dict[Tuple[int, int], List[int]], Dict[Tuple[int, int], List[str]]]:
    """Scan s_plXX.c, shlXX_*.c, plsXX_*.c for whiff gauge calls, (grp, no) -> atck_no & move hints."""
    whiffs: List[WhiffGaugeEntry] = []
    anim_to_atck: Dict[Tuple[int, int], List[int]] = {}
    anim_to_hints: Dict[Tuple[int, int], List[str]] = {}

    s_pl_path = root / "src" / f"s_pl{char_id:02x}.c"
    if s_pl_path.exists():
        lines = s_pl_path.read_text(encoding="utf-8", errors="replace").splitlines()
        cur_func = "<top>"
        last_atcks: List[int] = []
        last_char: Optional[Tuple[int, int]] = None
        func_whiffs: List[int] = []
        brace_depth = 0
        for lineno, line in enumerate(lines, start=1):
            stripped = line.strip()
            if brace_depth == 0:
                fm = re.match(r"^(?:static\s+)?(?:void|int|Uint\d+|Sint\d+|PLAYER_WORK\s*\*)\s*\*?\s*(\w+)\s*\(", stripped)
                if fm and not stripped.endswith(";"):
                    cur_func = fm.group(1)
                    last_atcks = []
                    last_char = None
                    func_whiffs = []
            brace_depth += line.count("{") - line.count("}")

            # Track atck_no assignments
            am = re.search(r"atck_no\s*=\s*([^;]+);", line)
            if am:
                rhs = am.group(1).strip()
                # Direct constant
                if rhs in defs:
                    last_atcks.append(defs[rhs] & 0x7F)
                elif re.fullmatch(r"0x[0-9a-fA-F]+|\d+", rhs):
                    last_atcks.append(int(rhs, 0) & 0x7F)
                else:
                    # e.g. pl_w->atck_level + 48 or similar
                    om = re.search(r"(?:atck_level|nml_no|sub_type)\s*\+\s*(0x[0-9a-fA-F]+|\d+)", rhs)
                    if om:
                        base = int(om.group(1), 0)
                        for lv in range(3):
                            last_atcks.append((base + lv) & 0x7F)

            # Track pl_char_set(pl_w, Group, no)
            cm = re.search(r"pl_char_set\s*\(\s*\w+\s*,\s*([^,]+),\s*([^)]+)\)", line)
            if cm:
                grp_tok = cm.group(1).strip()
                no_tok = cm.group(2).strip()
                grp = ANIM_GROUP_SYM_TO_ID.get(grp_tok)
                if grp is None and re.fullmatch(r"0x[0-9a-fA-F]+|\d+", grp_tok):
                    grp = int(grp_tok, 0)
                if grp is not None:
                    nos: List[int] = []
                    if re.fullmatch(r"0x[0-9a-fA-F]+|\d+", no_tok):
                        nos = [int(no_tok, 0)]
                    elif re.search(r"(?:atck_level|nml_no)\s*\+\s*(0x[0-9a-fA-F]+|\d+)", no_tok):
                        om = re.search(r"\+\s*(0x[0-9a-fA-F]+|\d+)", no_tok)
                        b = int(om.group(1), 0) if om else 0
                        nos = [b, b + 1, b + 2]
                    elif no_tok in ("pl_w->atck_level", "atck_level"):
                        nos = [0, 1, 2]
                    for n in nos:
                        last_char = (grp, n)
                        anim_to_hints.setdefault(last_char, []).append(cur_func)
                        for a in last_atcks:
                            lst = anim_to_atck.setdefault(last_char, [])
                            if a not in lst:
                                lst.append(a)
                        for wi in func_whiffs:
                            w = whiffs[wi]
                            if w.group is None:
                                w.group = grp
                            if n not in w.anim_indices:
                                w.anim_indices.append(n)

            # Track pl_gauge_add(pl_w, N)
            gm = re.search(r"pl_gauge_add\s*\(\s*\w+\s*,\s*(-?\d+)\s*\)", line)
            if gm:
                amt = int(gm.group(1))
                entry = WhiffGaugeEntry(
                    func_name=cur_func,
                    file_path=s_pl_path,
                    line_no=lineno,
                    amount=amt,
                    move_hint=cur_func,
                    group=last_char[0] if last_char else None,
                    anim_indices=[last_char[1]] if last_char else [],
                )
                func_whiffs.append(len(whiffs))
                whiffs.append(entry)

            if brace_depth <= 0:
                brace_depth = 0
                cur_func = "<top>"

    # Also scan shlXX_*.c and plsXX_*.c for Group 23 (Char_Effect) or other group links
    for pat in (f"shl{char_id:02x}_*.c", f"pls{char_id:02x}_*.c"):
        for sp in sorted((root / "src").glob(pat)):
            stext = _strip_c_comments(sp.read_text(encoding="utf-8", errors="replace"))
            # Look for SHL tables with { ..., char_no, atck_no } or direct assignments
            for rm in re.finditer(r"\{\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(\d+|0x[0-9a-f]+)\s*,\s*([^}]+)\}", stext):
                cno = int(rm.group(5), 0)
                at_tok = rm.group(6).strip()
                ano = defs.get(at_tok)
                if ano is None and re.fullmatch(r"0x[0-9a-fA-F]+|\d+", at_tok):
                    ano = int(at_tok, 0)
                if ano is not None:
                    lst = anim_to_atck.setdefault((23, cno), [])
                    if (ano & 0x7F) not in lst:
                        lst.append(ano & 0x7F)
                    anim_to_hints.setdefault((23, cno), []).append(sp.stem)
            # Fallback: find pl_char_set / ef_char_set with atck_no in same file
            ef_chars = re.findall(r"(?:ef_char_set|pl_char_set)\s*\(\s*\w+\s*,\s*([^,]+),\s*(\d+|0x[0-9a-f]+)", stext)
            at_assigns = re.findall(r"atck_no\s*=\s*([A-Za-z0-9_]+)\s*;", stext)
            found_atcks = []
            for tok in at_assigns:
                if tok in defs:
                    found_atcks.append(defs[tok] & 0x7F)
                elif re.fullmatch(r"0x[0-9a-fA-F]+|\d+", tok):
                    found_atcks.append(int(tok, 0) & 0x7F)
            for grp_s, no_s in ef_chars:
                grp = ANIM_GROUP_SYM_TO_ID.get(grp_s.strip())
                if grp is None and re.fullmatch(r"\d+|0x[0-9a-fA-F]+", grp_s.strip()):
                    grp = int(grp_s.strip(), 0)
                if grp is not None:
                    no = int(no_s, 0)
                    anim_to_hints.setdefault((grp, no), []).append(sp.stem)
                    for a in found_atcks:
                        lst = anim_to_atck.setdefault((grp, no), [])
                        if a not in lst:
                            lst.append(a)

    return whiffs, anim_to_atck, anim_to_hints


def load_character(root: Path, char_id: int) -> CharSourceData:
    """Load complete source representation for character XX (0x00..0x3A)."""
    hex_id = f"{char_id:02x}"
    atck_names, sp_names, defs = parse_pl_def_h(root, char_id)
    header_comment, spr_prm, char_tbl, group_tables, sequences, decl_order = _parse_char_c(
        root / "data" / f"char_{hex_id}.c", char_id
    )
    frame_boxes = _parse_frame_box_c(root / "data" / f"frame_box{hex_id}.c")
    hit_boxes = _parse_hit_c(root / "data" / f"hit_{hex_id}.c")
    hit_data = _parse_hit_dt_c(root / "data" / f"hit_dt{hex_id}.c")
    orig_hd_count = len(hit_data)
    while len(hit_data) < 128:
        hit_data.append(HitData())
    whiff_gauges, proj_atck_map, anim_hints = _scan_s_pl_and_shl(root, char_id, defs)

    # Enrich empty HitData comments using plXX_def.h names if available
    for idx, hd in enumerate(hit_data):
        if not hd.comment and idx in atck_names:
            hd.comment = atck_names[idx]

    # Build comprehensive ActionEntry list for EVERY (group, anim_idx) slot across char_tbl_XX
    actions: List[ActionEntry] = []
    for grp_idx, tbl_sym in enumerate(char_tbl):
        if tbl_sym == "0" or tbl_sym not in group_tables:
            continue
        grp_sym, grp_desc = ANIM_GROUP_MAP.get(grp_idx, (f"Grp_{grp_idx}", f"Group {grp_idx}"))
        ptrs = group_tables[tbl_sym]
        for anim_idx, seq_sym in enumerate(ptrs):
            if seq_sym == "0" or seq_sym not in sequences:
                continue
            seq = sequences[seq_sym]
            # Infer candidate atck_no values from:
            # 1) standard normals table (7..12)
            # 2) CGFrame flag1 values on active frames (ATK/ATE or nonzero flag1)
            # 3) s_plXX / shlXX mappings
            # 4) char_XX.c comment annotations
            cands: List[int] = []
            if (grp_idx, anim_idx) in NORMAL_ANIM_TO_ATCK:
                a_id, nm = NORMAL_ANIM_TO_ATCK[(grp_idx, anim_idx)]
                if a_id not in cands:
                    cands.append(a_id)
            else:
                nm = ""
            for a_id in proj_atck_map.get((grp_idx, anim_idx), []):
                if a_id not in cands:
                    cands.append(a_id)
            for fr in seq.frames():
                if (fr.atck & 0xC0) and fr.flag1 > 0:
                    a_id = fr.flag1 & 0x7F
                    if a_id < len(hit_data) and not hit_data[a_id].is_empty() and a_id not in cands:
                        cands.append(a_id)
            # Extract descriptive hint from sequence comment for this group+idx
            hints = list(anim_hints.get((grp_idx, anim_idx), []))
            cmt_line = ""
            for ln in seq.comment.splitlines():
                if re.search(rf"\b{re.escape(grp_sym)}\s+{anim_idx}\b", ln):
                    cmt_line = ln.strip()
                    break
            if not cmt_line and seq.comment:
                cmt_line = seq.comment.splitlines()[0].strip()
            if cmt_line:
                hints.append(cmt_line)

            # Build readable dropdown label
            hint_short = ""
            if nm:
                hint_short = nm
            elif cmt_line:
                # Try parenthesized name e.g. "(Hadouken)" or routine after colon
                pm = re.search(r"\(([^)]+)\)", cmt_line)
                if pm:
                    hint_short = pm.group(1)
                elif ":" in cmt_line:
                    hint_short = cmt_line.split(":", 1)[1].strip()
            elif hints:
                hint_short = hints[0]
            if not hint_short and cands and cands[0] in atck_names:
                hint_short = atck_names[cands[0]]

            label = f"[{grp_idx:02d}:{anim_idx:02d}] {grp_sym} #{anim_idx} — {seq_sym}"
            if hint_short:
                label += f"  ({hint_short[:48]})"

            matching_whiffs = [
                w
                for w in whiff_gauges
                if w.group == grp_idx and (not w.anim_indices or anim_idx in w.anim_indices)
            ]

            actions.append(
                ActionEntry(
                    group=grp_idx,
                    anim_idx=anim_idx,
                    seq_name=seq_sym,
                    display_label=label,
                    atck_candidates=cands,
                    whiff_entries=matching_whiffs,
                    routine_hints=hints,
                )
            )

    return CharSourceData(
        char_id=char_id,
        header_comment=header_comment,
        spr_prm=spr_prm,
        char_tbl=char_tbl,
        group_tables=group_tables,
        sequences=sequences,
        decl_order=decl_order,
        frame_boxes=frame_boxes,
        hit_boxes=hit_boxes,
        hit_data=hit_data,
        atck_names=atck_names,
        sp_names=sp_names,
        whiff_gauges=whiff_gauges,
        actions=actions,
        projectile_atck_map=proj_atck_map,
        orig_hit_dt_count=orig_hd_count,
    )


# ---------------------------------------------------------------------------
# Binary compilers for Parts 5, 6, 7, 8 (matching SH4 little-endian memory)
# ---------------------------------------------------------------------------


def compile_char_bin(char_data: CharSourceData) -> bytes:
    """Compile CharSourceData into Part 5 binary (exact match to SH4 char_XX.bin)."""
    offsets: Dict[str, int] = {}
    cursor = 0

    spr_prm_name = f"spr_prm_{char_data.char_id:02x}"
    offsets[spr_prm_name] = cursor
    cursor += len(char_data.spr_prm) * 16

    char_tbl_name = f"char_tbl_{char_data.char_id:02x}"
    offsets[char_tbl_name] = cursor
    cursor += len(char_data.char_tbl) * 4

    for kind, sym in char_data.decl_order:
        offsets[sym] = cursor
        if kind == "tbl":
            cursor += len(char_data.group_tables[sym]) * 4
        else:
            cursor += char_data.sequences[sym].byte_size()

    buf = bytearray(cursor)
    pos = 0

    for row in char_data.spr_prm:
        struct.pack_into("<8H", buf, pos, *[v & 0xFFFF for v in row])
        pos += 16

    for sym in char_data.char_tbl:
        val = 0 if sym == "0" else offsets[sym]
        struct.pack_into("<I", buf, pos, val)
        pos += 4

    for kind, sym in char_data.decl_order:
        if kind == "tbl":
            for target in char_data.group_tables[sym]:
                val = 0 if target == "0" else offsets[target]
                struct.pack_into("<I", buf, pos, val)
                pos += 4
        else:
            seq = char_data.sequences[sym]
            for it in seq.items:
                if isinstance(it, CGFrame):
                    b = it.to_bytes()
                    buf[pos : pos + len(b)] = b
                    pos += len(b)
                elif isinstance(it, CGNext):
                    val = 0 if it.target == "0" else offsets[it.target]
                    struct.pack_into("<I", buf, pos, val)
                    pos += 4
                elif isinstance(it, RawWord):
                    struct.pack_into("<I", buf, pos, it.value & 0xFFFFFFFF)
                    pos += 4

    assert pos == cursor
    return bytes(buf)


def compile_frame_box_bin(frame_boxes: List[FrameBox]) -> bytes:
    return b"".join(fb.to_bytes() for fb in frame_boxes)


def compile_hit_box_bin(hit_boxes: List[HitBox]) -> bytes:
    return b"".join(hb.to_bytes() for hb in hit_boxes)


def compile_hit_data_bin(hit_data: List[HitData], min_rows: int = 128) -> bytes:
    raw = b"".join(hd.to_bytes() for hd in hit_data)
    min_bytes = min_rows * 28
    if len(raw) < min_bytes:
        raw += b"\x00" * (min_bytes - len(raw))
    return raw


# ---------------------------------------------------------------------------
# Global code tables: atck_gauge_tbl (src/pl_act.c) & bounce_rules (src/pl_bounce.c)
# ---------------------------------------------------------------------------


def parse_atck_gauge_tbl(root: Path) -> Tuple[List[int], int]:
    """Read normal whiff gauge table `{ L, M, H, Ex }` from src/pl_act.c."""
    p = root / "src" / "pl_act.c"
    lines = p.read_text(encoding="utf-8", errors="replace").splitlines()
    for i, ln in enumerate(lines):
        m = re.search(r"static\s+Sint16\s+atck_gauge_tbl\s*\[\s*4\s*\]\s*=\s*\{([^}]+)\}", ln)
        if m:
            vals = [int(v.strip(), 0) for v in m.group(1).split(",") if v.strip()]
            return vals, i + 1
    return [2, 6, 8, 8], -1


def save_atck_gauge_tbl(root: Path, values: List[int]) -> bool:
    """Update atck_gauge_tbl[4] inside src/pl_act.c."""
    p = root / "src" / "pl_act.c"
    text = p.read_text(encoding="utf-8", errors="replace")
    pat = re.compile(r"(static\s+Sint16\s+atck_gauge_tbl\s*\[\s*4\s*\]\s*=\s*\{)[^}]+(\})")
    new_vals = ", ".join(str(int(v)) for v in values[:4])
    new_text, count = pat.subn(rf"\g<1> {new_vals} \g<2>", text, count=1)
    if count == 0:
        return False
    p.write_text(new_text, encoding="utf-8")
    return True


def save_whiff_gauge_entry(entry: WhiffGaugeEntry, new_amount: int) -> bool:
    """Update a single pl_gauge_add(pl_w, N) call at its exact line in src/s_plXX.c."""
    lines = entry.file_path.read_text(encoding="utf-8", errors="replace").splitlines(keepends=True)
    idx = entry.line_no - 1
    if idx < 0 or idx >= len(lines):
        return False
    updated, count = re.subn(
        r"(pl_gauge_add\s*\(\s*\w+\s*,\s*)-?\d+(\s*\))",
        rf"\g<1>{int(new_amount)}\g<2>",
        lines[idx],
        count=1,
    )
    if count == 0:
        return False
    lines[idx] = updated
    entry.file_path.write_text("".join(lines), encoding="utf-8")
    entry.amount = int(new_amount)
    return True


_BOUNCE_PROP_SYMS = {
    0: "BOUNCE_PROP_NONE",
    1: "BOUNCE_PROP_GROUND",
    2: "BOUNCE_PROP_GROUND_FORCED",
    3: "BOUNCE_PROP_WALL",
    4: "BOUNCE_PROP_WALL_FORCED",
}
_BOUNCE_SYM_TO_ID = {v: k for k, v in _BOUNCE_PROP_SYMS.items()}


def parse_bounce_rules(root: Path) -> List[BounceRule]:
    """Parse bounce_rules[] from src/pl_bounce.c."""
    p = root / "src" / "pl_bounce.c"
    if not p.exists():
        return []
    text = p.read_text(encoding="utf-8", errors="replace")
    m = re.search(r"static\s+const\s+BOUNCE_RULE\s+bounce_rules\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;", text, re.S)
    if not m:
        return []
    rules: List[BounceRule] = []
    for line in m.group(1).splitlines():
        rm = re.search(r"\{([^}]*)\}\s*,?\s*(?:/\*(.*?)\*/)?", line)
        if not rm:
            continue
        parts = [t.strip() for t in rm.group(1).split(",") if t.strip()]
        if len(parts) < 5:
            continue
        pl_s, at_s, air_s, prop_s, nfs_s = parts[:5]
        pl_type = CHAR_SYM_TO_ID.get(pl_s, -1)
        if pl_type < 0:
            pl_type = int(pl_s, 0)
        atck_no = ATCK_NAME_TO_ID.get(at_s, -1)
        if atck_no < 0:
            atck_no = int(at_s, 0)
        need_air = int(air_s, 0)
        prop = _BOUNCE_SYM_TO_ID.get(prop_s, -1)
        if prop < 0:
            prop = int(prop_s, 0)
        no_fly = int(nfs_s, 0)
        height = 0.0
        if len(parts) >= 6:
            h_s = parts[5].rstrip("fF")
            try:
                height = float(h_s)
            except ValueError:
                height = 0.0
        cmt = (rm.group(2) or "").strip()
        rules.append(BounceRule(pl_type, atck_no, need_air, prop, no_fly, height, cmt))
    return rules


def save_bounce_rules(root: Path, rules: List[BounceRule]) -> bool:
    """Write updated bounce_rules[] back into src/pl_bounce.c."""
    p = root / "src" / "pl_bounce.c"
    if not p.exists():
        return False
    text = p.read_text(encoding="utf-8", errors="replace")
    pat = re.compile(r"(static\s+const\s+BOUNCE_RULE\s+bounce_rules\s*\[\s*\]\s*=\s*\{)(.*?)(\}\s*;)", re.S)
    lines: List[str] = [""]
    for r in rules:
        if r.prop == 0:
            continue
        char_name, char_sym = CHAR_BY_ID.get(r.pl_type, (f"pl{r.pl_type:02x}", str(r.pl_type)))
        atck_sym = STANDARD_ATCK_NAMES.get(r.atck_no, str(r.atck_no))
        prop_sym = _BOUNCE_PROP_SYMS.get(r.prop, str(r.prop))
        cmt = r.comment or f"{char_name} atck 0x{r.atck_no:02x}"
        lines.append(
            f"\t{{ {char_sym:<18s}, {atck_sym:<14s}, {r.need_air}, {prop_sym:<24s}, {r.no_fly_screen}, {r.height:.2f}f }},\t/* {cmt} */"
        )
    lines.append("")
    replacement_body = "\n".join(lines)
    new_text, count = pat.subn(rf"\g<1>{replacement_body}\g<3>", text, count=1)
    if count == 0:
        return False
    p.write_text(new_text, encoding="utf-8")
    return True


def parse_cancel_rules(root: Path) -> List[CancelRule]:
    """Parse cancel_rules[] between CANCEL_RULES_BEGIN / END in src/pl_com.c."""
    p = root / "src" / "pl_com.c"
    if not p.exists():
        return []
    text = p.read_text(encoding="utf-8", errors="replace")
    m = re.search(r"/\*\s*CANCEL_RULES_BEGIN\s*\*/(.*?)/\*\s*CANCEL_RULES_END\s*\*/", text, re.S)
    if not m:
        return []
    rules: List[CancelRule] = []
    for line in m.group(1).splitlines():
        rm = re.search(r"\{([^}]*)\}\s*,?\s*(?:/\*(.*?)\*/)?", line)
        if not rm:
            continue
        parts = [t.strip() for t in rm.group(1).split(",") if t.strip()]
        if len(parts) < 4:
            continue
        pl_s, kind_s, id_s, fl_s = parts[:4]
        pl_type = CHAR_SYM_TO_ID.get(pl_s, -1)
        if pl_type < 0:
            pl_type = int(pl_s, 0)
        if pl_type == 0xFF:
            continue
        act_kind = int(kind_s, 0)
        move_id = ATCK_NAME_TO_ID.get(id_s, -1)
        if move_id < 0:
            move_id = int(id_s, 0)
        flags = int(fl_s, 0)
        cmt = (rm.group(2) or "").strip()
        rules.append(CancelRule(pl_type, act_kind, move_id, flags, cmt))
    return rules


def save_cancel_rules(root: Path, rules: List[CancelRule]) -> bool:
    """Write updated cancel_rules[] between CANCEL_RULES_BEGIN / END in src/pl_com.c."""
    p = root / "src" / "pl_com.c"
    if not p.exists():
        return False
    text = p.read_text(encoding="utf-8", errors="replace")
    pat = re.compile(r"(/\*\s*CANCEL_RULES_BEGIN\s*\*/)(.*?)(/\*\s*CANCEL_RULES_END\s*\*/)", re.S)
    lines: List[str] = [""]
    for r in rules:
        if r.flags == 0:
            continue
        char_name, char_sym = CHAR_BY_ID.get(r.pl_type, (f"pl{r.pl_type:02x}", str(r.pl_type)))
        id_str = STANDARD_ATCK_NAMES.get(r.move_id, f"0x{r.move_id:02x}") if r.act_kind == 0 else str(r.move_id)
        cmt = r.comment or f"{char_name} {'sp' if r.act_kind == 1 else 'atck'} {id_str}"
        lines.append(
            f"\t{{ {char_sym:<18s}, {r.act_kind}, {id_str:<14s}, 0x{r.flags:02x} }},\t/* {cmt} */"
        )
    lines.append("\t")
    replacement_body = "\n".join(lines)
    new_text, count = pat.subn(rf"\g<1>{replacement_body}\g<3>", text, count=1)
    if count == 0:
        return False
    p.write_text(new_text, encoding="utf-8")
    return True


def parse_chain_tables(root: Path, char_id: int) -> Dict[str, List[int]]:
    """Read normal chain tables (lp_tbl, mp_tbl, ..., sj_hk_tbl) from src/s_plXX.c."""
    p = root / "src" / f"s_pl{char_id:02x}.c"
    if not p.exists():
        return {}
    text = _strip_c_comments(p.read_text(encoding="utf-8", errors="replace"))
    out: Dict[str, List[int]] = {}
    for m in re.finditer(r"static\s+Sint8\s+(\w+_tbl)\s*\[\s*4\s*\]\s*=\s*\{([^}]*)\}", text):
        vals = [int(v.strip(), 0) for v in m.group(2).split(",") if v.strip()]
        if len(vals) == 4:
            out[m.group(1)] = vals
    return out


# ---------------------------------------------------------------------------
# Symbolic C Serializers for data/*XX.c
# ---------------------------------------------------------------------------


def _fmt_damage_type(v: int) -> str:
    dm = v & 0x1F
    if dm == 0:
        parts = ["ZU"]
    elif dm == 1:
        parts = ["BU_O"]
    elif dm == 4:
        parts = ["AS"]
    else:
        parts = [str(dm)]
    if v & 0x40:
        parts.append("NO_VEC")
    if v & 0x80:
        parts.append("NO_ZU")
    if v & 0x20:
        parts.append("FLY_SCREEN")
    return " | ".join(parts)


def _fmt_atck_kind(v: int) -> str:
    guard = v & 0x0F
    g_map = {0: "A_NOM", 2: "A_HI", 4: "A_LO", 8: "A_GUARD_NG"}
    parts: List[str] = []
    if guard in g_map:
        if guard != 0 or v == 0:
            parts.append(g_map[guard])
    else:
        parts.append(f"0x{guard:02x}")
    if v & 0x10:
        parts.append("A_SPECIAL")
    if v & 0x20:
        parts.append("A_HYPER")
    extra = v & 0xC0
    if extra:
        parts.append(f"0x{extra:02x}")
    return " | ".join(parts) if parts else "0"


def _fmt_damage_kind(v: int) -> str:
    hi = v & 0xF0
    lo = v & 0x0F
    k_map = {
        0x10: "K_S",
        0x20: "K_M",
        0x30: "K_L",
        0x90: "K_LS",
        0xA0: "K_LM",
        0xB0: "K_LL",
    }
    parts: List[str] = []
    if hi in k_map:
        parts.append(k_map[hi])
    elif hi:
        parts.append(f"0x{hi:02x}")
    if lo & 0x08:
        parts.append("K_BACK_MAX")
    elem = lo & 0x07
    if elem:
        parts.append(str(elem))
    return " | ".join(parts) if parts else "0"


def _fmt_hit_mark(v: int) -> str:
    return {0: "MARK_N1", 1: "MARK_N2", 2: "MARK_N3"}.get(v, str(v))


def _fmt_score(v: int) -> str:
    return {4: "SC100", 8: "SC200", 12: "SC300"}.get(v, str(v))


def _fmt_hit_se(v: int) -> str:
    return {
        5: "SD_NOR_S",
        6: "SD_NOR_M",
        7: "SD_NOR_L",
        9: "SD_KIK_S",
        10: "SD_KIK_M",
        11: "SD_KIK_L",
    }.get(v, str(v))


def _fmt_hit_flag(v: int) -> str:
    if v == 0:
        return "0"
    parts: List[str] = []
    for bit, sym in (
        (0x02, "NO_CHIP"),
        (0x04, "NO_WEIGHT"),
        (0x08, "NO_SCALE"),
        (0x10, "NO_COMBO"),
        (0x20, "NO_KO"),
        (0x40, "BLACK_COMBO"),
        (0x80, "AERIAL_FINISH"),
    ):
        if v & bit:
            parts.append(sym)
    rem = v & 0x01
    if rem:
        parts.append(f"0x{rem:02x}")
    return " | ".join(parts) if parts else "0"


def _fmt_cpu_ans(v: int) -> str:
    return {0: "YK_GD030", 1: "YK_GD060"}.get(v, str(v))


def save_hit_c(root: Path, data: CharSourceData) -> Path:
    char_name, _ = CHAR_BY_ID.get(data.char_id, (f"pl{data.char_id:02x}", ""))
    p = root / "data" / f"hit_{data.char_id:02x}.c"
    lines = [
        f"/* {char_name} (character {data.char_id:02x}): the boxes (HIT_BOX, 8 bytes).",
        "   Edited by Move Property Editor (mvc2-naomi-dc/tools/move_editor). */",
        '#include "player.h"',
        "",
        f"const HIT_BOX hit_{data.char_id:02x}[] = {{",
        "\t/*    x     w     y     h */",
    ]
    for i, b in enumerate(data.hit_boxes):
        extra = ""
        if PUSH_BOX_BASE <= i < PUSH_BOX_BASE + 4:
            st_names = ["PUSH_BOX+STANCE_STAND", "PUSH_BOX+STANCE_CROUCH", "PUSH_BOX+STANCE_AIR", "PUSH_BOX+STANCE_DOWN"]
            extra = f" ({st_names[i - PUSH_BOX_BASE]})"
        lines.append(f"\t{{ {b.x:4d}, {b.w:4d}, {b.y:4d}, {b.h:4d} }},\t/* {i}{extra} */")
    lines.append("};")
    lines.append("")
    p.write_text("\n".join(lines), encoding="utf-8")
    return p


def save_frame_box_c(root: Path, data: CharSourceData) -> Path:
    char_name, _ = CHAR_BY_ID.get(data.char_id, (f"pl{data.char_id:02x}", ""))
    p = root / "data" / f"frame_box{data.char_id:02x}.c"

    def fmt_b(val: int, is_atck2: bool = False, is_body3: bool = False) -> str:
        u = val & 0xFFFF
        if is_atck2 and (u & 0x8000):
            return f"0x8000 | {u & BOX_MASK}"
        if is_body3 and (u & 0xE000):
            return f"0x{u & 0xE000:04x} | {u & BOX_MASK}"
        return f"0x{u:04x}" if u >= 0x8000 else str(u)

    lines = [
        f"/* {char_name} (character {data.char_id:02x}): the frame boxes (FRAME_BOX, 16 bytes).",
        "   Edited by Move Property Editor (mvc2-naomi-dc/tools/move_editor). */",
        '#include "player.h"',
        "",
        f"const FRAME_BOX frame_box_{data.char_id:02x}[] = {{",
        "\t/* atck         atck2  body0  body1  body2         body3  catch  catch2 */",
    ]
    for i, fb in enumerate(data.frame_boxes):
        a1 = fmt_b(fb.atck_box)
        a2 = fmt_b(fb.atck_box2, is_atck2=True)
        b0 = fmt_b(fb.body_box0)
        b1 = fmt_b(fb.body_box1)
        b2 = fmt_b(fb.body_box2)
        b3 = fmt_b(fb.body_box3, is_body3=True)
        cb = fmt_b(fb.catch_box)
        cb2 = fmt_b(fb.catch_box2)
        lines.append(f"\t{{ {a1:>4s}, {a2:>12s}, {b0:>5s}, {b1:>5s}, {b2:>5s}, {b3:>12s}, {cb:>5s}, {cb2:>6s} }},\t/* {i} */")
    lines.append("};")
    lines.append("")
    p.write_text("\n".join(lines), encoding="utf-8")
    return p


def save_hit_dt_c(root: Path, data: CharSourceData) -> Path:
    char_name, _ = CHAR_BY_ID.get(data.char_id, (f"pl{data.char_id:02x}", ""))
    p = root / "data" / f"hit_dt{data.char_id:02x}.c"
    lines = [
        f"/* {char_name} (character {data.char_id:02x}): the attacks (HIT_DATA, 0x1c bytes).",
        "   Edited by Move Property Editor (mvc2-naomi-dc/tools/move_editor). */",
        '#include "player.h"',
        '#include "hit_equ.h"',
        "",
        f"const HIT_DATA hit_dt_{data.char_id:02x}[] = {{",
        "\t/* dmg                   type          kind      elem  piyo  stop  sand  sleep   vec  pow  range     mark  score        se  shc  jug  box  box2                     flag  13  guard  15  catch  abyss  cnt    answer        1a */",
    ]
    last_nonempty = 0
    for i, hd in enumerate(data.hit_data):
        if not hd.is_empty():
            last_nonempty = i
    write_count = max(data.orig_hit_dt_count, last_nonempty + 1)
    for i in range(write_count):
        hd = data.hit_data[i]
        cmt = hd.comment if hd.comment else f"{i:02x}"
        if not cmt.lower().startswith(f"{i:02x}"):
            cmt = f"{i:02x}: {cmt}"
        if hd.is_empty():
            lines.append(f"\t{{ 0 }},\t/* {cmt} */")
            continue
        vec_u = hd.vector & 0xFF
        lines.append(
            f"\t{{ {hd.damage:3d}, {_fmt_damage_type(hd.damage_type):>21s}, {_fmt_atck_kind(hd.atck_kind):>12s}, "
            f"{_fmt_damage_kind(hd.damage_kind):>10s}, {hd.piyo:4d}, {hd.hit_stop:4d}, {hd.sandbag_time:4d}, {hd.sleep_time:5d}, "
            f"0x{vec_u:02x}, {hd.power:3d}, 0x{hd.atck_range:02x}, {_fmt_hit_mark(hd.hit_mark):>7s}, {_fmt_score(hd.score):>5s}, "
            f"{_fmt_hit_se(hd.hit_se):>8s}, {hd.shc:3d}, {hd.jug:3d}, {hd.range_box:3d}, {hd.range_box2:4d}, "
            f"{_fmt_hit_flag(hd.hit_flag):>25s}, {hd.unk_13:2d}, {hd.guard_time:5d}, {hd.unk_15:2d}, "
            f"{hd.catch_ng_time:5d}, {hd.abyss_jug:5d}, {hd.hit_cnt:3d}, {_fmt_cpu_ans(hd.cpu_answer_no):>8s}, "
            f"{{ {hd.unk_1a_0}, {hd.unk_1a_1} }} }},\t/* {cmt} */"
        )
    lines.append("};")
    lines.append("")
    p.write_text("\n".join(lines), encoding="utf-8")
    return p


def _fmt_cg_byte_hex(v: int) -> str:
    return f"0x{v & 0xFF:02x}"


def _fmt_end_flag(v: int) -> str:
    if v == 0:
        return "0"
    if v == 0x80:
        return "END_OF_DATA"
    parts = ["END_OF_DATA"] if (v & 0x80) else []
    if v & 0x7F:
        parts.append(f"0x{v & 0x7F:02x}")
    return " | ".join(parts)


def _fmt_atck_phase(v: int) -> str:
    if v == 0:
        return "0"
    parts = []
    if v & 0x80:
        parts.append("ATK")
    if v & 0x40:
        parts.append("ATE")
    if v & 0x20:
        parts.append("CG_ATCK_NG")
    rem = v & 0x1F
    if rem:
        parts.append(str(rem) if rem < 10 else f"0x{rem:02x}")
    return " | ".join(parts)


def _fmt_se(v: int) -> str:
    if v == 0:
        return "0"
    if v & 0x80:
        lo = v & 0x7F
        return f"0x{lo:02x} | CSE" if lo else "CSE"
    return f"0x{v:02x}"


def save_char_c(root: Path, data: CharSourceData) -> Path:
    p = root / "data" / f"char_{data.char_id:02x}.c"
    lines: List[str] = []
    if data.header_comment:
        lines.append(f"/*{data.header_comment}*/")
    else:
        char_name, _ = CHAR_BY_ID.get(data.char_id, (f"pl{data.char_id:02x}", ""))
        lines.append(f"/* {char_name} (character {data.char_id:02x}): the animations. */")
    lines.append('#include "player.h"')
    lines.append('#include "char_tbl.h"')
    lines.append("")

    # Forward extern declarations
    seq_names = [sym for k, sym in data.decl_order if k == "seq"]
    tbl_names = [sym for k, sym in data.decl_order if k == "tbl"]
    if seq_names:
        chunks = [f"{s}[]" for s in seq_names]
        for i in range(0, len(chunks), 6):
            prefix = "extern const Sint32 " if i == 0 else "\t"
            sep = ";" if i + 6 >= len(chunks) else ","
            lines.append(prefix + ", ".join(chunks[i : i + 6]) + sep)
    if tbl_names:
        chunks = [f"*const {t}[]" for t in tbl_names]
        for i in range(0, len(chunks), 5):
            prefix = "extern const Sint32 " if i == 0 else "\t"
            sep = ";" if i + 5 >= len(chunks) else ","
            lines.append(prefix + ", ".join(chunks[i : i + 5]) + sep)
    lines.append("")

    # spr_prm
    lines.append("/* the sprites' sizes and turns, by a frame's zoom number */")
    lines.append(f"const SPR_PRM spr_prm_{data.char_id:02x}[{len(data.spr_prm)}] = {{")
    for row in data.spr_prm:
        r_fmt = [str(to_s16(row[i]) if i in (3, 4) else row[i]) for i in range(8)]
        lines.append("\t{ " + ", ".join(f"{x:>4s}" for x in r_fmt) + " },")
    lines.append("};")
    lines.append("")

    # char_tbl
    lines.append("/* the groups */")
    lines.append(f"const Sint32 *const *const char_tbl_{data.char_id:02x}[] = {{")
    for i in range(0, len(data.char_tbl), 6):
        lines.append("\t" + ", ".join(data.char_tbl[i : i + 6]) + ",")
    lines.append("};")
    lines.append("")

    # First group tables, then sequences, or strictly in decl_order?
    # Note: strictly in decl_order preserves compilation layout identically!
    # In Capcom's C files, all group_tables come right after char_tbl, then sequences.
    for kind, sym in data.decl_order:
        if kind == "tbl":
            ptrs = data.group_tables[sym]
            lines.append(f"const Sint32 *const {sym}[] = {{")
            for i in range(0, len(ptrs), 6):
                lines.append("\t" + ", ".join(ptrs[i : i + 6]) + ",")
            lines.append("};")
            lines.append("")
        else:
            seq = data.sequences[sym]
            if seq.comment:
                lines.append(f"/* {seq.comment} */")
            lines.append(f"const Sint32 {sym}[] = {{")
            for it in seq.items:
                if isinstance(it, CGFrame):
                    if it.kind == "EF_FRAME":
                        lines.append(
                            f"\tEF_FRAME( {to_s8(it.prm):4d}, {to_s8(it.ftype):4d}, {to_s8(it.ctr):4d}, "
                            f"{_fmt_end_flag(it.end):>11s}, {it.spr:5d} ),"
                        )
                    else:
                        lines.append(
                            f"\tCG_FRAME( {to_s8(it.prm):4d}, {to_s8(it.ftype):4d}, {to_s8(it.ctr):4d}, "
                            f"{_fmt_end_flag(it.end):>11s}, {it.spr:5d}, {_fmt_cg_byte_hex(it.prio):>4s}, "
                            f"{_fmt_cg_byte_hex(it.shadow):>6s}, {_fmt_atck_phase(it.atck):>16s}, "
                            f"{_fmt_cg_byte_hex(it.flag1):>5s}, {_fmt_se(it.se):>10s}, "
                            f"{it.zoom:4d}, {it.unk:3d}, {it.flag2:5d}, "
                            f"{('0x%04x' % it.rival) if it.rival >= 10 else str(it.rival):>6s}, {it.box:3d} ),"
                        )
                elif isinstance(it, CGNext):
                    lines.append(f"\tCG_NEXT( {it.target} ),")
                elif isinstance(it, RawWord):
                    lines.append(f"\t0x{it.value:08x},")
            lines.append("};")
            lines.append("")

    p.write_text("\n".join(lines), encoding="utf-8")
    return p
