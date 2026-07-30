# ADR 0009 — RDM IO: mixed-precision analog front-end and the budget-box benchmark

Status: Accepted (2026-07-21) — spec decision for the Phase 3 hardware; sim in
RDM Studio updated to match. Supersedes the flat "8× analog 12-bit" line of
`PLATFORM_PLAN_2026-07.md` §6.3 (plan doc amended in the same commit — see the
§6.3 blockquote there).

## Context

The July research round (`docs/research/2026-07-io-expander-and-suite-ux.md`)
surveyed MoTeC/Haltech/ECUMaster/AEM but missed **PT Motorsport (AU)**, whose
IO expander line has since been benchmarked (2026-07-21):

| Model | Price (AUD) | Inputs | Outputs | Notes |
|---|---|---|---|---|
| IO Nano | $99 | 4× AV (0–5 V only), 4× DIN on/off | 4× 200 mA LS PWM (490/980 Hz) | JST, board-only option |
| IO Mini | $200 | 4× AV (12 V-tol, 1k solder pull-ups), 4× DIN (1× freq) | 4× 200 mA LS | JST |
| **IO Mini2** | **$200** | **8× AV, 8× DIN (0–60 kHz freq + duty)** | **8× 500 mA LS PWM 0–60 kHz** | PCB terminals, 3D-printed case |
| IO DTM | $299 | 4× AV, 4× DIN (1× freq) | 4× 3 A (7 A total) | Deutsch DTM |

The Mini2 is the interesting one. Public firmware
([github.com/ptmotorsport/PT-IO-Mini2](https://github.com/ptmotorsport/PT-IO-Mini2))
shows it is an **Arduino UNO R4 Minima core** — Renesas RA4M1, Arduino
framework, ArduinoJson, PlatformIO. Estimated BOM US$18–25: one 48-pin MCU
using its internal ADC, a CAN transceiver, eight small low-side FETs, PCB
terminal blocks, printed case. No thermocouples, no high-side or high-current
outputs, no VR inputs, Windows-only serial config app.

Its one genuinely clever feature: **CAN emulation modes** — it impersonates
Haltech IO12A/B and IO16A/B, ECUMaster SwitchBoard v3, MoTeC E888 and Emtron
frames, so every major aftermarket ECU accepts it with zero custom config.
That, not the price, is why it sells.

## Decisions

### D1 — Mixed-precision analog front-end (4 + 4), not 8× uniform

AIN1–4 become **precision** channels: external 16-bit ΔΣ ADC (ADS1115-class,
one 4-ch part), buffered/protected 0–5 V front-end, **software-switchable 1 kΩ
pull-up** (analog switch, not solder pads — one better than PT and equal to
Haltech/ECUMaster). AIN5–8 become **standard** channels on the ESP32-S3's
internal 12-bit SAR ADC with eFuse calibration, divider + clamp front-end,
12 V-tolerant-clipped like everyone else.

Why: pressure sensors, widebands and thermistor bridges deserve real bits;
pots, switch ladders and float senders do not. Halving the precision channel
count cuts one ADC package, four buffer/protection networks and the second
precision rail (≈US$5–7 BOM) while the headline — *"16-bit where it counts"* —
still beats every box in the table (Haltech/ECUMaster: 10-bit, Mini2: RA4M1
internal, E888: 10-bit). Split-role inputs are market-normal (Haltech
AVI/DPI, AEM AV/temp/configurable, E888 AV/TC).

### D2 — Do not chase the AU$200 floor; keep the gap spec

The Mini2/SwitchBoard class (signal-level IO, ≤500 mA, no TC) is saturated at
AU$99–240 with sub-US$25 BOMs. RDM IO stays the box that class can't be:
4× K-type TC, 2× VR-capable digital, 4× 2 A low-side PWM + 2× 8–10 A
high-side, open CAN + DBC export, WiFi + RDM Studio (cross-platform, not a
Windows COM-port app). That combination still has **no competitor under
US$500** (E888+TC8 ≈ US$1,574).

### D3 — Adopt CAN emulation modes (PT's playbook, our firmware)

RDM IO firmware ships with impersonation profiles: **Haltech IO12A/B, MoTeC
E888, ECUMaster SwitchBoard v3** (Emtron later), selectable per device in RDM
Studio next to the RDM-native protocol and DBC export. Zero BOM cost; it makes
the box droppable into any existing loom the way the Mini2 is, and it's the
acquisition wedge into non-RDM cars. TC channels map into the E888 profile's
TC slots — something the Mini2 cannot emulate because it has no TC hardware.

### D4 — Two-tier connector/enclosure ladder (launch both)

PT charges $99→$299 for Nano→DTM with *less* IO than us at every rung.
RDM IO launches as: **base** — PCB spring terminals + printed vented case,
**A$449**; **DTM** — Deutsch DTM signals + DTP (or doubled DTM pins) for the
two high-side outputs, IP-rated case, **A$579**. Same PCB, same firmware;
the plan's single A$549–649 SKU is replaced by this pair. The base tier now
answers the Mini2 buyer's "why 2× the price?" with TC + power + 16-bit + suite
instead of asking them to fund a Deutsch harness they may not want.

### D5 — Sim reflects the split now

The RDM Studio IO Expander workspace (desktop overlay) marks AIN1–4 with a
gold ◆ and reports channel class (Precision 16-bit ΔΣ / Standard 12-bit) in
the pin inspector, so the software story is ahead of the hardware and the
spec is user-visible from day one.

## Consequences

- BOM lands ≈US$45–60 with TC and power outputs — healthy at A$449/579.
- Firmware grows a CAN profile layer (emulation table per mode) — pure
  software, testable against logged Haltech/MoTeC traffic.
- Datasheet must be honest about the split (per guardrail: publish real specs):
  AIN1–4 16-bit/0.1 mV-class, AIN5–8 12-bit/±ε after calibration.
- `PLATFORM_PLAN_2026-07.md` §6.3 and the research doc A1 table amended to
  carry the PT Motorsport line so the next pricing pass starts from reality.
