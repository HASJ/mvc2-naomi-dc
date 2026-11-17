"""Backup management (original baseline + timestamped snapshots) and Git/build helpers."""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from .constants import CHAR_BY_ID

_WIN_FLAGS = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


@dataclass
class BackupInfo:
    backup_dir: Path
    kind: str  # "original" | "snapshot"
    timestamp: str
    char_id: Optional[int]
    label: str
    summary: str
    files: List[str]


class BackupManager:
    """Maintains untouched `original/` baselines and timestamped pre-edit snapshots."""

    def __init__(self, repo_root: Path) -> None:
        self.repo_root = repo_root
        self.backup_root = repo_root / "backups" / "move_editor"
        self.original_dir = self.backup_root / "original"
        self.snapshots_dir = self.backup_root / "snapshots"

    def default_char_files(self, char_id: int) -> List[Path]:
        hx = f"{char_id:02x}"
        HX = f"{char_id:02X}"
        candidates = [
            self.repo_root / "ports" / "build-dist" / "disc" / f"PL{HX}_DAT.BIN",
            self.repo_root / "data" / f"char_{hx}.c",
            self.repo_root / "data" / f"frame_box{hx}.c",
            self.repo_root / "data" / f"hit_{hx}.c",
            self.repo_root / "data" / f"hit_dt{hx}.c",
            self.repo_root / "src" / f"s_pl{hx}.c",
            self.repo_root / "src" / "pl_act.c",
            self.repo_root / "src" / "pl_bounce.c",
        ]
        return [p for p in candidates if p.exists()]

    def _copy_with_manifest(
        self,
        dest_dir: Path,
        files: List[Path],
        kind: str,
        char_id: Optional[int],
        label: str,
        summary: str,
        overwrite_existing_files: bool = True,
    ) -> BackupInfo:
        files_root = dest_dir / "files"
        files_root.mkdir(parents=True, exist_ok=True)
        rel_list: List[str] = []
        hashes: Dict[str, str] = {}

        manifest_path = dest_dir / "manifest.json"
        if not overwrite_existing_files and manifest_path.exists():
            try:
                old = json.loads(manifest_path.read_text(encoding="utf-8"))
                hashes = dict(old.get("sha256", {}))
            except Exception:
                pass

        for p in files:
            if not p.exists():
                continue
            rel = p.resolve().relative_to(self.repo_root.resolve()).as_posix()
            dst = files_root / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            if overwrite_existing_files or not dst.exists():
                shutil.copy2(p, dst)
            hashes[rel] = _sha256(dst)
            if rel not in rel_list:
                rel_list.append(rel)

        # Merge previous file list if appending to original baseline
        if not overwrite_existing_files:
            for r in sorted(hashes.keys()):
                if r not in rel_list:
                    rel_list.append(r)

        ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        manifest = {
            "kind": kind,
            "timestamp": ts,
            "char_id": char_id,
            "label": label,
            "summary": summary,
            "files": rel_list,
            "sha256": hashes,
        }
        manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        return BackupInfo(dest_dir, kind, ts, char_id, label, summary, rel_list)

    def ensure_original_backup(self, char_id: int, extra_files: Optional[List[Path]] = None) -> BackupInfo:
        """Preserve the first-seen untouched baseline for char XX (never overwrites existing original files)."""
        char_name, _ = CHAR_BY_ID.get(char_id, (f"pl{char_id:02x}", ""))
        dest = self.original_dir / f"pl{char_id:02x}"
        files = self.default_char_files(char_id)
        if extra_files:
            for ef in extra_files:
                if ef not in files and ef.exists():
                    files.append(ef)
        return self._copy_with_manifest(
            dest_dir=dest,
            files=files,
            kind="original",
            char_id=char_id,
            label=f"Original baseline ({char_name})",
            summary="Untouched baseline saved prior to first Move Editor modification",
            overwrite_existing_files=False,
        )

    def create_snapshot(
        self,
        char_id: Optional[int],
        label: str,
        summary: str,
        files: Optional[List[Path]] = None,
    ) -> BackupInfo:
        """Create a timestamped snapshot backup before saving edits or restoring."""
        if char_id is not None:
            self.ensure_original_backup(char_id, extra_files=files)
        ts_slug = datetime.now().strftime("%Y%m%d_%H%M%S")
        slug_clean = re.sub(r"[^A-Za-z0-9_-]+", "_", label).strip("_")[:40] or "backup"
        prefix = f"pl{char_id:02x}_" if char_id is not None else "global_"
        dest = self.snapshots_dir / f"{ts_slug}_{prefix}{slug_clean}"
        suffix = 1
        while dest.exists():
            dest = self.snapshots_dir / f"{ts_slug}_{prefix}{slug_clean}_{suffix}"
            suffix += 1
        target_files = list(files) if files is not None else (
            self.default_char_files(char_id) if char_id is not None else []
        )
        return self._copy_with_manifest(
            dest_dir=dest,
            files=target_files,
            kind="snapshot",
            char_id=char_id,
            label=label,
            summary=summary,
            overwrite_existing_files=True,
        )

    def list_backups(self, char_id: Optional[int] = None) -> List[BackupInfo]:
        """List available baselines and snapshots, optionally filtered by character ID."""
        items: List[BackupInfo] = []
        search_dirs: List[Path] = []
        if self.original_dir.exists():
            search_dirs.extend(sorted(p for p in self.original_dir.iterdir() if p.is_dir()))
        if self.snapshots_dir.exists():
            search_dirs.extend(sorted((p for p in self.snapshots_dir.iterdir() if p.is_dir()), reverse=True))

        for d in search_dirs:
            mp = d / "manifest.json"
            if not mp.exists():
                continue
            try:
                data = json.loads(mp.read_text(encoding="utf-8"))
            except Exception:
                continue
            cid = data.get("char_id")
            if char_id is not None and cid is not None and cid != char_id:
                continue
            items.append(
                BackupInfo(
                    backup_dir=d,
                    kind=str(data.get("kind", "snapshot")),
                    timestamp=str(data.get("timestamp", "")),
                    char_id=cid,
                    label=str(data.get("label", d.name)),
                    summary=str(data.get("summary", "")),
                    files=list(data.get("files", [])),
                )
            )
        # Put original baseline(s) first, then snapshots newest-first
        originals = [b for b in items if b.kind == "original"]
        snaps = sorted((b for b in items if b.kind != "original"), key=lambda x: x.backup_dir.name, reverse=True)
        return originals + snaps

    def restore_backup(self, info: BackupInfo) -> Tuple[List[str], Optional[BackupInfo]]:
        """Restore files from `info`, first taking a pre-restore safety snapshot."""
        files_root = info.backup_dir / "files"
        if not files_root.exists():
            raise FileNotFoundError(f"Backup files missing: {files_root}")

        existing_targets = [self.repo_root / rel for rel in info.files if (self.repo_root / rel).exists()]
        safety = None
        if existing_targets:
            safety = self.create_snapshot(
                info.char_id,
                label="pre_restore",
                summary=f"Automatic safety backup before restoring {info.backup_dir.name}",
                files=existing_targets,
            )

        restored: List[str] = []
        for rel in info.files:
            src = files_root / rel
            if not src.exists():
                continue
            dst = self.repo_root / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            tmp = dst.with_name(dst.name + ".restore.tmp")
            shutil.copy2(src, tmp)
            tmp.replace(dst)
            restored.append(rel)
        return restored, safety


