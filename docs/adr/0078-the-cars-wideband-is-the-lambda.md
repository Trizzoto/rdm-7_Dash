# ADR-0078: The car's wideband is the lambda, λ draws, and a bench Haltech

Date: 2026-09-15
Status: Accepted
Repos: RDM-7_Dash (`main/ui/settings/preset_picker_data.c`,
`main/ui/kit/ui_kit.c`, `main/ui/theme.h`, `main/embed/fonts/glass_symbols.ttf`,
`main/can/can_sim.c`, `main/can/can_bus_test.c`, `main/can/can_manager.c`)
Follows ADR-0074 (a car that answers OBD2 is told so) and ADR-0075 (one kit).
`0077` is claimed by the CAN emulation work (`can_emu.h`).

Found while making screenshots for a first-turn-on guide: a bench Haltech
Nexus at 1 Mbps, OBD2 present, the setup wizard walked end to end.

## 1. Lambda switched from the Haltech to OBD2

**What happened.**
- Step 3 offered "OBD2 can add 10", and one of the ten was **Lambda Bank 1**.
- After Add, the dashboard's LAMBDA tile read 0.12–0.33.

**Why.**
- The wizard binds a preset from `preconfig_items[]`. The Haltech rows had the
  wideband only as AFR ("WIDEBAND 1", 0.0147/bit), which lands on Wideband 1.
- Nothing fed `lambda_bank1`, the channel the default layout's LAMBDA tile shows.
- ADR-0074's offer sees an unbound channel and fills it. For `lambda_bank1` the
  OBD2 map uses PID 0x44, the **commanded** equivalence ratio: the ECU's target,
  not a measurement.
- `ecu_presets.c` already decoded Haltech lambda (0x368, 0.001 λ/bit) in its
  slot table. The wizard path just never used it.

**Decision.**
- The Haltech preset carries the wideband bytes a second time as lambda:
  "LAMBDA 1" and "LAMBDA 2", on 0x368 bytes 0–1 and 2–3, 0.001/bit.
- The existing aliases map `LAMBDA_1` to `lambda_bank1` and `LAMBDA_2` to
  `lambda_bank2`.
- So the tile shows the car's own sensor, and OBD2 has no gap to offer.
- The OBD2 poller already skips a PID whose signal name a CAN broadcast owns,
  so 0x44 can't overwrite it either.

**Not done here.** PID 0x44 still maps to `lambda_bank1` on OBD2-only cars.
- It should be `target_lambda`.
- But the OBD2 signal is literally named `LAMBDA`, the same name as the CAN
  slot, so remapping the channel alone would bind Target Lambda to the car's
  measured lambda.
- Fixing it needs the OBD2 signal renamed, with a migration for layouts that
  saved it. That is a separate change.

## 2. λ drew as a box

- Montserrat (LVGL's built-in faces) and the kit's Barlow have no Greek.
  Every lambda unit on the glass read "0.99 □": the OBD2 sheet, the channel
  editor, the unit dropdowns.
- The graphs screen had been hiding the unit to dodge it.
- `main/embed/fonts/glass_symbols.ttf` is DejaVu Sans, the copy LVGL ships in
  `scripts/built_in_font`, subset to λ μ µ Δ ° ± × ÷ ² ³ · – — … ≈ ≤ ≥ (12.8 KB,
  most of it the licence text the font requires to travel with it).
- It is attached as an LVGL `fallback`, rendered by tiny_ttf:
  - on the three Barlow kit faces;
  - on RAM copies of Montserrat 10–22. The built-in faces are `const` in flash,
    so they can't take a fallback themselves.
- `THEME_FONT_TINY…XLARGE` now call `ui_theme_font(px)`. It returns the copy
  once `uk_init()` has run, and the plain face before that, so nothing drawn
  earlier breaks.
- Only missing glyphs reach DejaVu. Existing text renders exactly as before.
- The graphs screen shows λ again.

## 3. A bench Haltech (`can.sim`)

- Serial RPC `can.sim {"on":true,"ecu":"Haltech","version":"Nexus"}` starts it.
- It builds every CAN row of that preset into frames at 10 Hz, with warm-idle
  values, and injects them through `can_inject_rx_frame`.
- So detection, decoding, the channels editor and gauges all run the real code.
- Bits are placed using the dash's own `can_extract_bits` as the oracle, so each
  field reads back exactly.
- A mixture is one number, whatever unit a row carries it in, so the AFR and
  lambda rows over the same bytes encode the same raw value.
- The bitrate scan listens to the TWAI driver directly. While the sim runs it
  reports the sim's IDs and frame count at the preset's rate: 1 Mbps for
  aftermarket ECUs, 500 k for the factory ones.
- Injected frames now count as received frames, so "NO CAN BUS" and bus health
  treat the stream as traffic.
- Not persisted: a restart turns it off.

## Consequences

- Good: a Haltech car's LAMBDA tile is the wideband, set up by the wizard alone.
- Good: units and symbols draw on every menu without per-string workarounds.
- Good: setup can be demonstrated and screenshotted with no car.
- Bad: Haltech channels now list the same sensor twice, as Wideband 1 (AFR) and
  Lambda Bank 1 (λ). That is deliberate, but it is two rows.
- Bad: seven more small tiny_ttf instances in PSRAM, and 12.8 KB of flash on an
  app partition with about 3 % free.
- Neutral: PID 0x44's mapping for OBD2-only cars is unchanged (see §1).
