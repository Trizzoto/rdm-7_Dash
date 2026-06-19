# Continuation prompt — RDM-7 review-fix burndown

Paste the block below into a new Claude Code session (run from
`C:\Users\ruuva\workspace\RDM-7_Dash`) to pick up where 2026-06-11 left off.

---

Continue burning down the RDM-7 comprehensive-review findings. Context:

**State**
- Branch `feature/widget-sys`, pushed to origin (last commit `edaee5b`).
- The full worklist is `REVIEW_FINDINGS_2026-06-11.md` in the repo root (~70 findings, severity + effort + file:line + fix).
- Already done + verified on-device + pushed this round: both CRITICALs (C1 atomic layout save, C2 rules-double-handling UAF) and HIGHs H5 (lv_async_call wrapper), H6 (channels.json save lock), H8 (font LVGL lock), H12 (CAN task WithCaps leak). H15 is only PARTIALLY done (FS read moved out of the lock; the ~1.7 s build-under-lock remains).
- The memory topic file `project_2026_06_11_comprehensive_review.md` has the detail.

**Device + build**
- Dev port COM27. Dash web API at http://192.168.4.61.
- Build: `. "$env:IDF_PATH\export.ps1" *> $null; idf.py build` (run long builds in background). Flash: `idf.py -p COM27 flash`. Flash+monitor pre-approved.
- clang/clangd diagnostics (`../hal.h not found`, `fsync` undeclared, `mkdir` too-many-args, host format-specifier) are host-index NOISE — `idf.py build` is the source of truth.

**Agent test kit (built this round — USE IT to verify every fix)**
- `GET /api/selftest` — uri tally / littlefs / can / heap / channels / signals / fonts + LVGL-lock latency.
- `GET /api/widgets` — live widget tree (id/type/pos/field values/rules) under the lock.
- `POST /api/can/inject` `{"id":864,"data":"hex-or-[bytes]","count":1,"extd":false}` — feeds the REAL decode path; assert the result on `/api/channels` (now includes `zone`).
- `GET /api/screenshot/hash?x&y&w&h` — deterministic region hash + `torn` flag.
- `tools/device_smoketest.ps1` is a starter harness. Python+urllib scripts work well for round-trip asserts.

**CRITICAL workflow gotcha — entangled files**
The working tree has uncommitted PRIOR-SESSION work in several files (boot-anim handler in `web_server_layout.c`, plus `splash_screen.c`, `ui_Screen3.c`, `widget_arc.c`, `widget_text.c`, etc.). When you commit a fix that touches one of these, you MUST stage only YOUR hunks, never the prior work. Technique that worked: `git diff HEAD -- <file>` → split into hunks → keep only hunks containing your change's signature string AND excluding prior-work signatures (e.g. exclude `boot_anim`) → `git apply --cached`. Files NOT in that entangled set can be `git add`-ed directly. Always `git diff --cached` to confirm only your changes are staged before committing.

**Per-fix loop:** read the finding in REVIEW_FINDINGS → implement → `idf.py build` (background) → flash COM27 → verify on-device with the test kit → commit (isolated, one logical fix per commit, end message with the Co-Authored-By line) → update the memory topic file.

**Suggested next, by tier:**
1. Safe: H15-full (snapshot the signal registry under a brief lock, build the source-options JSON unlocked — closes the ~1.7 s freeze); H14 (web `index.html`: channels modal silently drops failed threshold/decode edits — re-queue + toast); docs sweep H20-22 (CLAUDE.md/handover: schema v13→14, "13 widgets"→15, add `main/data/` channel section, ADR-0005 numbering).
2. Risky / do standalone with care: H11 (enable `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` + `esp_ota_mark_app_valid_cancel_rollback()` after a healthy boot + `CONFIG_ESP_TASK_WDT_PANIC=y` + subscribe the LVGL task) — a mistimed mark_valid causes OTA rollback loops; test the mark-valid path carefully. H10 (SD hot-removal) needs a card to exercise.
3. Other repos: marketplace H3 (server-side layout validation before `is_published`) + H4 (private paid bucket + signed-URL download) in `C:\Users\ruuva\workspace\rdm7-marketplace`; studio H16-H19 in `rdm7-studio-web`.

Start by reading REVIEW_FINDINGS_2026-06-11.md and picking the next item; confirm with me before the risky boot-behavior tier.
