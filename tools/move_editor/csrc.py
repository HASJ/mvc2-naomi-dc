"""Tiny, best-effort C source helpers used to understand which animation states belong to which move.

This is not a C parser. It understands exactly the decompiled-source style used by `src/s_plXX.c` and
`src/shlXX_YY.c`: tab-indented one-statement-per-line code, `static void (*tbl[])(...) = {...}` jump tables
and `pl_char_set( pl_w, Group, no )` calls. Anything it cannot evaluate is skipped rather than guessed.
"""

from __future__ import annotations

import itertools
import re
from dataclasses import dataclass, field
from typing import Dict, Iterable, Iterator, List, Optional, Sequence, Set, Tuple

# ---------------------------------------------------------------------------
# Comment stripping
# ---------------------------------------------------------------------------

_BLOCK_RE = re.compile(r"/\*.*?\*/", re.S)
_LINE_RE = re.compile(r"//[^\n]*")


def strip_comments(text: str) -> str:
    """Remove C comments while preserving line numbers (newlines are kept)."""

    def blank(m: "re.Match[str]") -> str:
        return re.sub(r"[^\n]", " ", m.group(0))

    return _LINE_RE.sub(blank, _BLOCK_RE.sub(blank, text))


# ---------------------------------------------------------------------------
# Functions and jump tables
# ---------------------------------------------------------------------------


@dataclass
class CFunc:
    name: str
    line_start: int
    lines: List[Tuple[int, str]]  # (1-based file line, code without comments)

    @property
    def text(self) -> str:
        return "\n".join(t for _, t in self.lines)


@dataclass
class CTable:
    name: str
    rows: List[List[str]]  # 1-D tables have a single row
    entry_comments: List[List[str]] = field(default_factory=list)  # raw `/* ... */` after each entry
    pos: int = 0  # character offset of the table definition inside the stripped text
    end: int = 0


_FUNC_HEAD_RE = re.compile(r"^(?:static\s+)?(?:const\s+)?[A-Za-z_][\w \t\*]*?[ \t\*](\w+)\s*\(")
_NOT_FUNC = {"if", "for", "while", "switch", "return", "typedef", "struct", "sizeof"}


def parse_functions(stripped: str) -> Dict[str, CFunc]:
    """Return every function *definition* (not prototype) in the stripped source."""
    lines = stripped.split("\n")
    funcs: Dict[str, CFunc] = {}
    n = len(lines)
    i = 0
    while i < n:
        line = lines[i]
        if line and not line[0].isspace() and line[0] not in "#{}" and not line.rstrip().endswith(";"):
            m = _FUNC_HEAD_RE.match(line)
            if m and m.group(1) not in _NOT_FUNC:
                j = i + 1
                while j < n and not lines[j].strip():
                    j += 1
                if j < n and lines[j].lstrip().startswith("{"):
                    depth = 0
                    body: List[Tuple[int, str]] = []
                    k = j
                    while k < n:
                        t = lines[k]
                        depth += t.count("{") - t.count("}")
                        body.append((k + 1, t))
                        if depth <= 0:
                            break
                        k += 1
                    funcs[m.group(1)] = CFunc(m.group(1), i + 1, body)
                    i = k + 1
                    continue
        i += 1
    return funcs


_TABLE_HEAD_RE = re.compile(r"\(\s*\*\s*(\w+)\s*(?:\[[^\]]*\]\s*)+\)\s*\([^)]*\)\s*=\s*\{")


def _match_brace(text: str, open_pos: int) -> int:
    depth = 0
    for idx in range(open_pos, len(text)):
        ch = text[idx]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return idx
    return len(text) - 1


