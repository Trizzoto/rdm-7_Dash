# ADR 0010 — RDM IO: three-tier product ladder (Pico / Core / Pro)

Status: Accepted (2026-07-21) — extends ADR 0009 downward; supersedes the
single-SKU "RDM IO" row of `PLATFORM_PLAN_2026-07.md` §4 (plan amended in
place). Hardware not yet started; this fixes the target specs, prices and
build order.

## Context

ADR 0009 positioned the flagship expander ("don't chase the AU$200 floor")
after benchmarking the PT Motorsport line (Nano AU$99 → Mini2 AU$200 → DTM
AU$299). Tommy's direction 2026-07-21: also serve the bottom of the market —
*"a very cheap one, for the DIYers to just connect buttons and simple stuff
to"* — so the range covers DIY, mid, and the gap product.

The economics support it: PT proves an AU$99 box is viable on a ~US$10 BOM,
and our whole differentiation (WiFi + free cross-platform suite + dash
gateway + CAN emulation modes + DBC export) is **firmware and software we
already have to build for the flagship** — reusing it down-market costs
almost nothing per unit.

## Decision — the ladder

| Tier | Name | IO | Platform | Connect | Price (AUD) | Beats |
|---|---|---|---|---|---|---|
| 1 | **RDM IO Pico** | 8× DIN (buttons/switches, 12 V-tol, pull-ups), 4× AIN 0–5 V 12-bit (pots: bias, map select), 4× low-side 500 mA (LEDs, small relays), **NeoPixel out** (shift-light strips) | ESP32-C3 (TWAI on-die + WiFi) | JST + solder pads, printed sleeve | **$89** (board-only $69) | PT Nano $99: 2× the DIN, WiFi config, emulation modes, RGB out |
| 2 | **RDM IO Core** | 8× AIN 0–5 V (12 V-tol, **software** pull-ups), 8× DIN 0–20 kHz freq/duty, 8× low-side **1 A** PWM | ESP32-S3 | PCB spring terminals, printed case, USB-C | **$219** | Mini2 $200: software pull-ups (no soldering), 2× output current, WiFi + cross-platform suite vs Windows COM app |
| 3 | **RDM IO Pro** | Per ADR 0009: 4× precision 16-bit ΔΣ + 4× standard 12-bit AIN, 4× K-TC, 4× dig/freq (2 VR), 4× LS 2 A PWM, 2× HS 8–10 A | ESP32-S3 | Terminals **$449** / DTM+IP **$579** | — | Nothing under US$500 has TC + power + open CAN |

All three tiers share:

- **One firmware family** (ESP-IDF, TWAI): the same CAN profile layer —
  RDM-native, DBC export, and the emulation modes from ADR 0009 D3 (Haltech
  IO12A/B, MoTeC E888, ECUMaster SwitchBoard v3). *"Your $89 button box
  speaks Haltech"* is the Pico's whole marketing campaign.
- **One config UX**: the RDM Studio IO workspace (board render + pin
  inspector + live sim, already built for Pro) gains a model picker; pin
  groups are already data-driven so each tier is a board variant, not a new
  workspace.
- **Same OTA story**: un-brickable dual-slot updates via dash gateway or
  Studio, per the platform guardrails.

### Build order

1. **Phase 3a — Pico first.** Smallest hardware risk (~US$9–10 BOM, 2-layer
   board, no case tooling), and it forces the shared firmware layer
   (profiles, OTA, Studio workspace against real hardware) to exist before
   the expensive board does. DIY crowd = cheapest marketing; funnel into the
   suite.
2. **Phase 3b — Pro.** The gap product and the margin engine; carries the
   ADR 0009 spec unchanged.
3. **Phase 3c — Core last.** The most crowded segment; enter it once the
   suite + brand exist so it wins on ecosystem, not price. Re-check Mini2's
   price/spec before committing the final BOM.

### Pico vs the keypad line

Not competitors: the keypad (§4 row 2b) is the finished panel for people who
want buttons that look factory; the Pico is the harness-level part for people
who build their own wheels/panels. PT effectively sells both, so does Blink.
Pico's NeoPixel pad deliberately serves the DIY steering-wheel use case the
keypad can't.

## Consequences

- Platform plan §4 row 4 becomes three rows (amended 2026-07-21).
- Firmware repo gets a `profiles/` CAN emulation layer as its own module with
  golden-frame tests against logged Haltech/MoTeC traffic — it now ships on
  three products, so it must be table-stakes solid.
- Studio IO workspace needs a board/model picker before Pico ships (tracked
  as desktop work; the current workspace renders the Pro board).
- Margin sanity: Pico ~6–7× BOM at $89, Core ~5–6× at $219, Pro ~4–5× at
  $449/579 — all within market norms (40–60%+ after channel).
- Risk accepted: Pico cannibalizes some Core sales; acceptable because Pico's
  job is funnel volume, not margin.
