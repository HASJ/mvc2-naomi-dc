"""PLXX_DAT.BIN archive reader, sprite decoder (LZ + Morton 4bpp), and binary patcher."""

from __future__ import annotations

import struct
import zlib
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from .constants import DOT_X_SCALE, DOT_Y_SCALE


def pad32(data: bytes) -> bytes:
    """Pad byte buffer with NULs to a 32-byte boundary, matching tools/chrpack.py."""
    rem = len(data) % 32
    return data if rem == 0 else data + b"\x00" * (32 - rem)


def lz_unpack(data: bytes, ofs: int, out_size: int) -> bytes:
    """Decompress Capcom bytewise LZ stream (matching src/pl_char.c:lz_unpack)."""
    out = bytearray(out_size)
    dst = 0
    pos = ofs
    n_data = len(data)
    flag = 0
    bit = 0
    while dst < out_size and pos < n_data:
        if not bit:
            flag = data[pos]
            pos += 1
            bit = 0x80
        if flag & bit:
            if pos >= n_data:
                break
            c = data[pos]
            pos += 1
            count = (c & 0x0F) + 2
            back = (c >> 4) + 1
            src_idx = dst - back
            for _ in range(count):
                if dst >= out_size:
                    break
                out[dst] = out[src_idx] if 0 <= src_idx < dst else 0
                dst += 1
                src_idx += 1
        else:
            out[dst] = data[pos]
            dst += 1
            pos += 1
        bit >>= 1
    return bytes(out)


def _spread_bits(v: int) -> int:
    i = 0
    k = 0
    while v >> k:
        i |= ((v >> k) & 1) << (2 * k)
        k += 1
    return i


_TWIDDLE_CACHE: Dict[Tuple[int, int], Tuple[List[int], List[int]]] = {}


