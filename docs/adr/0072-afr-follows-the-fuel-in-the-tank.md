# ADR-0072: AFR follows the fuel in the tank

Date: 2026-09-14
Status: Accepted — native tests pass (`test_fuel_stoich` 14/14,
`test_channel_math_fold` 19/19, all 29 suites); web verified on the dev
server (E85 → λ 1.00 = 9.81 AFR in the drawer); firmware built and flashed to
a bench dash over USB, boot healthy. The HTTP endpoints were **not** exercised
on hardware — the bench dash had no WiFi network configured.
Repos: RDM-7_Dash (`main/data/`, `main/net/web_server_channels.c`,
`main/web/index.html`, `tools/mobile-dev-server.js`), rdm7-desktop
(`src/firmware-base.html`)
Follows ADR-0005/0006 (channels own decode), the display-unit contract.

## Context

A Link Generic Dash customer asked how to show AFR instead of lambda. The
answer — set the channel's "Display as" to AFR — is right on petrol. Two things
made it wrong elsewhere:

| Path | What it did | On E85 |
|---|---|---|
| Display as → AFR | `unit_convert` λ → AFR with a hardcoded **14.7** | λ 1.00 read **14.7**; the tune sheet says 9.8 |
| Calculate… `Lambda × 9.8` onto an AFR channel | `_fold` gave the result unit λ (number × channel keeps the channel's unit), then the output step converted λ → AFR **again** | read **144** |
| Calculate… `Lambda × 14.7` onto an AFR channel | same double conversion | read **216** |

## Decision

1. **The λ ↔ AFR ratio is the stoich of the fuel, set once per dash.** Choices:
   Petrol (14.7, the default, so an update changes nothing), E10, E85, E100,
   Methanol, and **Flex**, which reads the `ethanol_pct` channel (Link
   `ETHANOL`, Haltech `FUEL_COMP`) once a second and uses petrol until it has a
   non-stale reading. A 0.05 AFR hysteresis stops a noisy sensor re-scaling
   meters every tick.
2. **Blends mix by mass, not volume.** `w = ρe·V / (ρe·V + ρg·(1−V))`,
   `AFR = w·9.0 + (1−w)·14.7`. E85 comes out at 9.81 (volume would say 9.86);
   E10 14.10. The presets call the same function as Flex, so a flex car
   reading 85 % and a car set to E85 cannot disagree
   (`main/data/fuel_stoich_calc.h`, pure and host-tested).
3. **The ratio lives in `unit_convert.c` as a setter**, not a lookup of the
   fuel module — the converter stays dependency-free for the host tests, and
   every consumer (readouts, ranges, thresholds typed in display units) picks
   it up. The setter refuses anything outside 5–20 (a slipped decimal would
   silently mis-scale every AFR readout).
4. **Changing it notifies only the channels that convert λ ↔ AFR**
   (`channel_manager_notify_conversion_changed`), so meters and arcs re-derive
   their display scale; bars, panels and text convert per value already.
5. **It applies both ways.** A wideband that sends AFR, displayed as λ, uses the
   same ratio. The drawer's help says so.
6. **Calculate: scaling by a constant is a hand conversion.**
   `channel × number`, `number × channel` and `channel ÷ number` mark the
   result, and the output step does not unit-convert it. The trade, written
   down in `channel_math.c`: `MAP × 0.5` onto a psi channel now reads the raw
   half (90), not a converted one — someone scaling by a constant is assumed to
   be doing the arithmetic themselves. `channel + number` still converts.
7. **API speaks keys, not enum values.** `GET/POST /api/fuel/config`
   (`{"fuel":"e85"}`), and `/api/channels` carries `stoich` and `fuel` so the
   editor's JS mirror converts with the dash's number at no extra request.
   The setting is per dash (NVS `vehicle/fuel`), applies immediately rather
   than waiting for the drawer's Save, and is disabled offline.

## Consequences

- Nobody who never opens the Fuel setting sees a different number.
- The Fuel row appears only on a channel read or shown in λ or AFR.
- **Flex reads exactly one channel, `ethanol_pct`, and the UI owns telling
  the user that.** Rather than a "which channel" picker (one more setting to
  get wrong), every preset's ethanol signal maps onto `ethanol_pct` — Link
  `ETHANOL`, Haltech `FUEL_COMP`, MaxxECU `E85` (added after the fact: it had
  been falling through to a custom channel, leaving Flex silently on petrol).
  Under Flex the drawer states the source and live reading, or that
  Ethanol % isn't set up (button to it), or that it's waiting — and names a
  custom channel that looks like an ethanol reading Flex isn't using.
- The dash cannot read a flex sensor wired to it directly: those output a
  frequency and the dash's inputs don't measure one. The reading has to come
  over CAN or OBD2 (PID 0x52).
- Not covered: a car switching fuels without an ethanol sensor has to change
  the setting by hand; the dash's own on-screen settings have no Fuel control
  yet (web/desktop only).
