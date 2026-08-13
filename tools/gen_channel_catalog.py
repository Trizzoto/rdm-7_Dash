#!/usr/bin/env python3
"""Regenerate (or verify) the baked channel catalogue in main/web/index.html.

The catalogue is the firmware's own canonical-channel and ECU-preconfig
tables, emitted as JSON by a host-compiled C tool that LINKS the real
firmware arrays (tools/native/gen_channel_catalog.c). Baking it into the
page is what makes channel setup work with no dash on the network — from
zero, not just from a mirror (ADR-0033).

    python tools/gen_channel_catalog.py            # regenerate in place
    python tools/gen_channel_catalog.py --check    # exit 1 on drift (CI)

Drift is impossible to introduce silently: the C tool compiles the same
translation units the firmware ships, and CI recompiles + byte-compares.
Never hand-edit the block between the markers.
"""
import subprocess
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HTML = REPO / "main" / "web" / "index.html"
TOOL_SRC = REPO / "tools" / "native" / "gen_channel_catalog.c"
SOURCES = [
    TOOL_SRC,
    REPO / "main" / "data" / "canonical_channels.c",
    REPO / "main" / "ui" / "settings" / "preset_picker_data.c",
]
INCLUDES = ["main", "main/data", "main/ui/settings", "tests/native/mocks"]

BEGIN = "// AUTO-GENERATED BEGIN: CHANNEL_CATALOG (tools/gen_channel_catalog.py — do not hand-edit)"
END = "// AUTO-GENERATED END: CHANNEL_CATALOG"


def build_and_run() -> str:
    cc = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")
    if not cc:
        sys.exit("gen_channel_catalog: no C compiler on PATH (need gcc/cc/clang)")
    out_dir = REPO / "build" / "hosttools"
    out_dir.mkdir(parents=True, exist_ok=True)
    exe = out_dir / ("gen_channel_catalog.exe" if sys.platform == "win32" else "gen_channel_catalog")
    cmd = [cc, *map(str, SOURCES), *[f"-I{REPO / i}" for i in INCLUDES], "-o", str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"gen_channel_catalog: compile failed:\n{r.stderr}")
    run = subprocess.run([str(exe)], capture_output=True)
    if run.returncode != 0:
        sys.exit(f"gen_channel_catalog: emitter exited {run.returncode}")
    return run.stdout.decode("utf-8").strip()


def main() -> None:
    check = "--check" in sys.argv
    json_line = build_and_run()
    html = HTML.read_text(encoding="utf-8")
    try:
        b = html.index(BEGIN)
        e = html.index(END)
    except ValueError:
        sys.exit(f"gen_channel_catalog: markers not found in {HTML}")
    indent = html[html.rfind("\n", 0, b) + 1 : b]
    block = (
        f"{BEGIN}\n"
        f"{indent}window.RDM_BAKED_CATALOG = {json_line};\n"
        f"{indent}"
    )
    new_html = html[:b] + block + html[e:]
    if check:
        if new_html != html:
            sys.exit(
                "channel catalogue drift: the baked block in main/web/index.html\n"
                "does not match the firmware tables. Regenerate and commit:\n"
                "  python tools/gen_channel_catalog.py"
            )
        print("OK — baked channel catalogue matches the firmware tables")
        return
    if new_html != html:
        HTML.write_text(new_html, encoding="utf-8")
        print(f"regenerated baked catalogue ({len(json_line)} bytes of JSON)")
    else:
        print("baked catalogue already current")


if __name__ == "__main__":
    main()
