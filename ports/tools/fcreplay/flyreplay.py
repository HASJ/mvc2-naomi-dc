"""Read a Flycast Dojo replay (a .flyreplay file or a captured spectator stream): its header and each
frame's inputs for both players.

  python -I tools/fcreplay/flyreplay.py REPLAY [OUT.flyreplay]
With OUT, it writes the inputs again as a Dojo 7.9 replay (a Fightcade stream is Dojo 6's).
"""
import struct, sys

SPECTATE_START, PLAYER_INFO, MAPLE_BUFFER = 3, 5, 6
DC_BITS = {"a1": 0x0001, "hp": 0x0002, "lp": 0x0004, "start": 0x0008, "up": 0x0010, "down": 0x0020,
           "left": 0x0040, "right": 0x0080, "a2": 0x0100, "hk": 0x0200, "lk": 0x0400, "coin": 0x0800}


def strings(body, at, n):
    out = []
    for _ in range(n):
        size = struct.unpack_from("<I", body, at)[0]
        out.append(body[at + 4:at + 4 + size].rstrip(b"\0").decode("utf-8", "replace"))
        at += 4 + size
    return out, at


def read(path):
    """(header, {frame: (p1 kcode, p2 kcode)})"""
    data = open(path, "rb").read()
    head, frames, at = {}, {}, 0
    while at + 12 <= len(data):
        size, seq, cmd = struct.unpack_from("<III", data, at)
        body = data[at + 12:at + 12 + size]
        at += 12 + size
        if cmd == SPECTATE_START and not head:
            head["version"] = struct.unpack_from("<I", body, 0)[0]
            (head["game"], head["p1"], head["p2"], head["quark"], head["relay_key"]), o = strings(body, 4, 5)
            if head["version"] == 3:        # Dojo 6, as Fightcade's server sends it
                head["analog"] = struct.unpack_from("<I", body, o)[0]
                (head["state_md5"], head["state_commit"]), o = strings(body, o + 4, 2)
            else:
                head["analog"], head["precise_triggers"], head["ggpo"] = struct.unpack_from("<III", body, o)
                (head["p1_country"], head["p2_country"]), o = strings(body, o + 12, 2)
        elif cmd == MAPLE_BUFFER:
            fsize = struct.unpack_from("<I", body, 0)[0]
            for o in range(4, len(body) - fsize + 1, fsize):
                fr = body[o:o + fsize]
                if fr[:20] == b"0" * 20:
                    continue
                n = struct.unpack_from("<I", fr, 0)[0]
                step = 4 + head.get("analog", 0) if head.get("version") == 3 else (fsize - 4) // 2
                k1, k2 = struct.unpack_from("<I", fr, 4)[0], struct.unpack_from("<I", fr, 4 + step)[0]
                frames[n] = (k1 & 0xFFFFF, k2 & 0xFFFFF)
    return head, frames


def string(s):
    b = s.encode() + b"\0"
    return struct.pack("<I", len(b)) + b


def write(path, head, frames):
    """A Dojo 7.9 replay (version 2, 28-byte frames) of these inputs."""
    start = struct.pack("<I", 2) + b"".join(string(head.get(k, "")) for k in ("game", "p1", "p2", "quark", "relay_key"))
    start += struct.pack("<III", 0, 0, 1) + string("") + string("")
    data = [struct.pack("<III", len(start), 0, SPECTATE_START) + start]
    last = max(frames)
    for at in range(0, last + 1, 120):
        body = struct.pack("<I", 28) + b"".join(
            struct.pack("<II8xI8x", n, *frames.get(n, (0, 0))) for n in range(at, min(at + 120, last + 1)))
        data.append(struct.pack("<III", len(body), at + 1, MAPLE_BUFFER) + body)
    open(path, "wb").write(b"".join(data))


def main():
    head, frames = read(sys.argv[1])
    if len(sys.argv) > 2:
        write(sys.argv[2], head, frames)
    print(head)
    if frames:
        print("frames %d..%d (%d)" % (min(frames), max(frames), len(frames)))


if __name__ == "__main__":
    main()
