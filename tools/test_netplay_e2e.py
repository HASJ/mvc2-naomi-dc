"""End-to-end verification suite for UMvSC2 Netplay, Go Coordinator/R2 Relay, Replays & Signed Patches."""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

CREATE_NO_WINDOW = 0x08000000
SW_SHOWMINNOACTIVE = 7
STARTF_USESHOWWINDOW = 0x00000001


def free_tcp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


def free_udp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


def http_json(method: str, url: str, token: str | None = None, body: object | None = None) -> tuple[int, dict]:
    raw = None if body is None else json.dumps(body).encode("utf-8")
    req = urllib.request.Request(url, data=raw, method=method)
    req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        data = exc.read().decode("utf-8", errors="replace")
        try:
            return exc.code, json.loads(data)
        except Exception:
            return exc.code, {"raw": data}


def http_bytes(method: str, url: str, body: bytes | None = None, ctype: str = "application/octet-stream") -> tuple[int, bytes]:
    req = urllib.request.Request(url, data=body, method=method)
    if body is not None:
        req.add_header("Content-Type", ctype)
    with urllib.request.urlopen(req, timeout=15) as resp:
        return resp.status, resp.read()


def make_startupinfo():
    si = subprocess.STARTUPINFO()
    si.dwFlags |= STARTF_USESHOWWINDOW
    si.wShowWindow = SW_SHOWMINNOACTIVE
    return si


