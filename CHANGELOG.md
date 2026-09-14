# Changelog

All notable changes to the RDM-7 Dash firmware. Format loosely follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions are reported as `FIRMWARE_VERSION` from `main/include/version.h`.

This file starts tracking from **1.1.11** (the first release-tracked build, 2026-05-19). Earlier changes live in `git log` and topic files under `.claude/projects/.../memory/`.

## [Unreleased]

Changes that have landed on `master` since the last tagged version.

### Changed
- **"Your car also answers OBD2" — and what it could add.** (ADR-0074) After
  any ECU preset other than a Falcon's, the dash quietly checks whether the
  car answers OBD2 too. If it does, it lists the channels nothing feeds yet
  that OBD2 could fill, and offers them — nothing is added until you say so.
  - Wizard channels step: "checking for OBD2…", then **+ OBD2 can add N**,
    which opens the list already filled in, addable readings first.
  - "No ECU detected": when the car answers OBD2 the card says so — **Use
    OBD2 — your car answers 61 readings** — and puts that option first.
  - Device Settings → OBD2 readings shows **N TO ADD**.
  - Studio's Channels page shows the same offer with **Review and add…** and
    **Not now**. API: `GET /api/obd2/offer`, `POST /api/obd2/offer
    {"dismiss":true}`.
  - The ECU list uses the whole sheet and the presets' own names ("Ford Falcon
    FG", not "Ford  FG").
  - USB: new serial RPCs `touch` and `obd2.sim`, so a dash with no network can
    be tapped and bench-tested over its cable.
- **The setup wizard no longer asks everyone about OBD2.** (ADR-0073) It is
  four steps now — CAN, ECU, channels, connect. OBD2 was step 2 of 5 for every
  customer, though it only matters to two kinds of car:
  - **A Ford Falcon (BA/BF, FG) gets OBD2 set up by itself.** Applying the
    preset — in the wizard, from Studio's Quick ECU Setup or import, or after
    resetting the default layout — scans the car in the background and turns
    everything it answers that the broadcast lacks (timing, lambda, trims,
    pressures; on an FG also MAP and intake temp) into channels. With the
    ignition off it keeps trying, and resumes after a reboot, until the car
    answers once. Studio says so when the preset is applied, and the Channels
    header shows "OBD2 waiting for the car" until then.
  - **A car with no ECU preset** picks "My car uses OBD2" at the ECU step (on
    the "No ECU detected" card, or first in the ECU list). That screen scans
    straight away and sets up what the car reports, with Scan again and Back.
  - OBD2 lives in **Device Settings → OBD2 readings**, which opens the scan and
    reads WAITING FOR CAR while a setup is still owed.
  - Adding readings by hand from a scan list cancels a setup still owed, so a
    later background attempt never adds what you left unticked.
  - API: `/api/ecu/set` answers `obd2_autosetup: true` when it started one;
    `/api/ecu/current` carries `obd2_autosetup: "pending"`.
- **The car comes first in its own channel list.** (ADR-0071) The Channels
  page, the widget's channel picker and the dash's setup wizard now show "On
  this car" — every channel the dash has, custom ones in their own group — and
  then "Not set up", the rest of the catalogue. Before, a car's dozen channels
  were spread across sixteen group headings between greyed catalogue rows, and
  a channel you built yourself came after all of them.
  - The Source column prints the frame on a multiplexed id (`0x3E8 · frame 2`)
    and says "No source yet" only where that is true, instead of "Not set up"
    on 132 rows.
  - **Adding your ECU's extra signals** marks the ones the car already has,
    won't tick them twice, and offers "Tick all N not on this car". With no
    dash it opens on the ECU your channels already come from.
  - **Building a multiplexed channel by hand**: when the live probe sees byte 0
    change, "Use byte 0 as the frame index" turns the gate on, and every frame
    heard is listed with what your bits read in it — pick the frame by its
    value. Works in the new-channel form and in a channel's decode editor
    (where it waits for Save, like typing).
  - Quick ECU Setup's "Auto" box is now "Only ones the dash can hear", which
    is what it does, and no longer claims detection comes from "the last Scan
    Car".
  - The wizard reports a detected ECU as "Heard 5 of the 21 frames this ECU can
    send" rather than "(23% confidence)" for a correct match.
