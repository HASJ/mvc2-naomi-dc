"""Fetch a Fightcade Flycast replay the way Flycast Dojo spectates one: a SPECTATE_REQUEST for the quark,
then the server's messages saved as they come (a stream flyreplay.py reads).

  python -I tools/fcreplay/fcspec.py fcade://stream/flycast/flycast_mvsc2/QUARK.2,7007 OUT [--host=H]
"""
import socket, struct, sys, time

SPECTATE_REQUEST = 4


def string(s):
    b = s.encode() + b"\0"
    return struct.pack("<I", len(b)) + b


def target(url):
    tail = url.rstrip("/").split("/")[-1]
    quark, port = tail.rsplit(",", 1)
    return quark, int(port)


def fetch(url, out, host="ggpo.fightcade.com", game="mvsc2", idle=20):
    quark, port = target(url)
    body = string(quark) + string("") + string(game)
    s = socket.create_connection((host, port), timeout=15)
    s.sendall(struct.pack("<III", len(body), 1, SPECTATE_REQUEST) + body)
    s.settimeout(idle)
    total, t0 = 0, time.time()
    with open(out, "wb") as f:
        while True:
            try:
                b = s.recv(65536)
            except socket.timeout:
                print("fcspec: idle %d s, stopping" % idle)
                break
            except OSError as e:
                print("fcspec: %s" % e)
                break
            if not b:
                break
            f.write(b)
            total += len(b)
    s.close()
    print("fcspec: %d bytes in %.0f s" % (total, time.time() - t0))
    return total


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = dict(a[2:].split("=", 1) for a in sys.argv[1:] if a.startswith("--"))
    n = fetch(args[0], args[1], opts.get("host", "ggpo.fightcade.com"), opts.get("game", "mvsc2"),
              int(opts.get("idle", 20)))
    sys.exit(0 if n else 1)


if __name__ == "__main__":
    main()
