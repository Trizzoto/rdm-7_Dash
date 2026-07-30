# RDM Platform Plan — from dash designer to device suite

**Date:** 2026-07-10
**Status:** Proposal — for Tommy's review. Section 7's "Phase 0" rename already
landed same-day (see note below); Phase 2's GPS/lap-timing software has since
progressed well past what this document describes — for the current state and
forward plan there, see `rdm7-desktop/docs/LAP_ANALYSIS_REDESIGN_2026-07.md`
and this repo's `docs/STUDIO_ANALYSIS_PLAN_2026-07.md`. The Studio shell/nav
ideas in §5.3 were superseded by `docs/STUDIO_SHELL_PLAN_2026-07.md`
(2026-07-27) — read that first if you're touching navigation or workspace
placement. Keypad (§6.1) and IO (§6.3) remain unstarted proposals as written.
**Research inputs:** `docs/research/2026-07-keypad-market.md`, `docs/research/2026-07-gps-laptimer-market.md`, `docs/research/2026-07-io-expander-and-suite-ux.md`

---

## 1. Executive summary

RDM is expanding from one product (RDM-7 dash) to four: the dash, **Blink Marine CAN keypad support** (later an RDM-badged keypad), a **GPS lap timer**, and a **CAN IO expander**. The desktop program — "RDM-7 Visual Designer" v0.2.5 at the time of writing, **renamed "RDM Studio" the same day** (bundle identifier kept as `com.rdm7.designer` so self-update carries over) — must grow from a dash editor into the configuration suite for all of them.

Three decisions fall out of the market research, and everything else follows from them:

1. **One app, device-modular** — the Haltech NSP / AiM RS3 pattern, not MoTeC's split tools. Free, cross-platform, no licenses, ever. That sentence alone differentiates against MoTeC ($100 license transfers), AiM (Windows-only), and Holley (lockstep versions).
2. **The dash is the gateway.** Every competitor needs a USB-CAN dongle (ECUMaster) or vendor cable to configure bus devices. RDM-7 already has WiFi + a CAN transceiver + general-purpose TX (`can_transmit_frame`/`_ext`). The desktop app talks WiFi to the dash; the dash speaks CANopen/J1939 to keypads, GPS, and IO on the bus. **Zero extra hardware to configure anything** — nobody else can say that.
3. **Channels are the spine.** The channel registry (ADR-0005/0006) already decouples "a named quantity on this car" from where it comes from. A keypad button, an IO pin, and GPS lap delta are just channels with new sources (`signal_source_t` gains KEYPAD/GPS/IO). Widgets, rules, night mode, logging, and LED feedback all bind to channels — no new binding concept is needed anywhere in the system.

**Pricing headline enabled by this plan:** a complete lap-timing dash (RDM-7 $599 + RDM GPS ~$249) lands under A$900 vs ~A$3,000 for ECUMaster ADU5 + GPS-to-CAN, and a full "dash + keypad + IO" car is ~A$1,800 vs A$4,500+ from any established brand.

---

## 2. Where we are (assets to build on)

| Asset | State | Role in this plan |
|---|---|---|
| RDM-7 dash firmware 1.1.24 | Shipped; schema v17 | Becomes the bus gateway + lap-timing engine host |
| Channel system (`main/data/`) | Live; `channels.json` device-local | Cross-device data model; gains new sources |
| CAN manager | General TX (11/29-bit), promiscuous mode, bitrate switching | CANopen/J1939 shim builds directly on it |
| Desktop app v0.2.5 | Editor parity via ADR-0007 overlay, WiFi auto-connect, WASM preview, local dash, layout transfer, self-update pipeline | Becomes the suite shell |
| Web editor (embedded) | Single source of truth, phone-capable | Keypad/IO config should land here too (phone-configurable devices) |
| Marketplace (Supabase) | Layouts + assets | Grows to carry car projects, track DB, keypad templates |
| OTA + release infra | GitHub releases, signed updater | Extends to fleet firmware manager |

---

## 3. What the research says (condensed)

