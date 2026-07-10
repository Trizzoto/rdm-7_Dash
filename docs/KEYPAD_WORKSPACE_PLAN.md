# Keypad Workspace — design & integration plan (RDM Studio)

**Date:** 2026-07-10
**Parent:** `docs/PLATFORM_PLAN_2026-07.md` (Phase 1) · research: `docs/research/2026-07-keypad-market.md`
**Vision (Tommy):** select a keypad model → it appears on screen like a high-def image → LED rings glow the real colors → click any button to configure it.

That vision is exactly right — it's the WYSIWYG configurator no competitor has. One
engineering decision sharpens it: render the keypad **parametrically as vector
graphics**, not as photos.

## 1. Render approach: parametric vector, photo-grade

One keypad component draws any model from a descriptor (`{rows, cols, keySize,
variant}`): PKP-2200 (2×2) → PKP-2400 (2×4) → PKP-2600 (2×6) → PKP-3500 (3×5),
plus -LI large-key variants later. Housing, bezel, key wells, icon inserts, and
LED rings are drawn with layered gradients/shadows so it reads photo-grade.

Why vector beats photos here:

| | Photos (8+ models) | Parametric vector |
|---|---|---|
| LED glow in any of Blink's 9 colors, live | compositing hacks per photo | native — tint the ring, add bloom |
| Day/night brightness preview | no | yes (opacity/bloom scale) |
| Icon inserts swappable per key | no | yes — glyph library |
| Fits the ESP32-embedded editor (`index.html.gz` budget) | +100s of KB | a few KB of code |
| Crisp at any zoom / hi-DPI | no | yes |
| Same component can mirror **live state** | no | yes |

The mockup (see artifact "rdm-keypad-workspace-mockup") proves the look.

## 2. Two modes, one screen

- **Design mode** (offline or connected): click a key → inspector opens; edits
  apply to the config. LED preview animates the configured behavior (solid /
  slow / fast blink) in the chosen colors.
- **Live mode** (connected): the on-screen keypad mirrors the real unit —
  physical presses light the on-screen key, real LED states render live
  (button/LED states are channels, so this rides the existing
  `/api/signals/values` polling used by the live dash mirror). Clicking an
  on-screen key sends a **simulated press** so you can bench-test rules and
  ECU re-broadcast without touching the car. This is the dash live-mirror
  trick applied to the keypad — no competitor has anything like it.

## 3. UX flow

1. **Add keypad** — Devices ▸ Add… Two paths:
   - *Auto:* bus scan finds it ("PKP-2400 found — 125 kbit, node 0x15, CANopen. Adopt?") → adoption wizard (re-baud/re-address over SDO; bench-mode guard per platform plan §8).
   - *Manual:* model picker grid (rendered minis of each model) for offline design.
2. **Configure** — keypad rendered center-stage; click a key:
   - **Button**: label, channel name (auto-suggested from label), mode
     (momentary / toggle / 2–6-position / paired up-down with another key),
     short/long-press threshold.
   - **Function preset** dropdown: pit limiter, launch arm, fan override, pump,
     map switch, logging marker, **dash page next/prev, night mode toggle,
     brightness** (dash-native — work with no ECU at all).
   - **LED**: binding = *this button's state* (default) or **any channel**
     (the Haltech-style "LED follows the real thing" everyone loves — e.g. key
     LED red when `coolant_temp` crosses high_warn); per-state color from the
     9 Blink colors; solid/slow/fast blink; fault override.
   - **Keypad-level** (click housing): LED brightness day + night (0–63,
     hooked to existing night_mode), backlight color, protocol/node/baud info,
     re-broadcast profile (generic CAN DI frames for MaxxECU/Haltech/Link ECUs).
3. **Save** — `keypads.json` on the dash LittleFS (atomic tmp+fsync+rename+`.bak`
   idiom, same as `channels.json`); live-apply like layout edits.

## 4. Firmware work

- `main/can/bus_manager.c/h` — CANopen master shim: NMT start, heartbeat/bootup
  sniff, SDO client (0x2010 baud / 0x2013 node-id / model ident), TPDO1 button
  parse (0x180+id), RPDO LED/backlight writes (0x200/0x300+id). J1939 second
  (PGN 61184 + address claim).
- `main/can/keypad_manager.c/h` — model table; per-key state machine
  (raw press → momentary/toggle/n-state/paired logic → channel value);
  LED engine (evaluates bindings on channel change — a consumer of the
  existing rules engine — TX on change + periodic refresh + night hook).
- Channels: buttons register with `SIGNAL_SOURCE_KEYPAD`; stale after keypad
  heartbeat loss → widgets show stale state, LEDs recover on reconnect.
- API: `GET/POST /api/keypads`, `GET /api/bus/devices`, `POST /api/bus/adopt`,
  `POST /api/keypad/test_press`. Button state reads ride `/api/signals/values`.
  (Mind the `max_uri_handlers` tally.)
- Persistence: `keypads.json`; NOT part of the portable layout (device-local,
  like channels), but INCLUDED in the `.rdmcar` car project bundle.

## 5. Web editor / desktop split (ADR-0007)

The workspace is authored in **`main/web/index.html`** (phone-configurable —
a differentiator) and reaches RDM Studio through the normal
`tools/sync_firmware.py` pipeline. Desktop adds only: device-tree entry and
menu items (overlay blocks). Keypad field metadata should live in a schema
(`schema/keypads.schema.json`) + codegen like widgets do, so the inspector,
help tooltips, and firmware parser stay in lockstep.

## 6. Phasing

- **K0 — workspace shell, no device**: parametric renderer + inspector +
  `keypads.json` round-trip against the mock server. (The mockup artifact is
  the visual spec.)
- **K1 — CANopen live on the bench**: order a real **PKP-2400-SI (~US$280)**
  now — shim + adoption wizard + live mode verified against it.
- **K2 — J1939 + vendor presets**: adopt Haltech (0x0C) / MoTeC (0x0A) /
  pre-programmed Link/ECUMaster units ("reuse the keypad you already own").
- **K3 — depth**: LED-follows-channel everywhere, night pairs, re-broadcast
  profiles, multi-keypad (re-address second unit off 0x15), `.rdmcar` inclusion.

## 7. Open items

- **Order the bench keypad** (PKP-2400-SI) — everything in K1 blocks on it.
- Icon glyphs: draw our own set (ISO 7000-style automotive symbols) — don't
  copy Blink's laser-etch artwork files.
- Rotary variants (PKP-3500-SI-MT) out of scope until the base grid ships.