def _twiddle_axes(cw: int, ch: int) -> Tuple[List[int], List[int]]:
    cached = _TWIDDLE_CACHE.get((cw, ch))
    if cached is not None:
        return cached
    n = min(cw, ch)
    tcol = [((x // n) * n * n if cw > ch else 0) + (_spread_bits(x % n) << 1) for x in range(cw)]
    trow = [(0 if cw > ch else (y // n) * n * n) + _spread_bits(y % n) for y in range(ch)]
    _TWIDDLE_CACHE[(cw, ch)] = (tcol, trow)
    return tcol, trow


def argb4444_to_rgba(word: int) -> Tuple[int, int, int, int]:
    """Convert 16-bit ARGB4444 Dreamcast/NAOMI palette color to (R, G, B, A) 8-bit tuple."""
    a = ((word >> 12) & 0xF) * 17
    r = ((word >> 8) & 0xF) * 17
    g = ((word >> 4) & 0xF) * 17
    b = (word & 0xF) * 17
    return (r, g, b, a)


def rgba_to_png_bytes(width: int, height: int, rgba: bytes) -> bytes:
    """Encode tight RGBA8888 pixel buffer to PNG bytes using stdlib zlib (zero dependencies)."""
    stride = width * 4
    # Prefix each scanline with PNG filter byte 0
    raw = b"".join(b"\x00" + rgba[y * stride : (y + 1) * stride] for y in range(height))
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    idat = zlib.compress(raw, level=1)

    def chunk(tag: bytes, payload: bytes) -> bytes:
        crc = zlib.crc32(tag + payload) & 0xFFFFFFFF
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", crc)

    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")


def scale_rgba_nearest(
    width: int,
    height: int,
    rgba: bytes,
    origin_x: float,
    origin_y: float,
    scale_x: float,
    scale_y: float,
) -> Tuple[int, int, bytes, float, float]:
    """Scale an RGBA image by positive floats using nearest-neighbor sampling."""
    if width <= 0 or height <= 0:
        return width, height, rgba, origin_x, origin_y
    if abs(scale_x - 1.0) < 1e-6 and abs(scale_y - 1.0) < 1e-6:
        return width, height, rgba, origin_x, origin_y
    new_w = max(1, int(round(width * scale_x)))
    new_h = max(1, int(round(height * scale_y)))
    out = bytearray(new_w * new_h * 4)
    row_stride = width * 4
    out_stride = new_w * 4
    src_xs = [min(width - 1, int(nx * width / new_w)) * 4 for nx in range(new_w)]
    for ny in range(new_h):
        sy = min(height - 1, int(ny * height / new_h))
        src_row_base = sy * row_stride
        dst_row_base = ny * out_stride
        for nx, sx4 in enumerate(src_xs):
            s = src_row_base + sx4
            d = dst_row_base + nx * 4
            out[d : d + 4] = rgba[s : s + 4]
    return new_w, new_h, bytes(out), origin_x * scale_x, origin_y * scale_y


class PlDatArchive:
    """Reader and sprite renderer for a character's PLXX_DAT.BIN file."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.raw = path.read_bytes()
        if len(self.raw) < 64:
            raise ValueError(f"{path} is too small ({len(self.raw)} bytes)")
        self.offsets: List[int] = list(struct.unpack_from("<16I", self.raw, 0))
        self.parts: List[bytes] = self._slice_parts()
        self.cell_adrs = self.parts[0]
        self.spr_adrs = self.parts[1]
        self.col_adrs = self.parts[2]
        self._cell_cache: Dict[int, Tuple[int, int, bytes]] = {}
        self._palettes = self._parse_palettes()

    def _slice_parts(self) -> List[bytes]:
        # Compute end offset of each non-zero part using the next positive offset (or EOF).
        parts: List[bytes] = []
        n = len(self.raw)
        for i in range(16):
            start = self.offsets[i]
            if start == 0 or start >= n:
                parts.append(b"")
                continue
            end = n
            for j in range(i + 1, 16):
                if self.offsets[j] > start:
                    end = min(end, self.offsets[j])
            parts.append(self.raw[start:end])
        return parts

    def _parse_palettes(self) -> List[List[Tuple[int, int, int, int]]]:
        lines: List[List[Tuple[int, int, int, int]]] = []
        n_words = len(self.col_adrs) // 2
        words = struct.unpack_from(f"<{n_words}H", self.col_adrs, 0)
        for base in range(0, len(words), 16):
            line: List[Tuple[int, int, int, int]] = []
            for idx in range(16):
                if base + idx < len(words):
                    rgba = argb4444_to_rgba(words[base + idx])
                else:
                    rgba = (0, 0, 0, 0)
                if idx == 0:
                    rgba = (0, 0, 0, 0)
                line.append(rgba)
            lines.append(line)
        return lines

    def cell_count(self) -> int:
        if len(self.cell_adrs) < 4:
            return 0
        first_ofs = struct.unpack_from("<I", self.cell_adrs, 0)[0]
        return first_ofs // 4

    def spr_count(self) -> int:
        if len(self.spr_adrs) < 4:
            return 0
        first_ofs = struct.unpack_from("<I", self.spr_adrs, 0)[0]
        return first_ofs // 4

    def decode_cell(self, cell_no: int) -> Tuple[int, int, bytes]:
        """Return (w_draw, h_draw, palette_indices) for a 4bpp cell (index 0 is transparent)."""
        cached = self._cell_cache.get(cell_no)
        if cached is not None:
            return cached
        max_cells = self.cell_count()
        if cell_no < 0 or cell_no >= max_cells:
            return 0, 0, b""
        co = struct.unpack_from("<I", self.cell_adrs, cell_no * 4)[0]
        if co + 4 > len(self.cell_adrs):
            return 0, 0, b""
        w8, h8, cw8, ch8 = self.cell_adrs[co : co + 4]
        w_draw, h_draw = w8 * 8, h8 * 8
        cw, ch = cw8 * 8, ch8 * 8
        if cw == 0 or ch == 0 or w_draw == 0 or h_draw == 0:
            return 0, 0, b""
        raw = lz_unpack(self.cell_adrs, co + 4, (cw * ch) >> 1)
        tcol, trow = _twiddle_axes(cw, ch)
        out = bytearray(w_draw * h_draw)
        for dy in range(h_draw):
            tex_y = ch - 1 - dy
            if tex_y < 0 or tex_y >= ch:
                continue
            r = trow[tex_y]
            row_ofs = dy * w_draw
            for dx in range(w_draw):
                if dx >= cw:
                    continue
                k = tcol[dx] + r
                out[row_ofs + dx] = (raw[k >> 1] >> ((k & 1) * 4)) & 0x0F
        res = (w_draw, h_draw, bytes(out))
        self._cell_cache[cell_no] = res
        return res

    def _palette_line(self, attr: int, color_idx: int, spr_flag: bool = True) -> List[Tuple[int, int, int, int]]:
        bank = (attr & 0x03FF) >> 4
        if spr_flag and bank < 8:
            idx = (color_idx % 6) * 8 + bank
        elif bank >= 8:
            # Effect palettes start after the 6 character costumes (6 * 8 = 48 lines)
            idx = 48 + (bank - 8)
            if idx >= len(self._palettes):
                idx = bank
        else:
            idx = bank
        if 0 <= idx < len(self._palettes):
            return self._palettes[idx]
        return self._palettes[0] if self._palettes else [(0, 0, 0, 0)] * 16

    def render_sprite(
        self,
        spr_no: int,
        color_idx: int = 0,
        flip_h: bool = False,
        spr_prm: Optional[Tuple[int, int, int, int, int, int, int, int]] = None,
    ) -> Optional[Tuple[int, int, bytes, float, float]]:
        """Render sprite (using spr_no & 0x7fff) to (width, height, rgba_bytes, origin_x, origin_y).

        Coordinates are in native CPS2 dot pixels relative to the character origin (0, 0).
        If flip_h is True, the sprite is mirrored around x=0 (facing Right / P1 view).
        """
        if spr_no < 0:
            return None
        real_no = spr_no & 0x7FFF
        max_spr = self.spr_count()
        if real_no < 0 or real_no >= max_spr:
            return None
        so = struct.unpack_from("<I", self.spr_adrs, real_no * 4)[0]
        if so + 2 > len(self.spr_adrs):
            return None
        cnt = struct.unpack_from("<H", self.spr_adrs, so)[0]
        if cnt == 0 or cnt > 512:
            return None

        ox = 0
        oy = 0
        # Collect placement rectangles in native space (before P1 flip_h / spr_prm)
        placed = []
        min_x = 10**9
        min_y = 10**9
        max_x = -10**9
        max_y = -10**9
        for j in range(cnt):
            rec_ofs = so + 2 + j * 8
            if rec_ofs + 8 > len(self.spr_adrs):
                break
            px, py, attr, cno = struct.unpack_from("<hhHH", self.spr_adrs, rec_ofs)
            ox -= px
            oy -= py
            w_draw, h_draw, idx_bytes = self.decode_cell(cno)
            if w_draw <= 0 or h_draw <= 0:
                continue
            cell_flip_h = bool(attr & 0x8000)
            cell_flip_v = bool(attr & 0x4000)
            pal = self._palette_line(attr, color_idx)
            placed.append((ox, oy, w_draw, h_draw, idx_bytes, cell_flip_h, cell_flip_v, pal))
            min_x = min(min_x, ox)
            min_y = min(min_y, oy)
            max_x = max(max_x, ox + w_draw)
            max_y = max(max_y, oy + h_draw)

        if not placed:
            return None

        width = max_x - min_x
        height = max_y - min_y
        if width <= 0 or height <= 0:
            return None
        buf = bytearray(width * height * 4)

        for x0, y0, cw, ch, idx_bytes, c_fh, c_fv, pal in placed:
            for dy in range(ch):
                sy = (ch - 1 - dy) if c_fv else dy
                src_row = sy * cw
                dst_y = (y0 - min_y) + dy
                dst_row = dst_y * width
                for dx in range(cw):
                    sx = (cw - 1 - dx) if c_fh else dx
                    cidx = idx_bytes[src_row + sx]
                    if cidx == 0:
                        continue
                    r, g, b, a = pal[cidx]
                    if a == 0:
                        continue
                    dst_x = (x0 - min_x) + dx
                    d4 = (dst_row + dst_x) * 4
                    buf[d4] = r
                    buf[d4 + 1] = g
                    buf[d4 + 2] = b
                    buf[d4 + 3] = a

        origin_x = float(-min_x)
        origin_y = float(-min_y)
        out_bytes = bytes(buf)

        # Apply SPR_PRM zoom/flip if present and valid
        if spr_prm is not None:
            zm_x, zm_y, _ang, _ofs_x, _ofs_y, prm_flip = spr_prm[:6]
            if prm_flip & 1:
                flip_h = not flip_h
            sx = (zm_x / 1000.0) if zm_x > 0 else 1.0
            sy = (zm_y / 1000.0) if zm_y > 0 else 1.0
            if sx != 1.0 or sy != 1.0:
                width, height, out_bytes, origin_x, origin_y = scale_rgba_nearest(
                    width, height, out_bytes, origin_x, origin_y, sx, sy
                )

        if flip_h:
            mirrored = bytearray(len(out_bytes))
            row_stride = width * 4
            for y in range(height):
                rb = y * row_stride
                for x in range(width):
                    mx = width - 1 - x
                    s = rb + x * 4
                    d = rb + mx * 4
                    mirrored[d : d + 4] = out_bytes[s : s + 4]
            out_bytes = bytes(mirrored)
            origin_x = float(width) - origin_x

        return width, height, out_bytes, origin_x, origin_y


def patch_pl_dat(
    bin_path: Path,
    part5: bytes,
    part6: bytes,
    part7: bytes,
    part8: bytes,
    out_path: Optional[Path] = None,
) -> int:
    """Replace parts 5..8 in PLXX_DAT.BIN, rebuild header offsets for parts 5..13, and write atomically."""
    raw = bin_path.read_bytes()
    offsets = list(struct.unpack_from("<16I", raw, 0))
    n = len(raw)

    def get_part(idx: int) -> bytes:
        s = offsets[idx]
        if s == 0 or s >= n:
            return b""
        e = n
        for j in range(idx + 1, 16):
            if offsets[j] > s:
                e = min(e, offsets[j])
        return raw[s:e]

    new_parts = [
        get_part(0),
        get_part(1),
        get_part(2),
        get_part(3),
        get_part(4),
        pad32(part5),
        pad32(part6),
        pad32(part7),
        pad32(part8),
        pad32(get_part(9)),
        pad32(get_part(10)),
        pad32(get_part(11)),
        pad32(get_part(12)),
        pad32(get_part(13)),
        get_part(14),
        get_part(15),
    ]

    cursor = 0x40
    out_offsets = [0] * 16
    chunks: List[bytes] = []
    for i in range(16):
        if i == 4 or i >= 14:
            # Parts 4, 14, 15 are NULL (0) unless non-empty
            if len(new_parts[i]) > 0:
                out_offsets[i] = cursor
                p = pad32(new_parts[i])
                chunks.append(p)
                cursor += len(p)
            else:
                out_offsets[i] = 0
        else:
            out_offsets[i] = cursor
            p = pad32(new_parts[i])
            chunks.append(p)
            cursor += len(p)

    header = struct.pack("<16I", *out_offsets)
    result = header + b"".join(chunks)
    target = out_path if out_path is not None else bin_path
    tmp = target.with_suffix(".BIN.tmp")
    tmp.write_bytes(result)
    tmp.replace(target)
    return len(result)
