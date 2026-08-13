#!/usr/bin/env python3
"""
check_preset_signedness.py — CI / pre-commit safety net.

The same ECU signal can be described in TWO independent C tables:
  1. ECU_PRESETS        (main/layout/ecu_presets.c)      — bulk layout-level apply
  2. preconfig_items    (main/ui/settings/preset_picker_data.c) — wizard + "+ New source"

They have diverged before (MaxxECU temps/ignition lost their signed fix in
the preconfig table — sub-zero temps read ~6500). This script parses both
tables and, for every (make, version) pair, matches rows by
(can_id, bit_start, bit_length) and asserts the WIRE-FORMAT fields agree:
endianness and is_signed. Scale/offset/decimals may legitimately differ
(unit-convention choice: kPa vs psi, lambda vs AFR) and are NOT compared.

Usage:
    python tools/check_preset_signedness.py

Exit codes:
    0 = tables agree
    1 = divergence detected
    2 = parse / setup error
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent

ECU_PRESETS_C = REPO / "main" / "layout" / "ecu_presets.c"
PRESET_PICKER_C = REPO / "main" / "ui" / "settings" / "preset_picker_data.c"

# Same ECU, different display identity in the two tables. Without these the
# exact-key lookup silently skips the pair and CI goes green on a real
# divergence (this is how the GT86 0x0D1 VEHICLE SPEED is_signed mismatch
# hid — the very table class that already shipped the 3.6x speed bug).
# Key: (make, version) in ecu_presets.c -> (make, version) in preset_picker.c
ALIASES = {
    ("Toyota / Subaru", "86 / BRZ"): ("Toyota", "GT86 Gen 1"),
}


def _strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    return src


def parse_ecu_presets(path: Path) -> dict:
    """Return {(make, version): {(can_id, bit_start, bit_len): (endian, signed)}}"""
    src = _strip_comments(path.read_text(encoding="utf-8"))
    presets: dict = {}
    # Each preset block: .make = "X", ... .version = "Y", ... .rows = { ... },
    block_re = re.compile(
        r'\.make\s*=\s*"([^"]+)"\s*,\s*'
        r'\.version\s*=\s*"([^"]+)"\s*,\s*'
        r'\.display\s*=\s*"[^"]*"\s*,\s*'
        r"\.rows\s*=\s*\{(.*?)\n\s*\}\s*,\s*\n\s*\}",
        re.S,
    )
    # Row: [ECU_SIG_X] = { 0x520, 0, 16, 1.0f, 0.0f, false, 1, "rpm", 0 },
    row_re = re.compile(
        r"\[\s*ECU_SIG_\w+\s*\]\s*=\s*\{\s*"
        r"(0[xX][0-9a-fA-F]+|\d+)\s*,\s*"  # can_id
        r"(\d+)\s*,\s*"                      # bit_start
        r"(\d+)\s*,\s*"                      # bit_length
        r"[-\d.]+f?\s*,\s*"                  # scale
        r"[-\d.]+f?\s*,\s*"                  # offset
        r"(true|false)\s*,\s*"               # is_signed
        r"(\d+)"                             # endian
    )
    for m in block_re.finditer(src):
        make, version, rows_src = m.group(1), m.group(2), m.group(3)
        rows = {}
        for r in row_re.finditer(rows_src):
            can_id = int(r.group(1), 0)
            if can_id == 0:
                continue  # SIG_UNSUPPORTED
            key = (can_id, int(r.group(2)), int(r.group(3)))
            rows[key] = (int(r.group(5)), r.group(4) == "true")
        if rows:
            presets[(make, version)] = rows
    return presets


def parse_preconfig(path: Path) -> dict:
    """Return {(make, version): {(can_id, bit_start, bit_len): [(label, endian, signed)]}}"""
    src = _strip_comments(path.read_text(encoding="utf-8"))
    # Row: { "MaxxECU", "1.2", "GEAR", "536", 1, 0, 16, 1, 0, 0, true },
    row_re = re.compile(
        r'\{\s*"([^"]+)"\s*,\s*'   # make
        r'"([^"]+)"\s*,\s*'        # version
        r'"([^"]+)"\s*,\s*'        # label
        r'"([0-9a-fA-Fx]+)"\s*,\s*'  # can_id (hex string)
        r"(\d+)\s*,\s*"            # endianess
        r"(\d+)\s*,\s*"            # bit_start
        r"(\d+)\s*,\s*"            # bit_length
        r"[-\d.]+\s*,\s*"          # scale
        r"[-\d.]+\s*,\s*"          # value_offset
        r"\d+\s*,\s*"              # decimals
        r"(true|false)"            # is_signed
    )
    out: dict = {}
    for r in row_re.finditer(src):
        make, version, label = r.group(1), r.group(2), r.group(3)
        can_id = int(r.group(4), 16)
        key = (can_id, int(r.group(6)), int(r.group(7)))
        out.setdefault((make, version), {}).setdefault(key, []).append(
            (label, int(r.group(5)), r.group(8) == "true")
        )
    return out


def main() -> int:
    for p in (ECU_PRESETS_C, PRESET_PICKER_C):
        if not p.exists():
            print(f"ERROR: {p} not found", file=sys.stderr)
            return 2

    presets = parse_ecu_presets(ECU_PRESETS_C)
    preconfig = parse_preconfig(PRESET_PICKER_C)
    if not presets:
        print("ERROR: parsed zero ECU_PRESETS blocks — regex drift?", file=sys.stderr)
        return 2
    if not preconfig:
        print("ERROR: parsed zero preconfig rows — regex drift?", file=sys.stderr)
        return 2

    failures = []
    compared = 0
    compared_by_mv: dict = {}
    for mv, rows in presets.items():
        cat = preconfig.get(mv)
        if cat is None and mv in ALIASES:
            cat = preconfig.get(ALIASES[mv])
        if cat is None:
            continue  # preset make/version with no preconfig catalog — fine
        for key, (endian, signed) in rows.items():
            for label, c_endian, c_signed in cat.get(key, []):
                compared += 1
                compared_by_mv[mv] = compared_by_mv.get(mv, 0) + 1
                can_id, bit, ln = key
                where = f"{mv[0]} {mv[1]} 0x{can_id:X} b{bit} len{ln} ({label})"
                if c_endian != endian:
                    failures.append(f"{where}: endian {c_endian} (preconfig) != {endian} (preset)")
                if c_signed != signed:
                    failures.append(
                        f"{where}: is_signed {str(c_signed).lower()} (preconfig) != "
                        f"{str(signed).lower()} (preset)"
                    )

    if compared == 0:
        print("ERROR: zero overlapping rows compared — regex drift?", file=sys.stderr)
        return 2

    # A stale alias must fail loudly, not silently stop checking. The global
    # `compared` counter only catches a total regex failure, so assert each
    # aliased pair actually matched rows.
    for fw_mv, pick_mv in ALIASES.items():
        if fw_mv in presets and not compared_by_mv.get(fw_mv):
            print(
                f"ERROR: alias {fw_mv} -> {pick_mv} compared zero rows.\n"
                f"       Either the preconfig key was renamed (update ALIASES)\n"
                f"       or the shared rows no longer overlap by "
                f"(can_id, bit_start, bit_length).",
                file=sys.stderr,
            )
            return 2

    if failures:
        print(f"PRESET SIGNEDNESS DIVERGENCE — {len(failures)} mismatch(es):")
        for f in failures:
            print(f"  {f}")
        print(
            "\nECU_PRESETS (ecu_presets.c) and preconfig_items (preset_picker.c)"
            "\nmust agree on wire format. See the SIGN comments in ecu_presets.c."
        )
        return 1

    print(f"OK — {compared} overlapping rows agree on endian + is_signed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