Full reports in `docs/research/`. The load-bearing findings:

**Keypads** — Every ECU vendor's keypad is a rebadged Blink Marine or Grayhill at 40–110% markup (Blink PKP-2400 $280 → Link sells the same unit for up to $736). Blink ships **no consumer PC tool**; config happens over CAN via the ECU vendor's suite. The #1 forum failure mode is the **baud/node-ID/protocol dance** (MaxxECU's official procedure: restart the ECU after every baud change). The most-loved feature is Haltech's **LED-follows-actual-output-state**; Link users begged for it for years. Nobody offers a WYSIWYG keypad preview.

**GPS lap timers** — 25 Hz multi-constellation + IMU is table stakes (RaceBox Mini: $219 retail proves the BOM). The gap: RaceBox has no dash; AiM/ECUMaster want $1.4–2.2k for the dash before a $300–480 GPS module. Analysis software is universally **Windows-only, clunky, or license-locked** (MoTeC i2 Pro: machine-locked, $100 transfer fee). ECUMaster's GPS module needs a **separate USB-CAN dongle just to configure it**. Feature baseline on-dash: auto track detection, GPS start/finish (no beacon), best/last/**predictive delta**, sectors.

**IO expanders** — Nothing under US$500 combines analog + thermocouple + usable power outputs + **open configurable CAN**. Haltech IO12 ($370): no TC, 1 A outputs, hard-coded IDs, Haltech-only. AEM's 22-ch ($361): locked to their own dashes — the fatal mistake. MoTeC E888 + TC8 to get TC coverage: ~$1,574. Link's forum has users literally requesting this product.

**Suite UX** — Single-app suites win sentiment (NSP "a dream", RS3 "clunky but one app") vs MoTeC's tool sprawl + licensing resentment. ECUMaster Light Client is the closest prior art to dash-gateway: bus discovery, per-device property editing, one-click bus-wide bitrate change. Pain to avoid: Holley's lockstep version requirements, AiM's config version lock-in, NSP's updater loops.

---

## 4. Product line & positioning

| # | Product | What it is | Target price | Sell when |
|---|---|---|---|---|
| 1 | **RDM-7 Dash** | Existing | A$599 (beta $299) | Now |
| 2 | **Keypad support** | Software: configure any Blink PKP (+ vendor rebrands) from the dash/desktop/phone | Free feature; sell Blink units as bundle | Phase 1 |
| 2b | **RDM Keypad** | RDM-badged Blink rebrand (8 + 12 button) | A$449 / A$549 (40–60% margin per market norms) | After Phase 1 proves demand |
| 3 | **RDM GPS** | 25 Hz u-blox M9/M10 + IMU CAN puck; lap engine runs **on the dash** | A$199–249 | Phase 2 |
| 4a | **RDM IO Pico** | DIY button/switch box: 8× DIN + 4× AIN 12-bit + 4× LS 500 mA + NeoPixel out; ESP32-C3, WiFi config, CAN emulation modes | A$89 (board-only $69) | Phase 3a *(ADR 0010)* |
| 4b | **RDM IO Core** | Mini2-class: 8× AIN (software pull-ups) + 8× DIN 20 kHz freq/duty + 8× LS 1 A PWM; terminals + printed case | A$219 | Phase 3c *(ADR 0010)* |
| 4c | **RDM IO Pro** | The gap box: 4× precision 16-bit ΔΣ + 4× std 12-bit AIN, 4× K-TC, 4× dig/freq (2 VR), 4× LS 2 A PWM, 2× HS 8–10 A; open CAN, DBC export + emulation | A$449 terminals / A$579 DTM *(ADRs 0009/0010)* | Phase 3b |

