"""Generate/sign Ed25519 manifest.json + manifest.sig for dc_game.dll patches (RFC 8032 pure stdlib Python).

Usage:
  sign_patch_manifest.py --keygen OUT_SEED_HEX
  sign_patch_manifest.py PATCH_DIR PATCH_ID [--seed-file PATH | --seed-hex HEX]
"""
import argparse
import hashlib
import json
import secrets
from pathlib import Path

P = (1 << 255) - 19
L = (1 << 252) + 27742317777372353535851937790883648493
D = (-121665 * pow(121666, P - 2, P)) % P
BY = (4 * pow(5, P - 2, P)) % P
BX = 15112221349535400772501151409588531511454012693041857206046113283949847762202
G = (BX, BY)


def _ed_add(pt1, pt2):
    x1, y1 = pt1
    x2, y2 = pt2
    x1x2 = (x1 * x2) % P
    y1y2 = (y1 * y2) % P
    dxxyy = (D * x1x2 * y1y2) % P
    x3 = ((x1 * y2 + x2 * y1) * pow((1 + dxxyy) % P, P - 2, P)) % P
    y3 = ((y1y2 + x1x2) * pow((1 - dxxyy) % P, P - 2, P)) % P
    return (x3, y3)


def _ed_mul(pt, s):
    res = (0, 1)
    cur = pt
    while s > 0:
        if s & 1:
            res = _ed_add(res, cur)
        cur = _ed_add(cur, cur)
        s >>= 1
    return res


def _ed_encode(pt):
    x, y = pt
    bits = bytearray(y.to_bytes(32, "little"))
    if x & 1:
        bits[31] |= 0x80
    return bytes(bits)


def ed25519_pubkey(seed: bytes) -> bytes:
    assert len(seed) == 32
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return _ed_encode(_ed_mul(G, a))


def ed25519_sign(seed: bytes, msg: bytes) -> bytes:
    assert len(seed) == 32
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    prefix = h[32:]
    pk = _ed_encode(_ed_mul(G, a))

    r = int.from_bytes(hashlib.sha512(prefix + msg).digest(), "little") % L
    R = _ed_encode(_ed_mul(G, r))
    k = int.from_bytes(hashlib.sha512(R + pk + msg).digest(), "little") % L
    S = (r + k * a) % L
    return R + S.to_bytes(32, "little")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def sign_patch_dir(patch_dir: Path, patch_id: str, seed: bytes):
    dll_path = patch_dir / "dc_game.dll"
    if not dll_path.is_file():
        raise FileNotFoundError(f"Missing {dll_path}")
    disc_files = []
    disc_dir = patch_dir / "disc"
    if disc_dir.is_dir():
        for p in sorted(disc_dir.rglob("*"), key=lambda x: x.relative_to(disc_dir).as_posix()):
            if p.is_file():
                disc_files.append({
                    "name": p.relative_to(disc_dir).as_posix(),
                    "sha256": sha256_file(p),
                    "size": p.stat().st_size,
                })
    manifest = {
        "patch_id": patch_id,
        "dll_name": "dc_game.dll",
        "dll_sha256": sha256_file(dll_path),
        "dll_size": dll_path.stat().st_size,
        "disc_files": disc_files,
    }
    manifest_bytes = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")
    sig = ed25519_sign(seed, manifest_bytes)
    (patch_dir / "manifest.json").write_bytes(manifest_bytes)
    (patch_dir / "manifest.sig").write_bytes(sig)
    pk = ed25519_pubkey(seed)
    print(f"Signed {patch_id} (pubkey={pk.hex()}, dll_sha256={manifest['dll_sha256']})")


def format_c_pubkey(pk: bytes) -> str:
    rows = []
    for i in range(0, 32, 8):
        rows.append("    " + ", ".join(f"0x{b:02x}" for b in pk[i:i + 8]) + ",")
    return "\n".join(rows)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("patch_dir", nargs="?")
    p.add_argument("patch_id", nargs="?")
    p.add_argument("--keygen", help="Generate a new 32-byte hex seed file and print its C pubkey")
    p.add_argument("--pubkey", action="store_true", help="Print public key from seed without signing")
    p.add_argument("--seed-file", help="Path to 32-byte hex seed file")
    p.add_argument("--seed-hex", help="32-byte hex seed literal")
    args = p.parse_args()

    if args.keygen:
        seed = secrets.token_bytes(32)
        out = Path(args.keygen)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(seed.hex() + "\n", encoding="utf-8", newline="\n")
        pk = ed25519_pubkey(seed)
        print(f"Wrote seed to {out}\nPublic key hex: {pk.hex()}\nC array:\n{format_c_pubkey(pk)}")
        return

    seed_hex = args.seed_hex
    if not seed_hex and args.seed_file:
        seed_hex = Path(args.seed_file).read_text(encoding="utf-8").strip().splitlines()[0].strip()
    if not seed_hex:
        default_seed = Path(__file__).resolve().parent.parent / "ports" / "port" / "tools" / "dev_patch_seed.hex"
        if default_seed.is_file():
            for line in default_seed.read_text(encoding="utf-8").splitlines():
                line = line.strip()
                if line and not line.startswith("#"):
                    seed_hex = line
                    break
    if not seed_hex:
        raise SystemExit("No Ed25519 signing seed specified (--seed-file / --seed-hex)")
    seed = bytes.fromhex(seed_hex)
    if args.pubkey:
        pk = ed25519_pubkey(seed)
        print(f"Public key hex: {pk.hex()}\nC array:\n{format_c_pubkey(pk)}")
        return
    if not args.patch_dir or not args.patch_id:
        raise SystemExit("patch_dir and patch_id are required unless --keygen or --pubkey")
    sign_patch_dir(Path(args.patch_dir), args.patch_id, seed)


if __name__ == "__main__":
    main()
