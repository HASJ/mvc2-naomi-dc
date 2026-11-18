"""Play a Flycast Dojo replay in Flycast Dojo, minimised and without focus, and keep the arcade's RAM each
frame as SCRD archives (OUTDIR/game_N.scrd).

  python -I tools/fcreplay/fcrun.py REPLAY OUTDIR [--fc=DIR] [--dojo6]
--dojo6 counts frames as Dojo 6 did (a frame each render, or each 50 ms without one), for Fightcade's replays.
--v6: DIR's flycast.exe is Dojo 6 itself, which spectates REPLAY (a stream from fcspec.py) from a local server,
as under Fightcade.
DIR holds the Dojo flycast.exe built with core/dojo/scrd_dump.cpp, data/mvsc2.state.net, and ROMs/ with
mvsc2.zip and naomi.zip.
"""
import os, shutil, socket, struct, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FC = os.environ.get("FCR_FC", os.path.join(ROOT, "build", "fcrun"))
MINGW = os.environ.get("FCR_MINGW", "C:/msys64/mingw64/bin")
CONFIG = {
    "dojo:Replay": "yes", "dojo:NetBeacon": "no", "dojo:AutoLoadNetState": "yes", "dojo:ShowReplayInputDisplay": "no",
    "audio:backend": "null", "config:rend.vsync": "no", "config:aica.LimitFPS": "no", "config:Dynarec.Enabled": "yes",
    "config:rend.FixedFrequency": "0", "log:LogToFile": "yes",
}


COMMIT = "4385c204857d88c444894856b6931f2ca71763af"    # Dojo's mvsc2 net state, data/mvsc2.state.net.COMMIT


def stream6(data):
    """A Fightcade stream's messages for Dojo 6: the players' names left out, and the net state's commit set."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from flyreplay import strings
    at, out = 0, []
    while at + 12 <= len(data):
        size, seq, cmd = struct.unpack_from("<III", data, at)
        body = data[at + 12:at + 12 + size]
        at += 12 + size
        if cmd == 5:
            continue
        if cmd == 3:
            (game, _, _, quark, code), o = strings(body, 4, 5)
            names = b"".join(string(x) for x in (game, "p1", "p2", quark, code))
            body = body[:4] + names + body[o:o + 4] + string("") + string(COMMIT)
        out.append(struct.pack("<III", len(body), seq, cmd) + body)
    return out


def string(s):
    b = s.encode() + b"\0"
    return struct.pack("<I", len(b)) + b


def serve(replay):
    """A local spectator server for one connection: it takes the request and sends the stream, a message each
    10 ms after the first 20 (Dojo 6 waits on its buffer again if it all comes before it starts)."""
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)

    def one():
        c, _ = srv.accept()
        c.recv(4096)
        for n, m in enumerate(stream6(open(replay, "rb").read())):
            c.sendall(m)
            if n >= 20:
                time.sleep(0.01)
        try:
            while c.recv(4096):     # Dojo's keepalives, till it quits
                pass
        except OSError:
            pass
        c.close()
        srv.close()
    threading.Thread(target=one, daemon=True).start()
    return srv.getsockname()[1]


def run(replay, out, fc=FC, timeout=3600, dojo6=False, v6=False):
    replay, out = os.path.abspath(replay), os.path.abspath(out)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    cfg = os.path.join(fc, "emu.cfg")
    if not os.path.exists(cfg):
        open(cfg, "w").close()
    args = [os.path.join(fc, "flycast.exe")]
    roms = os.path.join(fc, "ROMs")
    cfgs = dict(CONFIG, **{"dojo:ReplayFilename": replay, "config:Dreamcast.ContentPath": roms})
    if v6:
        del cfgs["dojo:Replay"], cfgs["dojo:ReplayFilename"]
        cfgs.update({"dojo:Enable": "yes", "dojo:Receiving": "yes", "dojo:ActAsServer": "no",
                     "dojo:SpectatorIP": "127.0.0.1", "dojo:SpectatorPort": str(serve(replay))})
    for k, v in cfgs.items():
        args += ["-config", "%s=%s" % (k, v)]
    args.append(os.path.join(roms, "mvsc2.zip"))
    env = dict(os.environ, FC_SCRD=out, PATH=MINGW + ";" + os.environ.get("PATH", ""))
    if dojo6:
        env["FC_DOJO6"] = "1"
    si = subprocess.STARTUPINFO()
    si.dwFlags, si.wShowWindow = subprocess.STARTF_USESHOWWINDOW, 7     # minimised, without taking focus
    with open(os.path.join(out, "run.log"), "w") as log:
        p = subprocess.Popen(args, cwd=fc, env=env, startupinfo=si, stdout=log, stderr=subprocess.STDOUT)
        try:
            rc = p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            p.kill()
            rc = "timeout"
    done = os.path.exists(os.path.join(out, "done"))
    print("fc: exit %s, %s" % (rc, "replay played" if done else "replay not finished"))
    return done


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = dict((a[2:].split("=", 1) + [""])[:2] for a in sys.argv[1:] if a.startswith("--"))
    sys.exit(0 if run(args[0], args[1], opts.get("fc", FC), int(opts.get("timeout", 3600)), "dojo6" in opts,
                    "v6" in opts) else 1)


if __name__ == "__main__":
    main()