def parse_tables(stripped: str, raw: Optional[str] = None) -> Dict[str, CTable]:
    """Find `(*name[])(...) = { a, b, ... }` function-pointer tables (1-D or 2-D).

    `raw` is the unstripped source (same offsets as `stripped`); when given, the comment trailing each
    entry (e.g. `/* 00 Hadouken */`) is returned in `entry_comments`.
    """
    out: Dict[str, CTable] = {}
    for m in _TABLE_HEAD_RE.finditer(stripped):
        start = m.end() - 1
        end = _match_brace(stripped, start)
        body = stripped[start + 1 : end]
        rows: List[List[str]] = []
        cmts: List[List[str]] = []
        if "{" in body:
            for rm in re.finditer(r"\{([^{}]*)\}", body):
                rows.append([t.strip() for t in rm.group(1).split(",") if t.strip()])
                cmts.append([])
        else:
            rows.append([t.strip() for t in body.split(",") if t.strip()])
            cmts.append([])
            if raw is not None:
                entry_cmts: List[str] = []
                for em in re.finditer(r"/\*(.*?)\*/|([A-Za-z_]\w*)", raw[start + 1 : end], re.S):
                    if em.group(2):
                        entry_cmts.append("")
                    elif entry_cmts:
                        entry_cmts[-1] = f"{entry_cmts[-1]} {em.group(1).strip()}".strip()
                cmts[0] = entry_cmts
        out[m.group(1)] = CTable(m.group(1), rows, cmts, pos=m.start(), end=end)
    return out


# ---------------------------------------------------------------------------
# Expression evaluation
# ---------------------------------------------------------------------------


class Unknown(Exception):
    """Raised when an expression cannot be evaluated from what is known statically."""


_TOKEN_RE = re.compile(
    r"\s*(0[xX][0-9a-fA-F]+|\d+\.\d*(?:[eE][-+]?\d+)?f?|\d+|[A-Za-z_]\w*|<<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^~!<>?:(),\[\]])"
)
_CAST_RE = re.compile(r"\(\s*(?:const\s+)?(?:Sint\d+|Uint\d+|int|short|char|long|unsigned)\s*\)")


def normalize_expr(expr: str) -> str:
    """`pl_w->char_no.b.no + 1` -> `char_no_b_no + 1` (object prefixes dropped, member dots flattened)."""
    e = _CAST_RE.sub("", expr)
    e = re.sub(r"\b\w+\s*->\s*", "", e)
    e = re.sub(r"(?<=\w)\s*\.\s*(?=[A-Za-z_])", "_", e)
    return e.strip()


def _tokenize(expr: str) -> List[str]:
    toks: List[str] = []
    pos = 0
    expr = expr.strip()
    while pos < len(expr):
        m = _TOKEN_RE.match(expr, pos)
        if not m:
            raise Unknown(expr)
        toks.append(m.group(1))
        pos = m.end()
    return toks


class _Parser:
    PREC = [
        ["||"],
        ["&&"],
        ["|"],
        ["^"],
        ["&"],
        ["==", "!="],
        ["<", "<=", ">", ">="],
        ["<<", ">>"],
        ["+", "-"],
        ["*", "/", "%"],
    ]

    def __init__(self, toks: List[str]) -> None:
        self.t = toks
        self.i = 0

    def peek(self) -> Optional[str]:
        return self.t[self.i] if self.i < len(self.t) else None

    def eat(self, tok: Optional[str] = None) -> str:
        cur = self.peek()
        if cur is None or (tok is not None and cur != tok):
            raise Unknown("syntax")
        self.i += 1
        return cur

    def parse(self):
        node = self.ternary()
        if self.peek() is not None:
            raise Unknown("trailing")
        return node

    def ternary(self):
        cond = self.binary(0)
        if self.peek() == "?":
            self.eat("?")
            a = self.ternary()
            self.eat(":")
            b = self.ternary()
            return ("tern", cond, a, b)
        return cond

    def binary(self, level: int):
        if level >= len(self.PREC):
            return self.unary()
        node = self.binary(level + 1)
        while self.peek() in self.PREC[level]:
            op = self.eat()
            node = ("bin", op, node, self.binary(level + 1))
        return node

    def unary(self):
        tok = self.peek()
        if tok in ("-", "!", "~", "+"):
            self.eat()
            return ("un", tok, self.unary())
        return self.primary()

    def primary(self):
        tok = self.eat()
        if tok == "(":
            node = self.ternary()
            self.eat(")")
            return node
        if re.fullmatch(r"0[xX][0-9a-fA-F]+", tok):
            return ("num", int(tok, 16))
        if re.fullmatch(r"\d+", tok):
            return ("num", int(tok))
        if re.match(r"\d", tok):
            raise Unknown("float")
        if re.fullmatch(r"[A-Za-z_]\w*", tok):
            if self.peek() in ("(", "["):
                raise Unknown("call")
            return ("var", tok)
        raise Unknown(tok)


