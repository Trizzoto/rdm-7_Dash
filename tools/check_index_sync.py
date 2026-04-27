#!/usr/bin/env python3
"""
check_index_sync.py — Verify the in-repo copies of index.html stay in sync.

Two copies live in this repo and must match exactly:

  1. main/web/index.html    — canonical, embedded into firmware via
                              EMBED_TXTFILES in main/CMakeLists.txt.
  2. data/web/index.html    — used by tools/mobile-dev-server.js for local
                              browser testing without a device.

The desktop copy at ../rdm7-desktop/src/index.html is a separate Tauri-app
repo with its own delta (USB connect, auto-updater, RDM SDK wrapper) and
is NOT checked here. The check_desktop_sync.py companion (TBD) handles
the structural-but-not-byte-for-byte audit when needed.

Usage:
    python tools/check_index_sync.py            # check, exit 1 on drift
    python tools/check_index_sync.py --fix      # copy main → data

Intended for CI / a pre-commit hook. Exit codes:
    0  files identical
    1  files differ (or one missing)
    2  invocation error (bad args, repo layout unexpected)
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent
CANONICAL = REPO_ROOT / "main" / "web" / "index.html"
MIRROR    = REPO_ROOT / "data" / "web" / "index.html"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--fix", action="store_true",
        help="Copy main/web/index.html → data/web/index.html instead of failing.")
    args = ap.parse_args()

    if not CANONICAL.is_file():
        print(f"error: canonical missing: {CANONICAL}", file=sys.stderr)
        return 2
    if not MIRROR.is_file():
        if args.fix:
            print(f"creating mirror: {MIRROR}")
            MIRROR.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(CANONICAL, MIRROR)
            return 0
        print(f"error: mirror missing: {MIRROR}", file=sys.stderr)
        print("       run with --fix to create it from canonical", file=sys.stderr)
        return 1

    canonical_hash = sha256(CANONICAL)
    mirror_hash = sha256(MIRROR)

    if canonical_hash == mirror_hash:
        size_kb = CANONICAL.stat().st_size / 1024
        print(f"OK: {CANONICAL.relative_to(REPO_ROOT)} == "
              f"{MIRROR.relative_to(REPO_ROOT)} ({size_kb:.1f} KB)")
        return 0

    if args.fix:
        print(f"copying {CANONICAL.relative_to(REPO_ROOT)} → "
              f"{MIRROR.relative_to(REPO_ROOT)}")
        shutil.copy2(CANONICAL, MIRROR)
        return 0

    canonical_kb = CANONICAL.stat().st_size / 1024
    mirror_kb = MIRROR.stat().st_size / 1024
    print(
        f"DRIFT: index.html copies differ\n"
        f"  {CANONICAL.relative_to(REPO_ROOT)}  {canonical_kb:6.1f} KB  "
        f"sha256={canonical_hash[:16]}...\n"
        f"  {MIRROR.relative_to(REPO_ROOT)}  {mirror_kb:6.1f} KB  "
        f"sha256={mirror_hash[:16]}...\n"
        f"  Re-run with --fix to copy main → data, or diff manually if the\n"
        f"  data/ copy was edited deliberately.",
        file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