class GitManager:
    """Hidden-window Git status, diff, logical grouping, and commit runner."""

    def __init__(self, repo_root: Path) -> None:
        self.repo_root = repo_root

    def _run(self, args: List[str], timeout: int = 30) -> subprocess.CompletedProcess:
        return subprocess.run(
            ["git", "-C", str(self.repo_root), *args],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
            creationflags=_WIN_FLAGS,
        )

    def is_repo(self) -> bool:
        try:
            r = self._run(["rev-parse", "--is-inside-work-tree"])
            return r.returncode == 0 and r.stdout.strip() == "true"
        except Exception:
            return False

    def get_status(self) -> List[Tuple[str, str]]:
        """Return list of `(xy_status, repo_relative_path)`."""
        r = self._run(["status", "--porcelain=v1", "-uall"])
        if r.returncode != 0:
            raise RuntimeError(r.stderr.strip() or "git status failed")
        items: List[Tuple[str, str]] = []
        for line in r.stdout.splitlines():
            if len(line) < 4:
                continue
            st = line[:2]
            path = line[3:].strip()
            if path.startswith('"') and path.endswith('"'):
                path = path[1:-1]
            if " -> " in path:
                path = path.split(" -> ", 1)[1]
            items.append((st, path))
        return items

    def get_diff(self, rel_paths: Optional[List[str]] = None, max_chars: int = 40000) -> str:
        args = ["diff"]
        if rel_paths:
            args.append("--")
            args.extend(rel_paths)
        r = self._run(args)
        out = r.stdout or ""
        if not out.strip() and rel_paths:
            # Show summary for untracked or staged files
            r2 = self._run(["diff", "--cached", "--", *rel_paths])
            out = r2.stdout or ""
        if len(out) > max_chars:
            out = out[:max_chars] + "\n... [diff truncated for preview] ...\n"
        return out

    def get_recent_commits(self, limit: int = 8) -> List[str]:
        r = self._run(["log", f"-n{limit}", "--oneline"])
        return r.stdout.splitlines() if r.returncode == 0 else []

    def suggest_commit_groups(
        self,
        status_items: List[Tuple[str, str]],
        active_char_id: Optional[int] = None,
    ) -> List[Tuple[str, str, List[str]]]:
        """Group dirty files by Single-Responsibility commit topic: `(title, suggested_msg, paths)`."""
        groups: List[Tuple[str, str, List[str]]] = []
        paths = [p for _, p in status_items]

        if active_char_id is not None:
            hx = f"{active_char_id:02x}"
            char_name, _ = CHAR_BY_ID.get(active_char_id, (f"pl{hx}", ""))
            char_data_files = [
                p
                for p in paths
                if p.startswith("data/") and (f"_{hx}." in p or p.endswith(f"box{hx}.c") or p.endswith(f"hit_{hx}.c") or p.endswith(f"hit_dt{hx}.c"))
            ]
            if char_data_files:
                groups.append(
                    (
                        f"Character data (pl{hx} {char_name})",
                        f"pl{hx} ({char_name}): update move data and hitboxes",
                        char_data_files,
                    )
                )
            char_src_files = [
                p for p in paths if re.match(rf"^src/(?:s_pl|shl|pls){hx}(?:_|\.)", p)
            ]
            if char_src_files:
                groups.append(
                    (
                        f"Character code (pl{hx} {char_name})",
                        f"pl{hx} ({char_name}): update move code and meter gain",
                        char_src_files,
                    )
                )

        bounce_files = [p for p in paths if p in ("src/pl_bounce.c", "include/pl_bounce.h")]
        if bounce_files:
            groups.append(("Bounce rules (src/pl_bounce.c)", "Combat: update ground/wall bounce rules", bounce_files))

        act_files = [p for p in paths if p == "src/pl_act.c"]
        if act_files:
            groups.append(("Global whiff meter (src/pl_act.c)", "Combat: adjust normal attack whiff meter gain", act_files))

        port_files = [p for p in paths if p.startswith("ports/")]
        if port_files:
            groups.append(("PC port sources", "ports: update disc file checks / port integration", port_files))

        tool_files = [p for p in paths if p.startswith("tools/") or p == ".gitignore"]
        if tool_files:
            groups.append(("Move Editor tool", "tools: add or update move property & hitbox editor", tool_files))

        assigned = {p for _, _, lst in groups for p in lst}
        other = [p for p in paths if p not in assigned]
        if other:
            groups.append(("Other modified files", "Update game data/source files", other))
        return groups

    def commit_files(self, rel_paths: List[str], message: str) -> Tuple[bool, str]:
        if not rel_paths:
            return False, "No files selected for commit."
        msg = message.strip()
        if not msg:
            return False, "Commit message cannot be empty."

        add_res = self._run(["add", "--", *rel_paths])
        if add_res.returncode != 0:
            return False, add_res.stderr.strip() or add_res.stdout.strip()

        cmt_res = self._run(["commit", "--only", "-m", msg, "--", *rel_paths])
        if cmt_res.returncode != 0:
            return False, cmt_res.stderr.strip() or cmt_res.stdout.strip()
        return True, (cmt_res.stdout.strip() or "Committed successfully.")


def rebuild_pc_port(repo_root: Path) -> Tuple[bool, str]:
    """Rebuild mvc2-naomi-dc/ports/build and install to ports/build-dist (hidden window)."""
    build_dir = repo_root / "ports" / "build"
    dist_dir = repo_root / "ports" / "build-dist"
    env = dict(os.environ)
    env["PATH"] = r"C:\msys64\mingw32\bin;" + env.get("PATH", "")

    r1 = subprocess.run(
        ["cmake", "--build", str(build_dir)],
        env=env,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        creationflags=_WIN_FLAGS,
    )
    if r1.returncode != 0:
        return False, "cmake --build failed:\n" + (r1.stdout + "\n" + r1.stderr).strip()

    r2 = subprocess.run(
        ["cmake", "--install", str(build_dir), "--prefix", str(dist_dir)],
        env=env,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        creationflags=_WIN_FLAGS,
    )
    if r2.returncode != 0:
        return False, "cmake --install failed:\n" + (r2.stdout + "\n" + r2.stderr).strip()

    return True, "Rebuilt and installed mvc2.exe and mvc2dc.exe to ports/build-dist/."
