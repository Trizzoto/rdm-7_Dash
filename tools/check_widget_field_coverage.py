#!/usr/bin/env python3
"""
check_widget_field_coverage.py — CI guard for the "codegen-wiped field" bug class.

THE HOLE THIS PLUGS
-------------------
check_widget_codegen.py verifies schema -> codegen-output sync. It is blind to a
field that exists in the firmware C but was NEVER in schema/widgets.schema.json:
there is nothing to be out of sync WITH. So a field hand-added to the WIDGET_DEFS
block in main/web/index.html (codegen OUTPUT) instead of to the schema (the SOURCE
OF TRUTH) survives until the next codegen run, then silently vanishes from the
editor while the firmware keeps working. The feature is alive on-device and
invisible in the editor — undetectable short of a customer asking "where is this
setting?". That has now happened 6 times (grad_stops, warning.flash_mode +
flash_speed, bar.show_bar_label, meter shadow_* x7, line.curvature, rpm_bar
show_rpm_value + rpm_value_font + rpm_value_color).

THE DETECTOR
------------
Each widget's inspector field get/set hooks are keyed by the SCHEMA field name by
construction — that is their whole purpose. So the hook literals are a faithful
list of what the firmware believes the schema exposes:

    strcmp(name, "show_bar_label")   ->  schema field "show_bar_label"

Diffing those literals against the schema's field names for that widget is nearly
noise-free. (Do NOT use a to_json-based heuristic instead: schema name != JSON key
— start_angle_user -> "start_angle", major_tick_step -> "major_tick_every" — so it
over-reports badly.)

KNOWN BLIND SPOT
----------------
This detector only sees fields the on-device inspector edits via a strcmp(name, ...)
hook. A WEB-ONLY field — one with no on-device inspector row, e.g. a gradient
(grad_stops), an image/font uploader, or anim_frames — has no such hook, so if it
is wiped from the schema this guard stays green. grad_stops was in fact one of the
six motivating wipes and would NOT have been caught here. Those field types are
web-editor constructs; guarding them would need a to_json/from_json-key diff, which
is the noisy heuristic rejected above. So: a green result proves every HOOK-BACKED
field is schema-backed, not that every field is. When adding a web-only field type,
add it to the schema first and lean on check_widget_codegen.py (schema<->codegen
sync), not on this check.

Usage:
    python tools/check_widget_field_coverage.py

Exit codes:
    0 = OK (every inspector hook is backed by a schema field, or allowlisted)
    1 = coverage gap (firmware exposes a field the schema does not declare)
    2 = configuration / setup error
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SCHEMA_PATH = REPO_ROOT / "schema" / "widgets.schema.json"
WIDGET_DIR = REPO_ROOT / "main" / "widgets"

# Matches the inspector field hooks, e.g. strcmp(name, "bar_min").
# \s* spans newlines, so hooks wrapped across lines still match.
HOOK_RE = re.compile(r'strcmp\(\s*name\s*,\s*"([^"]+)"\s*\)')


# ---------------------------------------------------------------------------
# ALLOWLIST — inspector hooks that are deliberately NOT schema fields.
#
# Every entry below was hand-verified during the 2026-07-15 audit. They are
# CORRECT AS-IS. Do not "fix" them by adding the field to the schema — they will
# just re-flag on the next audit and you will have added a bogus editor control.
#
# Adding to this list must be a deliberate act: a new entry means "the firmware
# reads this name but the editor must not offer it, and here is why". If you
# cannot write that reason, the field belongs in schema/widgets.schema.json
# instead. Key = widget name (as in the schema); value = {hook: reason}.
# ---------------------------------------------------------------------------
ALLOWLIST: dict[str, dict[str, str]] = {
    # Universal channel binding. The editor's signal picker maps w.signal <->
    # config.signal_name, so it is deliberately not a per-widget schema field.
    # shift_light is the sole exception — it declares signal_name as a real field
    # and is therefore absent from this list.
    "*": {
        "signal_name": "universal channel binding, handled by the editor's signal picker (w.signal <-> config.signal_name)",
    },
    "arc": {
        # Internal/derived representation. The user-facing controls are
        # start_angle_user + sweep_degrees, which ARE schema fields.
        "start_angle": "internal/derived; user-facing controls are start_angle_user + sweep_degrees",
        "end_angle": "internal/derived; user-facing controls are start_angle_user + sweep_degrees",
    },
    "meter": {
        # Same derived-angle rationale as arc. Currently inert (widget_meter.c
        # hooks start_angle_user/sweep_degrees directly) — kept as a guard so the
        # derived pair cannot leak into the editor if meter ever grows them.
        "start_angle": "internal/derived; user-facing controls are start_angle_user + sweep_degrees",
        "end_angle": "internal/derived; user-facing controls are start_angle_user + sweep_degrees",
        # A real codegen wipe, but intentionally left out of the UI: it is a perf
        # toggle, now default-on, with a deferred resize bug. Exposing it invites
        # users to disable an optimisation. Restore to the schema only alongside
        # that resize fix.
        "static_ticks": "perf toggle, default-on, deliberately unexposed (deferred resize bug)",
    },
    "shape_panel": {
        # Deliberately dropped by b43c2d9 ("drop the Trapezoid option"): Trapezoid
        # is gone from the shape_type enum, so this taper code is orphaned. Dead
        # code pending a separate cleanup — NOT a wiped field to restore.
        "taper": "orphaned dead code; Trapezoid removed from shape_type by b43c2d9, cleanup pending",
        "taper_side": "orphaned dead code; Trapezoid removed from shape_type by b43c2d9, cleanup pending",
    },
}


def _allowed(widget: str, hook: str) -> bool:
    return hook in ALLOWLIST.get("*", {}) or hook in ALLOWLIST.get(widget, {})


def main() -> int:
    if not SCHEMA_PATH.is_file():
        print(f"check_widget_field_coverage: schema not found: {SCHEMA_PATH}", file=sys.stderr)
        return 2

    try:
        schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"check_widget_field_coverage: cannot read schema: {exc}", file=sys.stderr)
        return 2

    widgets = schema.get("widgets")
    if not isinstance(widgets, list) or not widgets:
        print("check_widget_field_coverage: schema has no 'widgets' list.", file=sys.stderr)
        return 2

    gaps: list[tuple[str, str]] = []
    checked = 0

    for entry in widgets:
        name = entry.get("name")
        if not name:
            print("check_widget_field_coverage: schema widget entry without a 'name'.", file=sys.stderr)
            return 2

        src_path = WIDGET_DIR / f"widget_{name}.c"
        if not src_path.is_file():
            # Hard error, not a skip: a rename that silently drops a widget from
            # coverage is exactly how this bug class survives.
            print(
                f"check_widget_field_coverage: no source for schema widget '{name}'\n"
                f"  expected: {src_path.relative_to(REPO_ROOT).as_posix()}\n"
                f"  If the file was renamed, update this check so '{name}' stays covered.",
                file=sys.stderr,
            )
            return 2

        src = src_path.read_text(encoding="utf-8", errors="replace")
        hooks = set(HOOK_RE.findall(src))
        schema_fields = {f.get("name") for f in entry.get("fields", [])}

        for hook in sorted(hooks - schema_fields):
            if not _allowed(name, hook):
                gaps.append((name, hook))
        checked += 1

    if gaps:
        print(
            "check_widget_field_coverage: COVERAGE GAP -- "
            f"{len(gaps)} field(s) implemented in firmware but missing from the schema.\n",
            file=sys.stderr,
        )
        for widget, hook in gaps:
            print(f"  {widget}.{hook}", file=sys.stderr)
        print(
            "\nEach field above has an inspector hook in main/widgets/widget_<name>.c but no\n"
            "matching entry in schema/widgets.schema.json -- so it works on-device and is\n"
            "INVISIBLE in the editor. This is the codegen-wiped-field bug class.\n"
            "\n"
            "FIX (in schema/widgets.schema.json -- NOT in the WIDGET_DEFS block of\n"
            "main/web/index.html, which is codegen output and will be overwritten):\n"
            "  1. Add the field to the widget's fields[] in schema/widgets.schema.json.\n"
            "     Read the CURRENT firmware factory default + to_json guards -- do not copy\n"
            "     a default out of the wiping commit's diff; defaults drift after a wipe.\n"
            "  2. python tools/codegen_widget_defs.py main/web/index.html\n"
            "  3. python tools/codegen_widget_inspector.py\n"
            "  4. python tools/check_widget_codegen.py\n"
            "\n"
            "If a field is intentionally not exposed in the editor (dead code, internal or\n"
            "derived value, deliberate omission), add it to ALLOWLIST at the top of\n"
            "tools/check_widget_field_coverage.py with the reason.",
            file=sys.stderr,
        )
        return 1

    print(f"check_widget_field_coverage: OK ({checked} widgets, every inspector hook is schema-backed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
