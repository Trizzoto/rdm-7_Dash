# Tests

Two test suites live under this directory:

| Suite | What it covers | Runtime | Trigger |
|---|---|---|---|
| **`tests/native/`** — host-side Unity | Pure-logic firmware code (CAN math, JSON round-trips, schema migration, widget precedence ladders, OBD2 frame parsing, OTA version comparator, gear math) | Sub-second; stock GCC, no IDF | Every change to the mirrored code |
| **`tests/api/`** — Python integration | Live HTTP contract against a running device (layout/signal/system/touch endpoints + the `413 layout_too_large` shape) | Seconds; requires a flashed board on the LAN | Before pushing changes that touch `main/net/web_server*.c` |

This file documents the native suite. For API tests see [`tests/api/README.md`](api/README.md) (if present) or the docstrings at the top of each `test_*.py`.

## Why native-host

ESP32 firmware is hard to test on the device — there's no shell, no test runner, no easy way to capture failures. Anything that *can* be tested as pure logic on the host *should* be, because:

- The build/run cycle is sub-second instead of ~2 minutes.
- Failures are debuggable with a normal debugger.
- CI can run the tests without a board.
- Refactors land with confidence.

## Current coverage

| File | Tests | Source-of-truth |
|---|---|---|
| `test_can_decode.c` | 20 | `main/can/can_decode.c` (real source linked) |
| `test_widget_rules.c` | 22 | `main/widgets/widget_rules.c` (real source + LVGL/heap-caps mocks) |
| `test_layout_migration.c` | 15 | `main/layout/layout_manager.c` schema-version gate + `_migrate_layout_root` (mirrored) |
| `test_widget_arc_precedence.c` | 19 | `main/widgets/widget_arc.c` `_arc_apply_fill_color` ladder (mirrored) |
| `test_obd2_freeze_frame.c` | 21 | `main/can/obd2.c` Mode 02 parse + `main/ui/menu/dtc_reader.c` `_ff_decode` (mirrored) |
| `test_ota_skip_version.c` | 12 | `main/net/ota_handler.c` skip-version comparator (mirrored) |
| `test_calculated_gear.c` | 13 | `main/widgets/signal_internal.c` CALCULATED_GEAR classifier (mirrored) |
| **Total** | **122** | |

100% pass on `.\run.ps1` (Windows / MinGW) and `make` (Linux / WSL / macOS).

## Two test patterns: link-real vs. mirror

The suite uses two strategies depending on how entangled the firmware source is with LVGL / FreeRTOS / ESP-IDF.

### Link-real (`test_can_decode.c`, `test_widget_rules.c`)

The firmware `.c` is `#include`d directly into the test TU. Any LVGL / esp_log / heap-caps calls inside are satisfied by header-only stubs in `tests/native/mocks/`. This is the strongest form of coverage — when firmware drifts, the test recompiles against the new source and fails if behaviour changed.

Use this when:
- The dependency surface is small enough that a few hundred lines of stubs cover it.
- The function under test is reachable from a `static` entry point you can call from the test.

### Mirror (`test_layout_migration.c`, `test_widget_arc_precedence.c`, `test_obd2_freeze_frame.c`, `test_ota_skip_version.c`, `test_calculated_gear.c`)

The firmware logic is re-implemented verbatim as a static helper inside the test file. The header comment of the test names the **source-of-truth reference** (file + function in `main/`) so future drift is obvious — if the firmware logic changes, the mirror won't, the test stays green, and the divergence is visible at code review.

Use this when:
- The function calls deep into LVGL / FreeRTOS / NVS and the dependency closure is too big to stub.
- The logic is a self-contained pure-data slice that's easy to copy faithfully.

Trade-off acknowledged: a mirror can rot silently. The mitigation is the explicit source-of-truth reference in the test header + a paired commit message describing what was mirrored, so reviewers know to update the mirror when they change firmware.

## Layout