- **One source picker, and a decode editor you can see.** (ADR-0069) The
  Channels surface had three separate ECU → Version → Signal drilldowns reading
  the same catalogue, and the channel drawer hid the CAN decode under "More"
  in a 440px pane that squeezed its dropdowns to 89px — they read "Unsigne" and
  "Little (In".

  - **One `#chSourcePicker`, two modes**: `bind` (one channel →
    `/api/channels/bind-source`) and `add` (many → `/api/channels/import-preset`).
    A rail of sources grouped by the question you arrived with, the version as a
    chip, and the signal list given the room — because which ECU the car runs is
    a car-level fact, not a per-channel one. Search is global, so you can find
    "coolant" without knowing your ECU calls it ECT.
  - **It works with no dash.** Both modes fall back to the catalogue baked into
    the page (ADR-0033) and say so. Previously "Pick a source" had no fallback
    at all and rendered "Failed to load sources" offline, while the bulk-import
    picker one menu item away listed every ECU we ship.
  - **"Type in a CAN decode…" is in the picker** — the missing create action
    that sent people hunting for a "custom" option.
  - **The decode editor is its own section**, `clamp(520px, 40vw, 760px)` wide
    on an auto-fit grid, carrying the live bus probe and the mux fields. That
    "is my bit offset right" answer previously existed only for the first ten
    seconds of a channel's life.
  - The Signal Manager is renamed **Preset library** and reframed as authoring;
    presenting it as a third picker is how it kept getting opened by people who
    wanted a source.
  - ~700 lines of superseded picker code removed, so the page grew the firmware
    binary by under 2 KB despite gaining a whole new surface.

### Fixed
- **Changing a car's ECU left the old ECU's decodes live.** Applying a preset
  only ever added signals, so after Haltech → Ford Falcon 30 Haltech decodes
  (throttle and MAP on 0x360, ignition on 0x362, oil pressure on 0x361…)
  stayed registered and came back on every boot. A channel bound by name then
  read the old ECU's bit layout — a wrong number on the new car's bus — and
  OBD2 would not poll it. A new ECU now retires exactly what the previous
  preset wrote. (ADR-0074)
- **The dash forgot its ECU at every boot** when it was chosen in the setup
  wizard or imported from Studio: it was saved where the next layout load
  overwrote it. It is now stored with the layout. (ADR-0074)
- An OBD2 reading could show its source as "CAN 0x0".
- The dashboard's NO CAN BUS badge covered the setup wizard's CAN step text.
- The OBD2 scan list put "already set up" rows first, pushing what can be
  added off the screen, and counted scan scaffolding as readings.
- **AFR on E85 read petrol numbers, and Calculate could read 216.** (ADR-0072)
  λ → AFR always used 14.7, so an E85 car at stoich showed 14.7 where the tune
  says 9.8. Each dash now has a **Fuel** setting — Petrol (the default, so
  nothing changes on update), E10, E85, E100, Methanol, or **Flex**, which
  follows the Ethanol % channel. It appears in the channel drawer next to
  "Display as" on any λ or AFR channel and applies to every AFR readout at once.
  Blends mix by mass (E85 = 9.81, E10 = 14.10).
  - Calculate… `Lambda × 14.7` onto an AFR channel read 216 (and `× 9.8` read
    144): the result was converted λ → AFR a second time. Scaling a channel by
    a constant is now treated as a hand conversion and left alone. Trade:
    `MAP × 0.5` onto a psi channel reads the raw half, not a converted one.
  - API: `GET/POST /api/fuel/config` (`{"fuel":"e85"}`); `/api/channels` now
    carries `stoich` and `fuel`.
  - **Flex says where its number comes from.** Under Fuel = Flex the drawer
    reads "Reading 72% ethanol from Ethanol % (E85 · 0x531)", or says the
    channel isn't set up (with a button to it), or that it's waiting for a
    reading. It also spots an ethanol reading living in a channel of its own
    ("Your Flex % channel isn't read by Flex"). Before, Flex with nothing
    feeding it looked exactly like Petrol.
  - **MaxxECU's `E85 %` now lands on Ethanol %**, like Link's `ETHANOL %` and
    Haltech's `FUEL COMP`. It used to become a custom channel, which Flex
    never read. Dashes that already applied the MaxxECU preset keep their
    custom channel; re-apply, or give Ethanol % the same source.