def _eval(node, env: Dict[str, int]) -> int:
    kind = node[0]
    if kind == "num":
        return node[1]
    if kind == "var":
        if node[1] in env:
            return env[node[1]]
        raise Unknown(node[1])
    if kind == "un":
        v = _eval(node[2], env)
        return {"-": -v, "!": int(not v), "~": ~v, "+": v}[node[1]]
    if kind == "tern":
        return _eval(node[2], env) if _eval(node[1], env) else _eval(node[3], env)
    op = node[1]
    if op == "&&":
        return int(bool(_eval(node[2], env)) and bool(_eval(node[3], env)))
    if op == "||":
        return int(bool(_eval(node[2], env)) or bool(_eval(node[3], env)))
    a, b = _eval(node[2], env), _eval(node[3], env)
    if op in ("/", "%") and b == 0:
        raise Unknown("div0")
    return {
        "|": a | b,
        "^": a ^ b,
        "&": a & b,
        "==": int(a == b),
        "!=": int(a != b),
        "<": int(a < b),
        "<=": int(a <= b),
        ">": int(a > b),
        ">=": int(a >= b),
        "<<": a << b,
        ">>": a >> b,
        "+": a + b,
        "-": a - b,
        "*": a * b,
        "/": int(a / b) if b else 0,
        "%": a % b if b else 0,
    }[op]


def c_eval(expr: str, env: Dict[str, int]) -> int:
    """Evaluate an integer C expression with names from `env`. Raises `Unknown` if impossible."""
    toks = _tokenize(normalize_expr(expr))
    if not toks:
        raise Unknown("empty")
    return _eval(_Parser(toks).parse(), env)


def split_top_level(text: str, sep: str = ",") -> List[str]:
    parts: List[str] = []
    depth = 0
    start = 0
    for i, ch in enumerate(text):
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif ch == sep and depth == 0:
            parts.append(text[start:i].strip())
            start = i + 1
    tail = text[start:].strip()
    if tail:
        parts.append(tail)
    return parts