```
tests/
├── README.md          (you are here)
├── api/               (Python HTTP contract tests against a live device)
│   └── test_*.py
└── native/
    ├── Makefile       (build rule per test, plain GCC, no CMake)
    ├── run.ps1        (Windows alternative to make)
    ├── unity.h        (vendored mini Unity — single header)
    ├── unity.c        (single source)
    ├── test_*.c       (7 files, see table above)
    ├── mocks/         (per-test stubs of LVGL / ESP-IDF when needed)
    │   ├── lvgl.h
    │   ├── esp_log.h
    │   └── esp_heap_caps.h
    └── cjson/         (vendored cJSON v1.7.18 — only when a test needs it)
        ├── cJSON.h
        └── cJSON.c
```

## Build & run

From the repo root:

**Linux / macOS / WSL** (with `make`):
```bash
cd tests/native
make                              # builds and runs every test
make test_widget_arc_precedence   # builds and runs one test
make clean
```

**Windows** (PowerShell, `gcc` on PATH — MinGW or MSYS2):
```powershell
cd tests\native
.\run.ps1                              # builds and runs every test
.\run.ps1 test_widget_arc_precedence   # one test
.\run.ps1 -Clean
```

Tests print one line per assertion (`[PASS] / [FAIL]`) and a final `=== N tests, N passed, 0 failed ===` summary. Exit code is 0 on success, non-zero on any failure — wire it into CI.

## Adding a new test

1. Create `tests/native/test_<name>.c`. Header-comment block names the source-of-truth in `main/`.
2. `#include "unity.h"` and either:
   - Link the real source directly (`#include "../../main/path/to/file.c"`) plus any needed mocks, or
   - Mirror the function under test as a `static` helper in the file.
3. Each test function: `void test_my_thing(void) { TEST_ASSERT_*(...); }`.
4. Wire it up:
   ```c
   int main(void) {
       UNITY_BEGIN();
       RUN_TEST(test_my_thing);
       return UNITY_END();
   }
   ```
5. Add to the `TESTS` list in `Makefile`. `run.ps1` auto-discovers `test_*.c`, no change needed there unless you need extra include paths or sources (then add a case to `Get-ExtraIncludes` / `Get-ExtraSources`).
6. Run the suite locally before committing.

## Unity assertion vocabulary

This repo bundles a minimal Unity — not every macro from upstream is available. Use:

| Need | Macro |
|---|---|
| Integer equality | `TEST_ASSERT_EQUAL_INT(exp, act)` |
| Hex equality (32-bit) | `TEST_ASSERT_EQUAL_HEX(exp, act)` — there is no `_HEX16` |
| String equality | `TEST_ASSERT_EQUAL_STRING(exp, act)` |
| Float equality | `TEST_ASSERT_EQUAL_FLOAT(exp, act)` |
| Boolean | `TEST_ASSERT_TRUE(x)` / `TEST_ASSERT_FALSE(x)` |
| Pointer non-null | `TEST_ASSERT_NOT_NULL(p)` |

See [`unity.h`](native/unity.h) for the full set.

## What can't be tested host-side (yet)

- Anything that calls `lv_*` for real — would need a full native LVGL build (worth investigating if widget rendering becomes a frequent bug source).
- Anything threading-dependent (FreeRTOS) — would need a stub scheduler.
- Anything that talks to flash, NVS, or the LCD panel — has to run on hardware.

For those paths, see the API integration suite in `tests/api/`, which exercises endpoints on a flashed device over HTTP.

## Conventions

- Tests are deterministic. No randomness, no timing dependence.
- One test function = one logical assertion (or close family). Don't chain unrelated tests.
- Test names: `test_<unit>_<scenario>`, e.g. `test_arc_rule_color_restored_when_limiter_clears`.
- File names: `test_<source_file_or_concept>.c`.
- Mirror-pattern tests: source-of-truth reference goes in the file's header comment block. Reviewers check it; future you will thank you.
