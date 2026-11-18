"""Read the SCRD archives tools/fcreplay's Flycast writes: each frame a 16-byte header (Dojo frame, both
players' kcode, vblank) and the arcade's RAM from 0x0C200000, XORed with the frame before and zero-run coded."""
import re, struct

BASE = 0x0C200000
FULL = re.compile(rb"\xff+")


def apply(data, cur):
    """XOR one coded frame into cur: zero runs leave it as it is."""
    pos, at = 0, 0
    while at < len(data):
        b = data[at]
        if b == 0xFF:
            end = FULL.match(data, at).end()
            pos += 128 * (end - at)
            at = end
        elif b & 0x80:
            pos += (b & 0x7F) + 1
            at += 1
        else:
            n = b + 1
            lit = data[at + 1:at + 1 + n]
            cur[pos:pos + n] = (int.from_bytes(cur[pos:pos + n], "little") ^ int.from_bytes(lit, "little")).to_bytes(
                n, "little")
            pos += n
            at += 1 + n
    return pos


def frames(path):
    """Each frame as (header tuple, RAM); the RAM buffer is reused, so copy what is kept."""
    data = open(path, "rb").read()
    assert data[:4] == b"SCRD"
    n = struct.unpack_from("<H", data, 4)[0]
    table = [struct.unpack_from("<II", data, 6 + 8 * i) for i in range(n)]
    cur = None
    for off, size in table:
        enc = data[off:off + size]
        if cur is None:
            cur = bytearray(16 * 1024 * 1024)
            cur = cur[:apply(enc, cur)]
        else:
            apply(enc, cur)
        yield struct.unpack_from("<IIII", cur, 0), memoryview(cur)[16:]


class Ram:
    def __init__(self, ram):
        self.ram = ram

    def get(self, addr, n):
        return bytes(self.ram[addr - BASE:addr - BASE + n])

    def u(self, addr, size=4, signed=False):
        return int.from_bytes(self.get(addr, size), "little", signed=signed)
