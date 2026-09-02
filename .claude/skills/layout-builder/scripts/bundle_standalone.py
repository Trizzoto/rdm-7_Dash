#!/usr/bin/env python3
"""Snapshot everything the skill's scripts need from the RDM-7_Dash repo into
`_bundled/`, so the skill still works when it is copied somewhere without a
firmware checkout (handing it to someone else, dropping it in another project).

    python .claude/skills/layout-builder/scripts/bundle_standalone.py

Writes:
    _bundled/schema/widgets.schema.json   the widget schema (verbatim copy)
    _bundled/device_fields.json           per-widget: the config keys the DEVICE's
                                          from_json actually reads, extracted from
                                          main/widgets/*.c
    _bundled/layout_manager.h             a stub carrying LAYOUT_SCHEMA_VERSION
    _bundled/README.md                    what this is and when it went stale

`validate_layout.py` and `gen_catalog.py` prefer the real repo and fall back to
these. **Re-run this whenever the schema, LAYOUT_SCHEMA_VERSION, or a widget's
from_json changes**, and commit the result — a stale bundle validates against
last month's firmware without saying so.
"""
import json, io, os, re, shutil, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
OUT = os.path.abspath(os.path.join(HERE, "..", "_bundled"))
SCHEMA = os.path.join(ROOT, "schema", "widgets.schema.json")
WIDGETS_DIR = os.path.join(ROOT, "main", "widgets")
LM_H = os.path.join(ROOT, "main", "layout", "layout_manager.h")

_FIELD_RE = re.compile(r'GetObjectItem(?:CaseSensitive)?\s*\(\s*[A-Za-z_]\w*\s*,\s*"([a-z_]\w*)"')


def git_describe():
    for args in (["git", "describe", "--tags", "--always", "--dirty"],
                 ["git", "rev-parse", "--short", "HEAD"]):
        try:
            return subprocess.check_output(args, cwd=ROOT,
                                           stderr=subprocess.DEVNULL).decode().strip()
        except Exception:
            continue
    return "unknown"


def main():
    if not os.path.exists(SCHEMA):
        raise SystemExit("no schema at %s — run this from inside the RDM-7_Dash "
                         "repo, it is what builds the snapshot" % SCHEMA)

    os.makedirs(os.path.join(OUT, "schema"), exist_ok=True)
    shutil.copyfile(SCHEMA, os.path.join(OUT, "schema", "widgets.schema.json"))

    S = json.load(io.open(SCHEMA, encoding="utf-8"))
    widgets = S.get("widgets") or S.get("widget_types")

    dev = {}
    missing = []
    for w in widgets:
        p = os.path.join(WIDGETS_DIR, "widget_%s.c" % w["name"])
        if not os.path.exists(p):
            missing.append(w["name"])
            continue
        names = set(_FIELD_RE.findall(io.open(p, encoding="utf-8", errors="ignore").read()))
        names.discard("config")
        dev[w["name"]] = sorted(names)
    with io.open(os.path.join(OUT, "device_fields.json"), "w",
                 encoding="utf-8", newline="\n") as fp:
        fp.write(json.dumps(dev, indent=1, sort_keys=True) + "\n")

    ver = "?"
    try:
        m = re.search(r"#define\s+LAYOUT_SCHEMA_VERSION\s+(\d+)",
                      io.open(LM_H, encoding="utf-8").read())
        if m:
            ver = m.group(1)
    except Exception:
        pass
    with io.open(os.path.join(OUT, "layout_manager.h"), "w",
                 encoding="utf-8", newline="\n") as fp:
        fp.write("/* Snapshot stub — NOT the firmware header. Carries only the one\n"
                 " * value the skill's scripts read from it. Regenerate with\n"
                 " * scripts/bundle_standalone.py. */\n"
                 "#define LAYOUT_SCHEMA_VERSION %s\n" % ver)

    nfields = sum(len(v) for v in dev.values())
    with io.open(os.path.join(OUT, "README.md"), "w",
                 encoding="utf-8", newline="\n") as fp:
        fp.write(
            "# _bundled — offline snapshot\n\n"
            "`validate_layout.py` and `gen_catalog.py` read the RDM-7_Dash repo when\n"
            "they can. When the skill is used **outside** a firmware checkout they\n"
            "fall back to this directory, so validation still works on a handed-over\n"
            "copy.\n\n"
            "| file | what |\n|---|---|\n"
            "| `schema/widgets.schema.json` | verbatim copy of the widget schema |\n"
            "| `device_fields.json` | per widget, the config keys the device's `from_json` reads (extracted from `main/widgets/*.c`) |\n"
            "| `layout_manager.h` | stub carrying `LAYOUT_SCHEMA_VERSION` only |\n\n"
            "**Do not hand-edit any of it.** Regenerate:\n\n"
            "```\npython scripts/bundle_standalone.py\n```\n\n"
            "## Provenance\n\n"
            "| | |\n|---|---|\n"
            "| firmware commit | `%s` |\n"
            "| `LAYOUT_SCHEMA_VERSION` | %s |\n"
            "| widgets | %d |\n"
            "| device fields captured | %d |\n\n"
            "A snapshot older than the firmware it is checked against will validate\n"
            "a layout as fine when it is not. If the numbers above disagree with the\n"
            "header of `reference/02-widget-catalog.md`, re-run the bundler.\n"
            % (git_describe(), ver, len(widgets), nfields))

    print("bundled -> %s" % os.path.relpath(OUT, ROOT))
    print("  schema: %d widgets" % len(widgets))
    print("  device_fields.json: %d widgets, %d fields" % (len(dev), nfields))
    print("  LAYOUT_SCHEMA_VERSION %s @ %s" % (ver, git_describe()))
    if missing:
        print("  WARNING: no .c found for: %s" % ", ".join(missing))


if __name__ == "__main__":
    main()
