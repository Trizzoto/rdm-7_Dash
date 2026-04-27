# Native-Host Tests

Tests for code that doesn't depend on ESP-IDF / FreeRTOS / LVGL / hardware. Built and run on the developer's machine with a stock C compiler — **no `idf.py build` required**.

## Why native-host

ESP32 firmware is hard to test on the device — there's no shell, no test runner, no easy way to capture failures. Anything that *can* be tested as pure logic on the host *should* be, because:

- The build/run cycle is sub-second instead of ~2 minutes.
- Failures are debuggable with a normal debugger.
- CI can run the tests without a board.
- Refactors land with confidence.

What can be tested on the host today:
- **CAN decode** ([test_can_decode.c](native/test_can_decode.c)) — `can_extract_bits` and `can_pack_bits` are pure math, zero deps.
- **Layout migration** (planned) — pure JSON manipulation via cJSON.
- **Widget JSON round-trip** (planned) — needs LVGL stubs in `mocks/lvgl.h`.

What can't (yet):
- Anything that calls `lv_*` functions for real (would need a full LVGL native build).
- Anything threading-dependent (FreeRTOS).
- Anything that talks to flash, NVS, or the LCD panel.

## Layout

```
tests/
├── README.md          (you are here)
├── native/
│   ├── Makefile       (build rule per test, plain GCC, no CMake)
│   ├── unity.h        (vendored Unity test framework, single header)
│   ├── unity.c        (single source)
│   ├── test_can_decode.c   (signal/CAN bit extraction tests)
│   └── …              (more as agents add them)
└── mocks/
    └── (per-test stubs of LVGL / ESP-IDF when needed)
```

## Build & run

From the repo root:

**Linux / macOS / WSL** (with `make`):
```bash
cd tests/native
make                  # builds and runs every test
make test_can_decode  # builds and runs one test
make clean
```

**Windows** (PowerShell, gcc on PATH — works with MinGW or MSYS2):
```powershell
cd tests\native
.\run.ps1                  # builds and runs every test
.\run.ps1 test_can_decode  # builds and runs one test
.\run.ps1 -Clean
```

Tests print one line per assertion (PASS/FAIL) and a final summary. Exit code is 0 on success, non-zero on any failure — wire that into CI.

## Adding a new test

1. Create `tests/native/test_<name>.c`.
2. Include `unity.h` and the source file(s) under test (e.g. `#include "../../main/can/can_decode.c"`).
3. Each test function: `void test_my_thing(void) { TEST_ASSERT_*(...); }`.
4. Wire it up:

   ```c
   int main(void) {
       UNITY_BEGIN();
       RUN_TEST(test_my_thing);
       return UNITY_END();
   }
   ```

5. Add to the `TESTS` list in `Makefile`.
6. `make test_<name>` should pass.

## When to mock vs. include real source

- **Pure logic with no deps** → include the real .c file directly.
- **Logic that pulls in LVGL/ESP-IDF** → write a focused stub in `mocks/`. Keep stubs to the minimum needed (struct shape + no-op functions). A 30-line stub beats a 3000-line LVGL build.
- **Anything you find yourself wanting to mock more than twice** → factor the testable code into a header-and-pure-source pair so the next test doesn't need new mocks.

## Conventions

- Tests are deterministic. No randomness, no timing dependence.
- One test function = one logical assertion (or close family). Don't chain unrelated tests.
- Test names: `test_<unit>_<scenario>`, e.g. `test_can_extract_bits_motorola_signed`.
- File names: `test_<source_file>.c`, e.g. `test_can_decode.c` for `can_decode.{c,h}`.