def balanced_paren(text: str, open_idx: int) -> int:
    """Index of the `)` matching the `(` at `open_idx`, or -1."""
    depth = 0
    for i in range(open_idx, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i
    return -1


# ---------------------------------------------------------------------------
# Versions (button strengths) and event extraction
# ---------------------------------------------------------------------------

Ver = Tuple[int, int]  # (atck_level: 0 light / 1 heavy, atck_pk: 0 punch / 1 kick)


@dataclass
class Event:
    kind: str  # "atck" | "char" | "spawn" | "timer" | "vercode"
    func: str
    line: int
    ver: Ver
    value: int = 0  # atck_no, anim number, timer value
    group: int = -1  # char: animation group
    name: str = ""  # spawn: effect function name; timer: field name
    args: Tuple[int, ...] = ()  # spawn: evaluated arguments after the work pointer
    tag: Tuple[Tuple[str, int], ...] = ()  # extra variable bindings used (e.g. sub_type)
    obj: str = ""  # work pointer the event applies to (`pl_w`, `ew`, ...)
    text: str = ""  # original source line, for notes


_IF_RE = re.compile(r"^(?:\}\s*)?(?:else\s+)?if\s*\(")
_ASSIGN_RE = re.compile(r"^(?:(\w+)\s*->\s*)?([A-Za-z_][\w\.]*)\s*(?:->\s*([\w\.]+))?\s*=(?!=)\s*(.+?)\s*;?\s*$")
_CHAR_SET_RE = re.compile(r"\b(?:pl_char_set|ef_char_set|ef_char_set2)\s*\(\s*(\w+)\s*,\s*(.+)\)\s*;")
_SPAWN_RE = re.compile(r"\b((?:shl|pls)[0-9a-fA-F]{2}_[0-9a-fA-F]{2})(\w*?)_set\s*\(([^;]*)\)\s*;")
_TIMER_RE = re.compile(r"^(?:\w+\s*->\s*)?(timer\d|cancel_timer)\s*=(?!=)\s*(.+?)\s*;")
_VER_NAMES = ("atck_level", "atck_pk")
_RETURN_RE = re.compile(r"\breturn\b")


def _uses_version(expr: str) -> bool:
    return any(v in expr for v in _VER_NAMES)


class FunctionWalker:
    """Walk one function's lines in order, tracking per-version variable values and branch scopes."""

    def __init__(
        self,
        defs: Dict[str, int],
        versions: Sequence[Ver],
        extra: Optional[Dict[str, Sequence[int]]] = None,
    ) -> None:
        self.defs = defs
        self.versions = list(versions)
        self.extra = dict(extra or {})
        self.env: Dict[Ver, Dict[str, int]] = {}
        for v in self.versions:
            self.env[v] = {"atck_level": v[0], "atck_pk": v[1]}

    def _combos(self) -> List[Dict[str, int]]:
        if not self.extra:
            return [{}]
        keys = list(self.extra)
        return [dict(zip(keys, vals)) for vals in itertools.product(*(self.extra[k] for k in keys))]

    def _env_for(self, v: Ver, combo: Dict[str, int]) -> Dict[str, int]:
        e = dict(self.defs)
        e.update(self.env[v])
        e.update(combo)
        return e

    def _cond_set(self, cond: str) -> Optional[Set[Ver]]:
        """Versions for which `cond` is true, or None if it does not depend on the button (or is unknown)."""
        if not _uses_version(cond):
            return None
        out: Set[Ver] = set()
        for v in self.versions:
            try:
                if c_eval(cond, self._env_for(v, {})):
                    out.add(v)
            except Unknown:
                return None
        return out

    def walk(self, fn: CFunc, base_allowed: Optional[Set[Ver]] = None) -> List[Event]:
        events: List[Event] = []
        allowed_base: Set[Ver] = set(base_allowed) if base_allowed is not None else set(self.versions)
        scopes: List[Tuple[int, Optional[Set[Ver]], Optional[Set[Ver]], bool]] = []  # depth, restrict, cond, has_return
        pending_else: Optional[Set[Ver]] = None
        depth = 0

        def current_allowed() -> Set[Ver]:
            allowed = set(allowed_base)
            for _, restrict, _, _ in scopes:
                if restrict is not None:
                    allowed &= restrict
            return allowed

        for lineno, raw_line in fn.lines:
            line = raw_line.strip()
            if not line:
                continue
            opens = line.count("{")
            closes = line.count("}")

            starts_close = line.startswith("}")
            if starts_close:
                for _ in range(closes if closes else 1):
                    depth -= 1
                    popped = None
                    while scopes and scopes[-1][0] > depth:
                        popped = scopes.pop()
                    if popped is not None and popped[2] is not None:
                        # block of a version-dependent `if`: remember for an `else` and for an early `return`
                        pending_else = popped[2]
                        if popped[3] and depth == 1 and not re.match(r"^\}\s*else", line):
                            allowed_base &= set(self.versions) - popped[2]
                            pending_else = None
                    elif popped is not None:
                        pending_else = None
                rest = line.lstrip("}").strip()
                if rest.startswith("else") and pending_else is not None:
                    restrict_else = set(self.versions) - pending_else
                    m_ei = re.match(r"else\s+if\s*\(", rest)
                    if m_ei:
                        cond_end = balanced_paren(rest, rest.index("("))
                        cset = self._cond_set(rest[rest.index("(") + 1 : cond_end]) if cond_end > 0 else None
                        restrict_else = restrict_else & cset if cset is not None else restrict_else
                    scopes.append((depth + 1, restrict_else, None, False))
                    depth += 1
                    pending_else = None
                    continue
                if rest.startswith("else"):
                    scopes.append((depth + 1, None, None, False))
                    depth += 1
                    pending_else = None
                    continue
                line = rest
                opens = line.count("{")
                if not line:
                    continue
            else:
                depth_before = depth

            if line.startswith("else") and pending_else is not None and not starts_close:
                restrict_else = set(self.versions) - pending_else
                pending_else = None
                body = line[4:].strip()
                if body.startswith("{") or not body:
                    scopes.append((depth + 1, restrict_else, None, False))
                    depth += 1
                    continue
                self._handle_statement_text(body, lineno, fn, restrict_else & current_allowed(), events)
                continue

            m_if = _IF_RE.match(line)
            if m_if:
                open_idx = line.index("(", m_if.start())
                close_idx = balanced_paren(line, open_idx)
                if close_idx > 0:
                    cond = line[open_idx + 1 : close_idx]
                    rest = line[close_idx + 1 :].strip()
                    cset = self._cond_set(cond)
                    if rest == "" or rest == "{":
                        scopes.append((depth + 1, cset, cset, False))
                        depth += 1
                        pending_else = None
                        continue
                    # same-line statement(s): `if( c ) A ; [else B ;]`
                    then_txt, else_txt = rest, ""
                    em = re.search(r";\s*else\b", rest)
                    if em:
                        then_txt = rest[: em.start() + 1]
                        else_txt = rest[em.end() :].strip()
                    allowed = current_allowed()
                    if cset is None:
                        self._handle_statement_text(then_txt, lineno, fn, allowed, events)
                        if else_txt:
                            self._handle_statement_text(else_txt, lineno, fn, allowed, events)
                    else:
                        self._handle_statement_text(then_txt, lineno, fn, allowed & cset, events)
                        if else_txt:
                            self._handle_statement_text(else_txt, lineno, fn, allowed - cset, events)
                        elif _RETURN_RE.search(then_txt) and len(scopes) == 0:
                            allowed_base &= set(self.versions) - cset
                    pending_else = cset if not else_txt else None
                    continue

            if line == "{":
                depth += 1
                scopes.append((depth, None, None, False))
                continue

            if re.match(r"^else\b", line):
                # `else stmt ;` after a one-line `if` whose condition did not depend on the button
                line = line[4:].strip()
                pending_else = None
                if not line:
                    continue
                if line.startswith("{"):
                    depth += 1
                    scopes.append((depth, None, None, False))
                    continue

            allowed = current_allowed()
            if _RETURN_RE.search(line) and scopes:
                top = scopes[-1]
                scopes[-1] = (top[0], top[1], top[2], True)
            self._handle_statement_text(line, lineno, fn, allowed, events)
            if opens or closes:
                depth += opens - (closes if not starts_close else 0)
            pending_else = None
        return events

    # -- statement handling ---------------------------------------------------

    def _handle_statement_text(
        self, text: str, lineno: int, fn: CFunc, allowed: Set[Ver], events: List[Event]
    ) -> None:
        if not allowed:
            return
        for stmt in split_top_level(text, ";"):
            stmt = stmt.strip().lstrip("{}").strip()
            if not stmt:
                continue
            self._handle_statement(stmt + " ;", lineno, fn, allowed, events)

    def _handle_statement(self, stmt: str, lineno: int, fn: CFunc, allowed: Set[Ver], events: List[Event]) -> None:
        m = _CHAR_SET_RE.search(stmt)
        if m:
            obj = m.group(1)
            parts = split_top_level(m.group(2))
            if len(parts) >= 2:
                for v in sorted(allowed):
                    for combo in self._combos():
                        env = self._env_for(v, combo)
                        try:
                            grp = c_eval(parts[0], env)
                            no = c_eval(parts[1], env)
                        except Unknown:
                            continue
                        events.append(
                            Event("char", fn.name, lineno, v, no, grp, obj=obj, tag=_tag(combo, parts, self.extra), text=stmt)
                        )
            return
        m = _SPAWN_RE.search(stmt)
        if m:
            prefix, mid, argtxt = m.group(1), m.group(2), m.group(3)
            args_src = split_top_level(argtxt)[1:]  # drop the work pointer
            for v in sorted(allowed):
                for combo in self._combos():
                    env = self._env_for(v, combo)
                    vals: List[int] = []
                    ok = True
                    for a in args_src:
                        try:
                            vals.append(c_eval(a, env))
                        except Unknown:
                            ok = False
                            break
                    events.append(
                        Event(
                            "spawn",
                            fn.name,
                            lineno,
                            v,
                            name=f"{prefix}{mid}_set",
                            args=tuple(vals) if ok else (),
                            text=stmt,
                        )
                    )
            return
        m = _TIMER_RE.match(stmt)
        if m:
            for v in sorted(allowed):
                try:
                    val = c_eval(m.group(2), self._env_for(v, {}))
                except Unknown:
                    continue
                events.append(Event("timer", fn.name, lineno, v, val, name=m.group(1), text=stmt))
        m = _ASSIGN_RE.match(stmt)
        if m and not stmt.lstrip().startswith(("if", "for", "while")):
            obj = m.group(1) or ""
            lhs = m.group(2) if not m.group(3) else f"{m.group(2)}_{m.group(3)}"
            lhs = lhs.replace(".", "_")
            rhs = m.group(4).rstrip(";").strip()
            if lhs == "atck_no" and obj not in ("em", "em_w", "target_w", "tw"):
                for v in sorted(allowed):
                    for combo in self._combos():
                        try:
                            val = c_eval(rhs, self._env_for(v, combo)) & 0x7F
                        except Unknown:
                            continue
                        events.append(Event("atck", fn.name, lineno, v, val, obj=obj or "pl_w", tag=_tag(combo, [rhs], self.extra), text=stmt))
                        self.env[v]["atck_no"] = val
                return
            if lhs in _VER_NAMES:
                return
            if _uses_version(rhs) and not re.search(r"\b(?:pl_char_set|pl_)\w*\(", rhs):
                # something other than animation/hit data differs by button (positions, speeds, ...)
                for v in sorted(allowed):
                    events.append(Event("vercode", fn.name, lineno, v, name=lhs, text=stmt))
            for v in sorted(allowed):
                try:
                    self.env[v][lhs] = c_eval(rhs, self._env_for(v, {}))
                except Unknown:
                    self.env[v].pop(lhs, None)
            return
        if _uses_version(stmt) and not stmt.startswith(("if", "else", "for", "while")):
            for v in sorted(allowed):
                events.append(Event("vercode", fn.name, lineno, v, name="", text=stmt))


def _tag(combo: Dict[str, int], parts: Sequence[str], extra: Dict[str, Sequence[int]]) -> Tuple[Tuple[str, int], ...]:
    used = [(k, combo[k]) for k in combo if any(re.search(rf"\b{re.escape(k)}\b", normalize_expr(p)) for p in parts)]
    return tuple(used)


# ---------------------------------------------------------------------------
# Function reachability
# ---------------------------------------------------------------------------


def referenced_functions(
    fn: CFunc,
    funcs: Dict[str, CFunc],
    tables: Dict[str, CTable],
    bindings: Optional[Dict[str, Sequence[int]]] = None,
) -> List[str]:
    """Same-file functions that `fn` calls or dispatches to, in order of first appearance.

    A jump table indexed by a variable with known candidate values (e.g. `init_jmp[ew->type]`) only yields
    the matching entries; any other index yields the whole table.
    """
    bindings = bindings or {}
    text = fn.text
    found: List[Tuple[int, str]] = []

    local_tables: Dict[str, CTable] = {}
    local_ranges: List[Tuple[int, int]] = []
    for m in _TABLE_HEAD_RE.finditer(text):
        end = _match_brace(text, m.end() - 1)
        local_tables[m.group(1)] = CTable(m.group(1), [[t.strip() for t in text[m.end() : end].split(",") if t.strip()]])
        local_ranges.append((m.start(), end))
    all_tables = dict(tables)
    all_tables.update(local_tables)

    def in_table_def(pos: int) -> bool:
        return any(a <= pos <= b for a, b in local_ranges)

    # table dispatches
    dispatch_spans: List[Tuple[int, int]] = []
    for m in re.finditer(r"\b(\w+)\s*\[\s*([^\]]+?)\s*\]\s*\(", text):
        tname = m.group(1)
        if tname not in all_tables or in_table_def(m.start()):
            continue
        dispatch_spans.append((m.start(), m.end()))
        idx_expr = normalize_expr(m.group(2))
        row_names: List[str] = []
        table = all_tables[tname]
        cand: Optional[Sequence[int]] = None
        if re.fullmatch(r"\w+", idx_expr) and idx_expr in bindings:
            cand = bindings[idx_expr]
        for row in table.rows:
            if cand is None:
                row_names.extend(row)
            else:
                for c in cand:
                    if 0 <= c < len(row):
                        row_names.append(row[c])
        for nm in row_names:
            if nm in funcs and nm != fn.name:
                found.append((m.start(), nm))

    # direct references outside table definitions/dispatches
    for m in re.finditer(r"\b([A-Za-z_]\w*)\b", text):
        nm = m.group(1)
        if nm == fn.name or nm not in funcs or in_table_def(m.start()):
            continue
        if any(a <= m.start() < b for a, b in dispatch_spans):
            continue
        # skip the bare identifier of a table dispatch (`xxx_jmp[...]`)
        found.append((m.start(), nm))

    found.sort(key=lambda p: p[0])
    seen: Set[str] = set()
    ordered: List[str] = []
    for _, nm in found:
        if nm not in seen:
            seen.add(nm)
            ordered.append(nm)
    return ordered
