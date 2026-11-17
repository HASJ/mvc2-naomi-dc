"""CLI entry point for launching the Move Property & Hitbox Editor."""

from __future__ import annotations

import argparse
from pathlib import Path

from .app import run_gui


def main() -> None:
    parser = argparse.ArgumentParser(description="UMvSC2 Move Property & Hitbox Editor")
    parser.add_argument("--repo", type=Path, default=None, help="Path to mvc2-naomi-dc directory")
    parser.add_argument("--char", type=lambda s: int(s, 0), default=0, help="Initial character ID (e.g. 0x00, 0x1e)")
    args = parser.parse_args()
    run_gui(repo_root=args.repo, char_id=args.char)


if __name__ == "__main__":
    main()