- **The setup wizard hid most of the channels applying an ECU created.** The
  dash's channel step listed the 135-row catalogue against a 144-row cap and
  put custom channels last, but applying an ECU makes a custom channel for
  every signal without a built-in slot (most of a Haltech's 69), so only about
  seven could appear. The car's channels now come first; a full list trims the
  catalogue's tail and says how many rows it left out. (ADR-0071)
- **After creating a channel, the drawer kept the heading "New channel"** over
  the new channel's editor.
- **Every same-named source row claimed "in use".** `/api/channels/source-options`
  marks `is_current` by derived signal *name*, so on a car running one ECU all
  nine makes' "COOLANT TEMP" rows said they were the live source. Tolerable when
  you could see one ECU at a time, plainly wrong once search shows them all. The
  row now compares the channel's actual decode — frame, bit offset and frame
  index (ADR-0005 makes that the authoritative fact).
- **The live bus probe labelled native values with the display unit.** It
  decodes raw → native, then printed `units_display`: a channel storing kPa and
  showing bar rendered a raw 416 kPa as "416.00 bar", off by 100x, in the one
  readout whose job is catching exactly that.
- **The browser dev server's ECU catalogue was a hand-typed stub** that had
  rotted into the pre-1.4.1 model of Link as consecutive ids (`0x3E8`/`0x3E9`/
  `0x3EA`), so reviewing the picker against it concluded the catalogue was
  unpopulated when the firmware had carried all 41 Generic Dash rows for months.
  It is now derived from the page's baked catalogue and cannot drift.
  `/api/channels/bind-source` was not mocked at all — it fell through to a
  catch-all `{ok:true}`, so a bind appeared to succeed while changing nothing.

### Added
- **A custom channel can decode a multiplexed CAN id.** The firmware's decode
  gate has always been general — `signal_dispatch_frame()` skips any frame
  whose mux field does not match — but the only way to reach it was a baked
  preset. Building a muxed channel by hand meant POSTing JSON at
  `/api/channels/create` by hand, because no form carried the fields. Now every
  authoring surface does, and the DBC importer no longer silently throws the
  multiplexing away.

  - **DBC import keeps the frame index.** `parseDbcFile()`'s signal regex
    matched the `M` / `mN` multiplexer marker with a NON-capturing group and
    discarded it, so a multiplexed DBC imported without error and produced
    channels that all decoded interleaved payloads on one id — confident
    garbage rather than an obvious failure. The marker is captured now and
    resolved in a second pass per message (`_dbcResolveMux`), because the `M`
    switch is not required to appear before the signals it gates. Messages with
    `mN` signals and no `M` are malformed per the spec; they import ungated and
    are reported by name, since guessing byte 0 would just be a different
    silent wrong answer. `.dbc` export round-trips the mux and is idempotent.

  - **Mux fields on all three decode forms** — the new-channel form, the custom
    preset signal form, and the legacy custom signal form — behind a disclosure,
    since most ECUs are not multiplexed.

  - **The live bus preview no longer lies about a muxed id.**
    `/api/can/monitor` returns one frame per id, whichever arrived last, so on
    a stream cycling 14 payloads the preview decoded a different quantity every
    poll. `?focus=<id>&mux_start=&mux_len=` aims a per-mux-value buffer in the
    id tracker (one id at a time, lock-free by request/adopt split) and the
    reply carries the latest frame for each index. When the index you asked for
    has not arrived, the preview says so and lists the ones that have, instead
    of decoding a different frame. Each `ids` entry also exposes `mux_seen`, so
    the editor can warn that an id looks multiplexed before anything is set up.

  - `/api/signal/update` accepts the mux triple, so a muxed signal registers
    live with its gate instead of decoding everything until the next reload.

  - Reference: [`docs/MULTIPLEXED_CAN.md`](docs/MULTIPLEXED_CAN.md) — the
    contract, the per-surface checklist, and how the mapping works from DBC,
    Link PCLink, AiM and MoTeC.

### Changed
- **MaxxECU is one preset again, not two.** The 1.2 and 1.3 entries were
  separate all the way through `ECU_PRESETS`, the preconfig catalogue and the
  picker's version column, but 1.3 is a strict superset: every signal the v1.2
  DBC defines sits at the same CAN id, bit offset, length, scale and signedness
  in v1.3, which only adds further frames (oil temp/pressure, fuel pressure,
  user channels, status flags). The split therefore asked the user to pick a
  firmware version that could not change what they got — and a wrong guess
  toward 1.2 hid 52 signals. A 1.2 unit simply never broadcasts the extra
  frames, and an unbroadcast channel reads stale rather than wrong.
  `ecu_preset_find()` maps a saved `"1.2"` onto the merged entry, so devices
  and layouts configured before this keep resolving.

### Fixed
- **Saving a custom preset no longer drops fields from its other signals.**
  Three call sites rebuilt the whole signal list by picking fields by hand, so
  anything not on those lists was silently dropped from every other signal as a
  side effect of adding or deleting one. That is how `unit` went missing, and
  multiplexing would have gone the same way. All three now share
  `_presetSigFields()`.

### Added
- **The dash plays a keypad's boot, with no laptop in the car.** A Blink keypad
  cannot animate itself — its only self-driven show is the fixed factory one in
  CANopen `2014h` — so a designed boot needed RDM Studio open on a laptop,
  streaming frames through the WiFi↔CAN gateway. The dash is the host now
  (ADR-0068): Studio bakes the boot to timed frames and posts the tape, the dash
  stores it on LittleFS and replays it at every power-up.

  - `main/can/keypad_lights.{c,h}` — a tape player, deliberately nothing more:
    no effect engine, no button state machine, no channel bindings. It NMT-starts
    the node, waits out the keypad's own start-up show, and transmits on the
    tape's own clock. Three refusals are the design — it will not play on a bus
    that is not on the bitrate the boot was made for (this runs at ignition in a
    moving car, and re-timing the bus to light some LEDs is not a trade anybody
    would take), it will not play while the bus scan owns the CAN peripheral, and
    it is capped at 24 frames a second like `/api/can/send`.
  - Tail contract carried across from ADR-0059: `loop:false` means the last frame
    is the hand-back and the player then goes silent, leaving the rings to
    whatever normally drives them; `loop:true` means the frames from
    `loop_from_ms` are one turn of a resting animation and get repeated.
  - `GET`/`POST /api/keypad/lights` (`main/net/web_server_keypad.c`) — store,
    report, play, stop, forget, and the at-power-up flag. Two URI slots, actions
    in the POST body. A bad upload is validated into memory and rejected before
    anything is written, so it can never replace a working boot.
  - **Tests**: `tests/native/test_keypad_lights.c` — 10 tests over the parse, the
    refusals, the bounds and the tail, run against a fixture generated by Studio
    itself (`node tools/check_lightshow.js --emit-tape ...`) so the two repos
    cannot drift apart quietly.

- **Live "is this receiving signal right now?" indicators across the picker chain.** The on-device OBD2 PID picker already had per-row blue badges showing live receive status; this release extends the same pattern up and across:

  - **ECU preset picker (on-device + web)** — each preset row carries a live blue dot that lights up when the bus is broadcasting the preset's CAN IDs and dims when it isn't. Detected presets sort to the top with an "NN% match" badge. The on-device picker was rewritten from a two-dropdown UI to a scrollable card list to fit per-row dots. Auto/Manual switch at the header filters out non-detected presets when the user wants to declutter (falls back to showing all if nothing matched, so the user is never stranded).
  - **Signals screen (on-device `ui_peaks.c`)** — each signal row gets a small dot on its left edge: blue when `signal_t.is_stale == false`, grey otherwise. 100 ms cadence so a freshly arriving frame lights up within a tick.
  - **Web sidebar signal cards** — `.sig-dot` now gets a `.live` class toggled by the existing `/api/signals/values` poll loop (1.5 s cadence). Three states: blue + glow when live, green when bound-but-stale, grey when neither.
  - **OBD2 Setup modal** — new "OBD2 Protocols responding" panel showing one chip per service (M01, M02, M03, M07, M09, M0A, M21, M22) with a live blue dot when the ECU has responded to that service within the last 5 s. Helps diagnose partial OBD2 connectivity ("Trouble Codes works but Vehicle Info times out — why?"). Title hover shows "responded N s ago" / "no response yet" for triage.

  Implementation pieces:
  - `ecu_preset_match_score(preset)` returns 0..100% based on the **live** `can_id_tracker` (continuous per-CAN-ID `last_seen_us` recording), not the static `can_bus_test` scan. Threshold `ECU_PRESET_MATCH_THRESHOLD = 30%`. Plug a loom in → dot lights within ~2 s. Unplug → fades within ~2.5 s.
  - `/api/ecu/list` response shape changed from a bare array to `{presets: [...], match_threshold: 30, auto_mode: bool}`. Each preset carries `match_score`. New endpoints `GET /api/ecu/picker_mode` and `POST /api/ecu/picker_mode` for the Auto flag (persisted in NVS namespace `ecu_pick`).
  - `obd2.c` tracks per-service last-response timestamps in `s_last_resp_ms[]`, stamped from `_process_full_message` on every positive response (NRCs not counted). New `obd2_protocol_is_fresh(service)` + `obd2_protocol_age_ms(service)` public API. `GET /api/obd2/protocols` returns the per-service freshness array.
  - On-device picker uses a 500 ms refresh timer; web modal polls `/api/ecu/list` every 1.5 s; OBD2 Setup polls `/api/obd2/protocols` every 2 s — same loop as the rest of its status grid.
  - **Tests**: `tests/native/test_ecu_preset_match.c` — 16 mirror-pattern tests covering degenerate inputs, intersection math, the 30% threshold boundary, rounding, de-duplication, and two realistic preset shapes. Suite now **157 tests, all green**.

### Changed
- _nothing yet_

### Fixed
- **`web_server_name_is_safe` / `web_server_filename_is_safe` UTF-8 portability**: cast input byte to `(unsigned char)` before the `< 0x20` control-char check. Pins the signedness behaviour across host gcc and ARM toolchains so UTF-8 multi-byte sequences are consistently accepted everywhere. `tests/native/test_web_path_safety.c::test_name_utf8_is_accepted` (formerly `test_name_high_ascii_rejected_when_char_is_signed`) flipped to assert TRUE for `éclair` and `日本` and locks the cast in.

---

## [1.1.11] — 2026-05-19

First release-tracked build and the **opening of the 2-week stabilisation window** before customer release. Significant cleanup + documentation pass plus the cumulative work since 1.1.10. Boots clean on COM13; web editor serves with `Content-Encoding: gzip`; **141/141 native tests pass**; pre-release audit walked end-to-end.

### Added
- **Ford FG preset — chassis extras.** New optional ECU signal slots `BOOST`, `FUEL_LEVEL`, `PARK_BRAKE`, `YAW_RATE`, `LATERAL_G` (in `ecu_signal_slot_t`). FG preset wires these from `0x425` / `0x4B0` / `0x000` / `0x360` per the BigFalconSheet DBC tab. Most factory presets leave them `SIG_UNSUPPORTED`. (commit `40b2d5e`)
- **OTA "Skip This Version" button.** OTA prompt gains a third button alongside Later / Install. Stores the offered version in NVS namespace `ota_cfg`; popup stays silent until upstream version differs. Cleared by factory reset. (commit `02a2588`, prior release)
- **OBD2 Mode 02 freeze frame** — tap-DTC-to-see-conditions flow. (commit `a0de71c`, prior release)
- **Schema migration skeleton** (`_migrate_layout_root`). Defined landing site for future field renames / removals / semantic changes. Currently a no-op switch — every transition through `LAYOUT_SCHEMA_VERSION=14` is additive. (commit `94b578b`)
- **`widget_arc` redline / limiter polish** — redline-zone arc, limiter flash/solid, value text overlay. (commit `5fe5c87`, prior release)
- **`widget_meter` port** from RDM-7_Dash_P4 — rear-extension partial refresh, ext-draw-size hook, anchor-aware tick recolor, `tick_label_divisor` field. (commit `e9b28ad`, prior release)
- **OBD2 hamburger section** (web editor) with Setup / Trouble Codes / Vehicle Info modals routing through existing `/api/obd2/*`. (commit `d43c7fa`, prior release)
- **Web editor: Save As menu** + meter sweep slider (30..360) + `label_gap` range widened to `-150..150`. (commit `e6f1a9f`, prior release)
- **Native test suite expansion**: 76 → 141 tests (+86%). New files: `test_layout_migration.c` (schema gate + migration helper), `test_widget_arc_precedence.c` (19 tests for the fill-color precedence ladder including the just-fixed regression), `test_obd2_freeze_frame.c` (21 tests for Mode 02 parse + scale/offset), `test_ota_skip_version.c` (12 tests for the comparator), `test_calculated_gear.c` (13 tests for the CALCULATED_GEAR classifier), `test_web_path_safety.c` (19 tests for path-traversal guards at the HTTP API boundary). All mirror-pattern with explicit source-of-truth references.
- **GitHub Actions CI**: `.github/workflows/native-tests.yml` runs the 141-test suite on every push/PR. `.github/workflows/schema-check.yml` validates `schema/widgets.schema.json` and detects codegen drift in `main/web/index.html` / `main/widgets/widget_fields.gen.c` before it reaches master.
- **Documentation**: `tests/README.md` refreshed to current state; `CHANGELOG.md` (this file); `SECURITY.md` (release-surface security posture + closed pre-release audit checklist); `docs/README.md` (documentation map); `docs/adr/README.md` (ADR index); `docs/adr/0005-html-source-of-truth.md` (the three-HTML-copies decision); root `README.md` linked to the handover docs.

### Changed
- **Embedded web editor is now gzipped** (`main/CMakeLists.txt` + `main/net/web_server.c`). 842 KB → 181 KB on the wire (4.65× reduction). Firmware image shrank from 0x31c1b0 → 0x27aa40 (-645 KB). **OTA partition free slack: 11% → 29%** (+18pp). Browsers handle `Content-Encoding: gzip` transparently. (commit `79c2789`)
- **CAN filter always includes the OBD2 response range** `0x7E8..0x7EF`, regardless of polling state. Previously gated on `obd2_is_running()`, which caused one-shot diagnostic endpoints (DTC read/clear, VIN, ECU name) to silently time out on native broadcast presets like FG or BA/BF. (commit `1c3f61b`)
- **`usb_cdc_protocol.c/h`** now flags its out-of-build status in a `BUILD STATUS` header block — won't show up as an "orphan" in future audits. USB CDC ACM and the USB Serial JTAG console can't coexist on the ESP32-S3, so this stub stays compiled-out until the console flips. (commit `a3ff0af`)
- **Tauri desktop bundle config** — `tauri.conf.json` now declares `targets: ["nsis", "msi"]` so `cargo tauri build` produces installers (.exe + .msi) alongside the standalone executable. (desktop repo)

### Fixed
- **`widget_arc` rule color survives limiter / redline zone exit.** When both a widget_rule arc_color override and the limiter were active on the same widget, the rule color was lost as soon as the zone cleared — `_arc_apply_fill_color` always restored `d->arc_color`, not the rule-overridden colour. Documented limitation deferred from `c338a6c`; now fixed by caching the rule colour in `arc_data_t._rule_arc_color` and reading it as the "normal" base inside `_arc_apply_fill_color`. (commit `c9262b8`)
- **`max_uri_handlers` 100 → 160** in `web_server.c` (prior release, retained). Current count is 100 handlers registered (62% of cap, 60 slots free).

### Security & Operational
- **Pre-release audit walked end-to-end** (2026-05-19). 4 checklist items in [`SECURITY.md`](SECURITY.md) closed (dependency CVE walk, reproducible build, factory layout safety, boot-log secret scan). 2 items deferred to v1.2 with documented reasoning (HMAC per-device derivation, OTA manifest signing). 2 waived with documented decision (Supabase Pro plan, flash encryption posture).
- **Reproducible build verified**: `idf.py fullclean && idf.py build` produces firmware size 0x27aa40 (2,599,488 bytes), stable across fresh checkouts. Binary SHA-256 varies run-to-run because ESP-IDF embeds build-time UUIDs/timestamps; size + partition layout are stable.
- **Boot-log audit**: no firmware code logs the CAN HMAC secret, WiFi passwords (only metadata: `"saved for '<ssid>'"`, `"updated"`, `"cleared"`), or OTA tokens at any log level.
- **Dependency CVE walk**: managed components (`esp_new_jpeg` 0.6.1, `littlefs` 1.20.4, `LVGL` 8.3.11) are current releases with no outstanding advisories. Desktop Cargo: initial visual review was clean; **`cargo audit` (run same-day after install completed) found 3 actual vulnerabilities** in `rustls-webpki 0.103.10` — TLS cert-validation flaws (RUSTSEC-2026-0098/-0099) and a CRL parse panic (RUSTSEC-2026-0104), all on the auto-updater TLS path. **Patched same-day** via `cargo update -p rustls-webpki` → 0.103.13 in the desktop repo. 20 "no longer maintained" advisories triaged and ignored in `src-tauri/.cargo/audit.toml` with per-entry rationale — all Linux-only GTK3 or build-time parsers, not exploitable on the Windows desktop path users run.
- **Factory layout safety**: factory default is compiled into `main/layout/default_layout.c` (widget positions only — no creds, no debug signal sources), not shipped as a JSON file.

### Found during audit (deferred fixes)
- _The portability bug logged here at v1.1.11 was fixed in the post-tag patch — see `[Unreleased] / Fixed` above._

### Internal
- `feature/widget-sys` fast-forwarded to `master` at `c88a42c`. Both branches pushed to origin.
- Desktop repo carries a local-only commit `f07ca2c` from the most recent sync pass — image `auto_size`, button/toggle momentary fields, duplicate-reverse cleanup. Not pushed.

---

## How to update this file

When you land a change worth tracking:

1. Add a bullet under `## [Unreleased]` in the relevant subsection (Added / Changed / Fixed / Security & Operational / Internal).
2. Keep the bullet single-line where possible. Wider context goes in the commit message.
3. Reference the commit SHA in parentheses at the end of the line: `(commit \`abc1234\`)`.
4. When cutting a release, rename the `[Unreleased]` heading to `[X.Y.Z] — YYYY-MM-DD`, create a fresh empty `[Unreleased]` block at the top, and bump `FIRMWARE_VERSION` in `main/include/version.h`.

Skip changes that are pure-refactor / no-behavior-change / typo-fix unless they're load-bearing for a future release note.

## Versioning policy

Semantic-ish: MAJOR.MINOR.PATCH.

- **PATCH** — bug fix, doc, or test-only change; no behaviour change for the user.
- **MINOR** — additive feature; old layouts and old API clients keep working.
- **MAJOR** — breaking. Old layouts may need migration; old API clients may break.

`LAYOUT_SCHEMA_VERSION` (in `main/layout/layout_manager.h`) bumps independently — track it in the bullet that introduces the schema change.
