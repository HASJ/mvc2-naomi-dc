"""A contact sheet of BMP shots, as a PNG: python -I sheet.py OUT.png COLS SCALE a.bmp b.bmp ..."""
import struct, sys, zlib


def bmp(path):
    d = open(path, "rb").read()
    off, w, h, bpp = struct.unpack_from("<I", d, 10)[0], *struct.unpack_from("<iiHH", d, 18)[:2], struct.unpack_from("<H", d, 28)[0]
    bpp = struct.unpack_from("<H", d, 28)[0]
    step = bpp // 8
    stride = (w * step + 3) & ~3
    rows = []
    for y in range(abs(h)):
        r = d[off + (abs(h) - 1 - y if h > 0 else y) * stride:][:w * step]
        rows.append(bytes(b for x in range(w) for b in (r[x * step + 2], r[x * step + 1], r[x * step])))
    return w, abs(h), rows


def main():
    out, cols, scale, files = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4:]
    imgs = [bmp(f) for f in files]
    w, h = imgs[0][0] // scale, imgs[0][1] // scale
    nrows = (len(imgs) + cols - 1) // cols
    W, H = w * cols, h * nrows
    canvas = [bytearray(W * 3) for _ in range(H)]
    for i, (iw, ih, rows) in enumerate(imgs):
        cx, cy = (i % cols) * w, (i // cols) * h
        for y in range(h):
            src = rows[y * scale]
            line = canvas[cy + y]
            for x in range(w):
                s = x * scale * 3
                line[(cx + x) * 3:(cx + x) * 3 + 3] = src[s:s + 3]
    raw = b"".join(b"\0" + bytes(r) for r in canvas)
    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    open(out, "wb").write(png)


if __name__ == "__main__":
    main()