def spawn_mvc2dc(
    mvc2dc: Path,
    dist_dir: Path,
    cfg_dir: Path,
    args: list[str],
    env_extra: dict[str, str] | None = None,
) -> subprocess.Popen:
    cfg_dir.mkdir(parents=True, exist_ok=True)
    log_path = cfg_dir / "mvc2.log"
    if log_path.exists():
        log_path.unlink()
    env = os.environ.copy()
    env["SDL_WINDOW_ACTIVATE_WHEN_SHOWN"] = "0"
    env["SDL_WINDOW_ACTIVATE_WHEN_RAISED"] = "0"
    if env_extra:
        env.update(env_extra)
    cmd = [str(mvc2dc), "--config", cfg_dir.as_posix(), *args]
    return subprocess.Popen(
        cmd,
        cwd=str(dist_dir),
        env=env,
        startupinfo=make_startupinfo(),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def run_mvc2dc(
    mvc2dc: Path,
    dist_dir: Path,
    cfg_dir: Path,
    args: list[str],
    env_extra: dict[str, str] | None = None,
    timeout: int = 120,
) -> str:
    p = spawn_mvc2dc(mvc2dc, dist_dir, cfg_dir, args, env_extra=env_extra)
    try:
        p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
        raise RuntimeError(f"Timeout running mvc2dc {' '.join(args)}")
    log_file = cfg_dir / "mvc2.log"
    log_text = log_file.read_text(encoding="utf-8", errors="replace") if log_file.exists() else ""
    if p.returncode != 0:
        raise RuntimeError(
            f"mvc2dc exited {p.returncode} args={args}\nlog=\n{log_text}"
        )
    return log_text


def make_synthetic_ko_replay(patch_id: str, p1_id: str, p2_id: str, p1_name: str, p2_name: str) -> bytes:
    # Header layout matching mvc2-naomi-dc/ports/port/net/replay.c (216 bytes)
    hdr = bytearray(216)
    hdr[0:6] = b"MVC2R\x01"
    hdr[6:38] = patch_id.encode("ascii")[:31].ljust(32, b"\x00")
    struct.pack_into("<I", hdr, 38, 0x11223344)
    struct.pack_into("<Q", hdr, 42, 946684800)
    struct.pack_into("<I", hdr, 50, 60)
    hdr[54] = 0  # winner_side = P1 (0)
    hdr[55] = 1  # is_ranked
    hdr[58:74] = p1_id.encode("ascii")[:15].ljust(16, b"\x00")
    hdr[74:90] = p2_id.encode("ascii")[:15].ljust(16, b"\x00")
    hdr[90:122] = p1_name.encode("ascii")[:31].ljust(32, b"\x00")
    hdr[122:154] = p2_name.encode("ascii")[:31].ljust(32, b"\x00")
    struct.pack_into("<HH", hdr, 154, 1625, 1480)
    # Team 1: Sentinel(0x34=52, beta=1), Cable(0x17=23, alpha=0), Strider(0x05=5, gamma=2)
    # Team 2: Magneto(0x2C=44, alpha=0), Storm(0x2A=42, alpha=0), Psylocke(0x08=8, beta=1)
    teams = [(52, 1, 0), (23, 0, 1), (5, 2, 0), (44, 0, 0), (42, 0, 1), (8, 1, 0)]
    for idx, (cid, ast, col) in enumerate(teams):
        struct.pack_into("<BBBB", hdr, 158 + idx * 4, cid, ast, col, 0)
    payload = b"\x3b"  # 60 identical 0-input frames
    csum = struct.pack("<Q", 0xCAFEBABE12345678)
    struct.pack_into("<III", hdr, 198, 60, 1, len(payload))
    return bytes(hdr) + payload + csum


def main() -> int:
    repo = Path(__file__).resolve().parent.parent
    ws_root = repo.parent
    dist_dir = repo / "ports" / "build-dist"
    mvc2dc = dist_dir / "mvc2dc.exe"
    server_exe = ws_root / "netplay-server" / "bin" / "netplay-server.exe"

    if not mvc2dc.exists():
        raise FileNotFoundError(f"Missing {mvc2dc}")
    if not server_exe.exists():
        raise FileNotFoundError(f"Missing {server_exe}")

    with tempfile.TemporaryDirectory(prefix="umvsc2_e2e_") as td:
        tmp = Path(td)
        http_port = free_tcp_port()
        udp_port = free_udp_port()
        p1_udp = free_udp_port()
        p2_udp = free_udp_port()
        base_url = f"http://127.0.0.1:{http_port}"

        srv_env = os.environ.copy()
        srv_env["HTTP_ADDR"] = f"127.0.0.1:{http_port}"
        srv_env["UDP_PORT"] = f"127.0.0.1:{udp_port}"
        srv_env["DATABASE_URL"] = f"sqlite://{(tmp / 'e2e.db').as_posix()}"
        srv_env["BLOB_STORE_DIR"] = (tmp / "blobs").as_posix()
        srv_env["PATCHES_DIR"] = (dist_dir / "patches").as_posix()
        srv_env["RELAY_PUBLIC_HOST"] = "127.0.0.1"
        srv_env["PUBLIC_BASE_URL"] = base_url

        srv_proc = subprocess.Popen(
            [str(server_exe)],
            cwd=str(tmp),
            env=srv_env,
            creationflags=CREATE_NO_WINDOW,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            for _ in range(50):
                try:
                    st, h = http_json("GET", f"{base_url}/healthz")
                    if st == 200 and h.get("ok"):
                        break
                except Exception:
                    pass
                time.sleep(0.1)
            else:
                raise RuntimeError("netplay-server failed health check")

            # 1. Stationary ID registration, mutable display name, Argon2id login
            st, r1 = http_json("POST", f"{base_url}/api/v1/auth/register", body={"display_name": "E2E_Alpha", "pin_inputs": [0, 2, 4, 1, 5, 7]})
            assert st == 201 and r1["player_id"].startswith("P-"), (st, r1)
            st, r2 = http_json("POST", f"{base_url}/api/v1/auth/register", body={"display_name": "E2E_Beta", "pin_inputs": [9, 8, 7, 6, 5, 4]})
            assert st == 201 and r2["player_id"].startswith("P-"), (st, r2)
            p1_id, p1_tok = r1["player_id"], r1["access_token"]
            p2_id, p2_tok = r2["player_id"], r2["access_token"]

            st, upd = http_json("PATCH", f"{base_url}/api/v1/profile", token=p1_tok, body={"display_name": "E2E_AlphaRenamed"})
            assert st == 200 and upd["player_id"] == p1_id and upd["display_name"] == "E2E_AlphaRenamed"

            st, lg = http_json("POST", f"{base_url}/api/v1/auth/login", body={"player_id": p1_id, "pin_inputs": [0, 2, 4, 1, 5, 7]})
            assert st == 200 and lg["display_name"] == "E2E_AlphaRenamed"
            print(f"[E2E 1/5] Auth & Mutable Display Name OK: {p1_id} -> {lg['display_name']}")

            # 2. Private Lobby + /j/<code> OpenGraph + invite_key join + uri_ipc single-instance forwarding
            st, lob = http_json(
                "POST",
                f"{base_url}/api/v1/lobbies",
                token=p1_tok,
                body={"name": "E2E Hidden Ft3", "is_private": True, "pin_inputs": [1, 2, 3, 4, 5, 6], "ft_target": 3},
            )
            assert st == 201, (st, lob)
            code, ikey = lob["join_code"], lob["invite_key"]
            st, page = http_bytes("GET", f"{base_url}/j/{code}?k={ikey}")
            html_txt = page.decode("utf-8")
            assert st == 200 and 'property="og:title"' in html_txt and f"umvsc2://join/{code}?k={ikey}" in html_txt
            st, jres = http_json("POST", f"{base_url}/api/v1/lobbies/{code}/join", token=p2_tok, body={"invite_key": ikey})
            assert st == 200 and jres.get("ok") is True

            cfg_ipc = tmp / "cfg_ipc"
            cfg_ipc.mkdir(parents=True, exist_ok=True)
            (cfg_ipc / "netplay_profile.cfg").write_text(
                f"server_url={base_url}\nplayer_id={p2_id}\ndisplay_name=E2E_Beta\naccess_token={p2_tok}\nrating=1500.0\n",
                encoding="utf-8",
            )
            uri = f"umvsc2://join/{code}?k={ikey}"
            host_proc = spawn_mvc2dc(
                mvc2dc, dist_dir, cfg_ipc, [],
                env_extra={"MVC2_FRAMES": "180", "MVC2_FPS": "60", "MVC2_SERVER_URL": base_url},
            )
            try:
                deadline = time.time() + 15
                while not (cfg_ipc / "mvc2_ipc.port").exists() and time.time() < deadline:
                    time.sleep(0.05)
                assert (cfg_ipc / "mvc2_ipc.port").exists(), "mvc2_ipc.port missing"
                join_rc = subprocess.call(
                    [str(mvc2dc), "--config", cfg_ipc.as_posix(), "--join", uri],
                    cwd=str(dist_dir),
                    creationflags=CREATE_NO_WINDOW,
                )
                assert join_rc == 0
                host_proc.wait(timeout=30)
            finally:
                if host_proc.poll() is None:
                    host_proc.kill()
                    host_proc.wait()
            ipc_log = (cfg_ipc / "mvc2.log").read_text(encoding="utf-8")
            assert f"URI_IPC RECEIVED uri={uri}" in ipc_log, ipc_log
            print(f"[E2E 2/5] Private Lobby /j/{code} + uri_ipc forwarding OK")

            # 3. Two-instance P2P Rollback Match forced through Go R2 UDP Relay + MVC2_NETSIM
            rep1 = tmp / "e2e_p1.mvc2r"
            rep2 = tmp / "e2e_p2.mvc2r"
            cfg_p1 = tmp / "cfg_p1"
            cfg_p2 = tmp / "cfg_p2"
            netsim_env = {"MVC2_FRAMES": "1200", "MVC2_NETSIM": "delay_ms=70,jitter_ms=20,loss_pct=10"}
            relay_spec = f"127.0.0.1:{udp_port}"

            proc1 = spawn_mvc2dc(
                mvc2dc, dist_dir, cfg_p1,
                ["--p2p", "1", str(p1_udp), f"127.0.0.1:{p2_udp}", "--relay", relay_spec,
                 "--relay-token", "0xCAFE1234", "--force-relay", "--delay", "1",
                 "--match-seed", "0x13579BDF", "--scripted-inputs", "--record", rep1.as_posix()],
                env_extra=netsim_env,
            )
            proc2 = spawn_mvc2dc(
                mvc2dc, dist_dir, cfg_p2,
                ["--p2p", "2", str(p2_udp), f"127.0.0.1:{p1_udp}", "--relay", relay_spec,
                 "--relay-token", "0xCAFE1234", "--force-relay", "--delay", "1",
                 "--match-seed", "0x13579BDF", "--scripted-inputs", "--record", rep2.as_posix()],
                env_extra=netsim_env,
            )
            try:
                proc1.wait(timeout=120)
                proc2.wait(timeout=120)
            finally:
                for pr in (proc1, proc2):
                    if pr.poll() is None:
                        pr.kill()
                        pr.wait()
            assert proc1.returncode == 0 and proc2.returncode == 0, (proc1.returncode, proc2.returncode)
            log1 = (cfg_p1 / "mvc2.log").read_text(encoding="utf-8")
            log2 = (cfg_p2 / "mvc2.log").read_text(encoding="utf-8")
            m1 = re.search(r"NETPLAY_P2P PASS .*relayed=(\d+).*desyncs=0", log1)
            m2 = re.search(r"NETPLAY_P2P PASS .*relayed=(\d+).*desyncs=0", log2)
            assert m1 and m2 and int(m1.group(1)) > 0 and int(m2.group(1)) > 0, (log1, log2)
            sha1 = hashlib.sha256(rep1.read_bytes()).hexdigest()
            sha2 = hashlib.sha256(rep2.read_bytes()).hexdigest()
            assert sha1 == sha2, f"Replay mismatch: {sha1} != {sha2}"
            print(f"[E2E 3/5] 2-Instance Forced Go R2 Relay + MVC2_NETSIM PASS: relayed={m1.group(1)}/{m2.group(1)} sha256={sha1[:16]}...")

            # 4. Glicko-2 Ladder Report, Replay DB Search, Patch Fetch, Replay Verify/Takeover & UI Smoke
            for side, tok, opp in ((1, p1_tok, p2_id), (2, p2_tok, p1_id)):
                st, rep_res = http_json(
                    "POST", f"{base_url}/api/v1/ladder/report", token=tok,
                    body={
                        "match_id": "M-E2E-1", "side": side, "opponent_id": opp,
                        "patch_id": "umvsc2-20000529-e7608487", "match_seed": 0x13579BDF,
                        "winner_side": 0, "final_checksum": 987654321,
                        "p1_chars": [52, 23, 5], "p2_chars": [44, 42, 8],
                    },
                )
                assert st == 200, (st, rep_res)
            assert rep_res["finalized"] is True

            st, lb = http_json("GET", f"{base_url}/api/v1/leaderboard?limit=10")
            assert st == 200 and len(lb["entries"]) >= 2
            by_id = {e["player_id"]: e for e in lb["entries"]}
            assert by_id[p1_id]["rating"] > 1500.0 and by_id[p2_id]["rating"] < 1500.0
            assert by_id[p1_id]["display_name"] == "E2E_AlphaRenamed"

            # Upload real 1200f P2P .mvc2r + synthetic KO replay to verify character+winner search & download roundtrip
            st, up_real = http_bytes("POST", f"{base_url}/api/v1/replays", body=rep1.read_bytes())
            assert st == 201
            real_info = json.loads(up_real.decode("utf-8"))
            patch_id = real_info["patch_id"]
            ko_bytes = make_synthetic_ko_replay(patch_id, p1_id, p2_id, "E2E_AlphaRenamed", "E2E_Beta")
            st, _ = http_bytes("POST", f"{base_url}/api/v1/replays", body=ko_bytes)
            assert st == 201

            st, found_win = http_json("GET", f"{base_url}/api/v1/replays?team_a_chars=52,23&winner_only=1")
            assert st == 200 and len(found_win["replays"]) == 1
            st, found_lose = http_json("GET", f"{base_url}/api/v1/replays?any_chars=44&winner_only=1")
            assert st == 200 and len(found_lose["replays"]) == 0

            # Download real replay + signed patch artifacts into isolated config dir, verify replay + takeover
            dl_rep = tmp / "downloaded_real.mvc2r"
            st, dl_bytes = http_bytes("GET", f"{base_url}/api/v1/replays/{real_info['replay_id']}/download")
            assert st == 200 and hashlib.sha256(dl_bytes).hexdigest() == sha1
            dl_rep.write_bytes(dl_bytes)

            dl_cfg = tmp / "cfg_dl"
            pdir = dl_cfg / "patches" / patch_id
            pdir.mkdir(parents=True, exist_ok=True)
            for fname in ("manifest.json", "manifest.sig", "dc_game.dll"):
                st, fbytes = http_bytes("GET", f"{base_url}/api/v1/patches/{patch_id}/{fname}")
                assert st == 200, (fname, st)
                (pdir / fname).write_bytes(fbytes)

            v_log = run_mvc2dc(mvc2dc, dist_dir, dl_cfg, ["--patch", patch_id, "--replay-verify", dl_rep.as_posix()])
            assert "REPLAY_VERIFY PASS" in v_log and "mismatches=0" in v_log, v_log

            t_log = run_mvc2dc(mvc2dc, dist_dir, dl_cfg, ["--takeover-test", dl_rep.as_posix()])
            tm = re.search(r"TAKEOVER_TEST PASS .*seek_sub_ms=([0-9.]+).*diverged=1 reset_exact=1", t_log)
            assert tm and float(tm.group(1)) < 12.0, t_log

            u_log = run_mvc2dc(mvc2dc, dist_dir, dl_cfg, ["--ui-smoke-test"], env_extra={"MVC2_SERVER_URL": base_url})
            assert "UI_SMOKE_TEST PASS" in u_log, u_log

            # Rate-limit check last so valid logins earlier weren't blocked by loopback IP limiter
            for _ in range(5):
                st, _ = http_json("POST", f"{base_url}/api/v1/auth/login", body={"player_id": p2_id, "pin_inputs": [0, 0, 0, 0, 0, 0]})
                assert st == 401
            st, _ = http_json("POST", f"{base_url}/api/v1/auth/login", body={"player_id": p2_id, "pin_inputs": [9, 8, 7, 6, 5, 4]})
            assert st == 429

            print(f"[E2E 4/5] Glicko-2 Leaderboard, Replay DB Search, Patch Fetch & Takeover (seek={tm.group(1)}ms) OK")
            print("[E2E 5/5] ALL E2E NETPLAY CHECKS PASSED!")
            return 0
        finally:
            srv_proc.terminate()
            try:
                srv_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                srv_proc.kill()


if __name__ == "__main__":
    sys.exit(main())