Positioning line: **"One car. One app. No licenses."** Every device configures through the same free suite, over WiFi, through the dash — from a laptop *or a phone*. The IO expander and GPS module also speak plain DBC-documented CAN so they sell into MoTeC/Link/Haltech cars as a wedge (AEM's lock-in mistake, inverted).

Guardrails from research (reputation is the product at this tier):
- Publish real specs (25 Hz, constellations, ±0.02 s) and **never silently downgrade** (VBOX's 20→10 Hz cut is still cited years later).
- Firmware updates must be un-brickable (AiM Solo 2 horror stories) — our dual-OTA + rollback already does this; extend the guarantee to every new device.
- No version lockstep: suite must speak N and N−1 device firmware, with explicit compat checks in the fleet updater.

---

## 5. The unifying architecture

### 5.1 Dash as gateway

```
Desktop/Phone ── WiFi ──> RDM-7 Dash ── CAN bus ──> Keypad (CANopen/J1939)
                                   │                 RDM GPS (RDM CAN protocol)
                                   │                 RDM IO  (RDM CAN protocol)
                                   └──> ECU (existing: presets/OBD2)
```

New firmware surface (all built on the existing CAN manager):
- **`bus_manager`** (new, `main/can/`): CANopen master shim — NMT start, heartbeat/bootup sniffing (0x700+id), SDO read/write (0x600/0x580+id), PDO mapping; J1939 address-claim listener. Small: the dash is a *configuration* master, not a full CANopen stack.
- **Device discovery**: passive sniff (heartbeats, address claims, known RDM frames) + active probe. Result: a live inventory of bus devices with type/protocol/baud/node-id.
- **Proxy API**: `GET /api/bus/devices`, `POST /api/bus/sdo`, `POST /api/bus/adopt` (guided re-baud/re-address), `POST /api/bus/nmt`. The desktop/web UI never speaks raw CAN — it calls these.
- Budget note: 25 Hz GPS + keypad PDOs are trivial next to existing 200 Hz logging paths. The lap-timing engine (Section 6.2) is arithmetic on one position stream — fine on the LVGL task at 25 Hz.

### 5.2 Channels as the spine

- `signal_source_t` gains `SIGNAL_SOURCE_KEYPAD`, `SIGNAL_SOURCE_GPS`, `SIGNAL_SOURCE_IO` (provenance only, as today).
- Keypad buttons → channels (`kp1_btn3`, or user-named `pit_limiter_sw`) with momentary/toggle/n-state semantics resolved *before* the channel (the channel just carries the state value).
- GPS/lap engine publishes canonical channels: `gps_speed`, `lat_g`, `lon_g`, `lap_time`, `lap_delta`, `lap_number`, `sector`, `best_lap`, …
- IO pins → channels via the existing pin→function→calibration model (channel math + calibration curves already exist).
- Consequence: **every existing widget, rule, night-mode override, and logger works with every new device on day one.** A bar bound to `lap_delta` is a predictive-delta bar. A warning widget bound to `kp1_btn3` shows pit-limiter state. This is the moat — competitors bolt features together; RDM composes.

### 5.3 Suite shell (desktop + web)

The desktop app is **RDM Studio** (renamed 2026-07-10, the day this plan was written; "RDM-7 Visual Designer" was dash-specific):

- **Device tree sidebar — "This car"**: the dash (WiFi) plus everything it sees on the bus. Sources: WiFi discovery (exists) + `GET /api/bus/devices` (new). ECUMaster Light Client is the reference, done over WiFi instead of a dongle.
- **Workspaces** (tabs/views per device type):
  - *Dash* — the existing editor, unchanged (ADR-0007 discipline: stays firmware-derived).
  - *Keypad* — WYSIWYG grid, per-button config, live LED preview (Section 6.1).
  - *Lap Timing* — track DB, start/finish editor, race-page setup, session analysis (Section 6.2).
  - *IO* — pin→function→calibration→broadcast wizard (Section 6.3).
- **Placement rule — superseded 2026-07-27, corrected here 2026-07-30:** this
  originally said device-config workspaces (keypad, IO, lap setup) belong in
  the firmware web editor first, attributing that constraint to ADR-0007.
  **ADR-0007 never said this** — it is 92 lines about one problem (three
  copies of the dash layout editor HTML drifting), and never mentions
  workspace placement. See `docs/STUDIO_SHELL_PLAN_2026-07.md` §2.0 for the
  full account. The rule is retired. Current policy: **the device serves an
  API; every configurator and workspace is authored in its client** — Studio
  now, a mobile app later — built to be the best version of itself, with no
  mirroring obligation. The device's embedded editor becomes a frozen
  limp-home page (bug fixes only), not the canonical UI for anything new.
  Desktop-only, unaffected by this either way: session analysis (heavy
  compute/plots), fleet firmware manager, car-project management UI.
- **Car project file (`.rdmcar`)**: bundles dash layout(s) + channel registry + keypad map + IO config + lap settings + track overrides. Offline-editable against the local dash (v0.2.5 pattern, extended), synced on connect, shareable on the marketplace ("full car setups", not just layouts). The existing `.rdm` layout bundle remains for layout-only sharing.
- **Fleet firmware manager**: one screen listing every RDM device + version + update badge (RS3's Connected Devices pattern), updating through the dash gateway, with an explicit compat matrix (suite vs device firmware N/N−1) to dodge Holley-style lockstep pain.
- **CAN hygiene at project level**: auto-assigned IDs with conflict validation across all devices in the project; **DBC import (exists conceptually via presets) + DBC export** for every RDM device so they drop into non-RDM ecosystems.

### 5.4 What does NOT change

- ADR-0007 stays: firmware `index.html` is the source of truth; desktop = overlay. New workspaces authored in the firmware editor ride the same pipeline.
- Layout JSON format, schema versioning, marketplace compat — untouched by keypad/IO/GPS configs (they live in their own stores, like `channels.json` today).
- The 32 KB layout budget is unaffected; device configs are separate files.

---

## 6. Per-product plans

### 6.1 Keypad configurator (Phase 1 — software only, highest leverage)

The wedge feature. Blink hardware exists, customers own it, and every competitor's config UX is documented pain.

**Firmware:**
1. CANopen shim in `bus_manager` (NMT start, heartbeat sniff, SDO, TPDO1 parse at 0x180+id; RPDO LED writes at 0x200/0x300+id). J1939 mode second (PGN 61184 + address claim).
2. **Adoption wizard backend**: detect keypad at any baud (temporarily re-baud the controller in a guarded window — bus must be idle/bench; wizard enforces), read model via SDO, rewrite baud (0x2010) / node-id (0x2013), verify, restore. This kills the MaxxECU multi-restart dance.
3. Keypad state → channels; LED feedback engine: each key's LED bound to a channel with per-state color (9 Blink colors), solid/slow/fast blink, fault-red — implemented as a consumer of the existing rules engine.
4. Brightness: global 0–63 + backlight color, **day/night pairs driven by the existing night_mode**.
5. Persistence: `keypads.json` on LittleFS beside `channels.json`, same atomic-write idiom.

**UI (firmware web editor → desktop via sync):**
- WYSIWYG keypad: render the exact grid (2×4 … 3×5), icon insert picker (Blink's catalog), simulated presses, **live LED state streamed from the dash**.
- Per-button: mode (momentary / toggle / 2–6-state / paired up/down), short/long-press thresholds (AiM's differentiator), target channel name, LED binding.
- Presets for vendor-programmed units: Haltech node 0x0C, MoTeC 0x0A, Blink/Link/AiM/ECUMaster 0x15 at 1M/500k/125k — "reuse the keypad you already own" is a marketing line.
- Function templates: pit limiter, launch arm, fan override, pump, map switch, logging marker, **dash page next/prev, night mode toggle** (dash-native functions no competitor keypad can do).
- Keypad → CAN re-broadcast profiles so button states reach ECUs (MaxxECU/Haltech/Link consume keypad-as-CAN-DI patterns) — keypad is useful with no PDM.

**Sell:** "RDM-7 + Blink PKP-2400 bundle" immediately; RDM-badged units once volume justifies (Blink rebrand for the quality story — 40–110% markup is the market norm; skip Alibaba $70 units, they'd undercut the Haltech-peer positioning).

### 6.2 GPS lap timer (Phase 2 — first RDM-designed hardware)

**Hardware (RDM GPS):** ESP32-C3/S3 + u-blox NEO-M9N or MAX-M10S (25 Hz, 4 constellations) + 6-axis IMU + CAN transceiver, IP67 roof puck, magnetic base, single 4-pin cable (12 V + CAN). BOM well under A$80 at the RaceBox price proof-point; retail A$199–249. Broadcasts an **open, DBC-published** protocol; OTA via dash gateway.

**Lap engine — runs on the dash** (the GPS stays dumb; engine survives GPS-vendor swaps):
- Auto track detection against on-device track DB; GPS start/finish line-crossing detection (interpolated between 25 Hz fixes → ±0.02 s); sectors.
- Best/last/theoretical lap; **predictive delta** vs auto-updated best reference (ECUMaster "Qualification Mode" pattern).
- Publishes everything as channels (5.2). New widgets: predictive delta bar (signed, center-zero), lap list panel, sector indicator; a bundled "Race Page" layout in the standard set.
- Session files to SD (existing data_logger) with lap/sector index.

**Track DB:** seed ~100 self-curated circuits (AU/NZ complete + major US/EU/JP), community submissions via marketplace with review (RaceChrono model). Own coordinates only — AiM's DB is proprietary; wholesale OSM import drags ODbL share-alike.

**Desktop analysis (RDM Studio workspace, desktop-only):** the anti-i2 — free, modern, cross-platform: session browser (auto-download over WiFi), delta-T plot vs reference, GPS track map with sector coloring, two-lap overlay, CSV export. Video overlay later; explicitly out of v1.

### 6.3 CAN IO expander (Phase 3 — RDM IO)

**Spec (the documented gap):** 8× analog 0–5 V 12-bit • 4× K-type thermocouple • 4× digital/frequency (2 VR-capable) • 4× low-side 2 A PWM • 2× high-side 8–10 A. Configurable 11/29-bit CAN, 125k–1M. IP-rated enclosure, automotive connector (DTM-style). ESP32 + TWAI inside → same OTA/gateway story. Retail A$549–649.

> **Amended 2026-07-21** — now a three-tier ladder (see §4 rows 4a–4c and
> `docs/adr/0010-rdm-io-three-tier-ladder.md`): **Pico** A$89 DIY button box
> (Phase 3a, first), **Pro** A$449/579 carrying the spec above with the
> mixed-precision analog front-end of `docs/adr/0009-rdm-io-mixed-precision-frontend.md`
> (Phase 3b), **Core** A$219 Mini2-class (Phase 3c, last). All tiers share one
> ESP32 firmware family with CAN emulation modes (Haltech IO12, MoTeC E888,
> ECUMaster SwitchBoard) + DBC export, and configure through the same Studio
> IO workspace.

**Config:** ECUMaster-style **pin → function → calibration → broadcast** wizard (firmware web editor first, desktop inherits). Input pins publish channels (calibration curves + channel math exist); outputs bind to channels/rules — closing the loop: *keypad button → channel → IO output*, a soft PDM for fans/pumps/lights without PDM wiring or price.

**Interop as strategy:** RDM-native protocol *and* DBC export + generic broadcast mode so it sells into MoTeC/Link/Haltech cars standalone. It fixes exactly what forums complain about: Haltech's hard-coded IDs/2-box cap, AEM's dash lock-in, E888's price.

### 6.4 Explicit non-goals (for now)

- **No PDM/PMU product** — different safety class (protected high-side switching, digital fusing, load shedding); revisit after RDM IO ships.
- **No full CANopen master stack** — only what keypads need.
- **No video** in analysis v1. **No RTK** GPS in v1.
- **No paid software tiers** — free suite is load-bearing for positioning.

---

## 7. Phased roadmap

**Phase 0 — Suite foundations** (with current momentum, ~now)
- **Done 2026-07-10.** ~~Rename plan~~: "RDM Studio" (binary/updater identity preserved — bundle id `com.rdm7.designer` unchanged — so self-update carried over).
- Device-tree sidebar: WiFi devices + `GET /api/bus/devices` stub.
- `.rdmcar` project format v1 (dash layout + channels; slots for future device configs).
- Firmware: `bus_manager` skeleton — passive sniff inventory (heartbeats/address claims), proxy endpoints.
- *Exit:* connect to dash → see "this car" tree with dash + any heartbeating CANopen device listed.

**Phase 1 — Keypad support** (first sellable expansion; software-only)
- CANopen shim + adoption wizard + LED/channel engine + `keypads.json` (firmware).
- Keypad workspace in web editor → desktop sync. Bench-verify with a real PKP-2400 before building UI polish (order one early).
- J1939 mode + vendor-preset adoption after CANopen path proven.
- *Exit:* bundle on sale; a customer configures a keypad from a phone in <10 min with zero extra hardware.

**Phase 2 — GPS lap timer**
- RDM GPS hardware (proto → smallrun), open CAN protocol + DBC.
- Dash lap engine + channels + race widgets + bundled Race Page; track DB seed.
- Desktop analysis workspace v1 (session browser, delta-T, track map, overlay, CSV).
- *Exit:* sub-A$900 lap-timing dash live-demoed; beta cars running it (Adelaide FB beta cohort is the obvious testbed).

**Phase 3 — RDM IO**
- Hardware proto (spec §6.3), RDM protocol + DBC export.
- Pin wizard in web editor; outputs bound to channels/rules (keypad→IO soft-PDM loop).
- *Exit:* dash+keypad+GPS+IO full-car demo car; marketplace carries `.rdmcar` full setups.

Dependencies: Phase 1 ships value alone; Phase 2/3 hardware lead times overlap software work (order GPS modules/IO protos during Phase 1). Nothing blocks on the P4 port; the gateway/channel work is portable to it later.

---

## 8. Risks & open decisions

| Risk / decision | Note | Recommendation |
|---|---|---|
| Adoption wizard re-bauds a live bus | Wrong-baud TX can error-flood a running car bus | Wizard requires bench/ignition-off mode; guarded window + auto-restore (design in from day 1) |
| One TWAI controller on the dash | Gateway + ECU traffic + keypad PDOs share one bus | Fine electrically (keypads/GPS are designed for shared vehicle buses); keep RDM device broadcast rates configurable |
| ESP32 CPU budget | Lap engine + CANopen shim on top of UI | Trivial vs existing 200 Hz logging; heap_monitor exists if needed |
| RDM-badged keypad inventory risk | Rebrand margin is proven, but stock costs cash | Bundles first (zero inventory risk), badge after demand data |
| Track DB licensing | AiM proprietary, OSM ODbL | Own curation + community submissions only |
| Suite rename timing | **Done 2026-07-10** — "RDM-7 Visual Designer" → "RDM Studio" | Renamed same day this plan was written; updater identity/keys (`com.rdm7.designer`) kept unchanged as intended |
| Desktop repo CLAUDE.md says "MaxxECU systems" | **Fixed** — stale positioning line | Confirmed gone from `rdm7-desktop/CLAUDE.md` as of 2026-07-30 |
| P4 port | Round + JC1060P470 variants pending | Gateway/channel work is display-agnostic by design; port after S3 ships each phase |

---

## 9. Why this wins (one paragraph)

Every competitor forces a choice: cheap-but-closed (Haltech boxes locked to Haltech, AEM to AEM), open-but-expensive (MoTeC + licenses), or good-hardware-clunky-software (AiM). RDM's existing architecture — channels as a car-wide data model, a WiFi dash that can master the CAN bus, one codebase driving web/desktop/device UI — lets it offer the combination nobody has: **open protocols, phone-to-desktop configuration with zero extra hardware, composable devices, at half the price, with free modern software.** The research says users are already asking for each piece on competitors' forums; this plan just assembles them in the order that turns each phase into revenue.
