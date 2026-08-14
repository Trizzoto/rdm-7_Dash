# ADR-0039: Setup is grouped by the question you arrived with

Date: 2026-08-14
Status: Accepted
Repos: RDM-7_Dash (`main/web/index.html`,
`main/ui/settings/device_settings.c`), rdm7-desktop (resynced base)
Follows 0030–0037. Prompted by the owner once Channels got its own tab:
*"we don't really need channels in there anymore… go through each setting
now and optimise inside the settings to make them all more intuitive."*

## The audit

Setup had six headings and was sorting by which subsystem implemented a
thing, not by what the reader wanted:

| Heading | What was in it |
|---|---|
| Vehicle & channels | one card — a signpost to a page that now has a tab |
| Connectivity | WiFi (which opened Device Info) and OBD2 |
| Device | Device Info, Dimmer, Gear Calc, Fuel Sender |
| Logging & dashboards | Live Data, Dashboard Switcher, **Marketplace** |
| Files & storage | Open, **Load JSON**, Save, Screenshot, File Manager |
| Developer Options | one card |

Three specific faults:

1. **Two cards, one destination.** "WiFi & network" and "Device Info" both
   called `openDeviceInfo()`. There was no page anywhere that answered
   *what network is the dash on*, despite a card promising exactly that.
2. **Odometer was unfindable.** It lived inside the Gear Setup modal
   because the two share a speed signal — true, and an implementation
   detail. On the dash it was already a top-level card.
3. **The two surfaces had drifted.** Same features, different homes: the
   dash filed Trouble Codes under CONNECTIVITY and Live Data under DEVICE;
   the web had them under Connectivity and "Logging & dashboards".

## Decision

Group by the question, and use the same groups on both surfaces.

- **Your car** — ECU & CAN bus · OBD2 diagnostics · Gear calculation ·
  Fuel sender · Odometer
- **This dash** — Screen & night · Layout switching · Device & updates ·
  WiFi & network
- **Data** — Live data & recording
- **Files & sharing** — Open · Save · Marketplace · File manager · Screenshot
- **Developer options** — Developer options · Load layout JSON

Open and Save are adjacent now; the JSON paste box that used to separate
them is an expert path and moved to Developer options. Marketplace is
filed under sharing because that is what it is.

**Three cards became real pages.** `ECU & CAN bus` answers "is the dash
talking to my car?" — preset, bus speed and whether frames are arriving —
where before the preset was reachable only from the Channels context strip
and the bus speed only from the dash's own screen. `WiFi & network` is a
page rather than an alias. `Odometer` gets its own door and names the
reading feeding it, linking to where that is set.

## Outcome first, wiring second

The modals had the same problem one level down.

**Screen & night** (was "Brightness Dimmer") opened on a field called
*Signal Source* and asked for a threshold before saying what any of it was
for. It now leads with brightness — applied live, because you are looking
at the screen you are dragging — then *Dim at night: Never / When the car
tells it to*. Signal, threshold, momentary-vs-toggle and invert only appear
once you have asked for a trigger, and the last three sit under Advanced.
Screen brightness moved in here too: "how bright is my screen" and "when
does it dim" are one question, and they were two cards apart.

**Fuel sender** led with a paragraph about non-linear senders and a generic
add-a-point loop. Two buttons now do the case that covers nearly everyone —
*Tank is empty* / *Tank is full*, one press each against the live voltage —
writing into the same points array the multi-point curve editor below uses.
A faster road to the common answer, not a second mechanism.

**Card subtitles say what you get, not what it is.** "Calculate gear from
RPM + speed. Wheel + final drive + ratios." became "Work out which gear
you're in from RPM and speed, when the ECU doesn't send it."

**Stat lines say something.** "Configurable" was true of every card and
told you nothing; Screen & night now reads *Always full brightness* or
*Dims on ILLUMINATION*, and ECU & CAN bus reads *MaxxECU 1.3 · nothing
arriving*.

## The verdict counts CAN, not everything

The first cut of the bus verdict read `info.signals.fresh`, which counts
every live signal — including the dash's own internals (FPS, free heap,
uptime). On a bench with no CAN bus attached at all it reported nine fresh
and the card cheerfully said **"Frames are arriving"**. The question is
about the CAN bus, so only `source === 'can'` signals count. It now reads
"0 of 20 arriving" and names the three things that cause it: wrong bus
speed, swapped CAN-H/CAN-L, wrong preset.

## What the dash does not copy

The dash's card opens a bitrate dropdown and a live frame table, and does
**not** carry the ECU preset — on the glass, presets are bound per-channel
and by the setup wizard, deliberately (that predates this ADR). So its card
is named "CAN bus" rather than "ECU & CAN bus". Naming it after Studio's
would have been a lie, and reading the layout file on the settings-open
path to fix that is a 32 KB allocation for a label. "Files & sharing" has
no dash equivalent — there are no file dialogs on the glass — and Studio's
Developer options collapse into the simulator, which sits under
**Data & testing** there.

## Verification

Against the live dash through the `--device` proxy: five sections in order,
every stat line populated from the real device — `MaxxECU 1.3 · nothing
arriving`, `v1.3.0 · 2655 KB free`, `WIFI_2468 · 192.168.4.69`, `4.8 km`,
`Always full brightness` — and 0 console errors. Each new modal opened and
read correctly: the bus verdict amber with "0 of 20 arriving", the network
page "Connected to WIFI_2468 / -59 dBm · strong", the odometer at 4.8 km
naming VEHICLE_SPEED, and Screen & night with the wiring correctly hidden
because no trigger is set. The dash's own Settings checked on the glass
over remote touch: YOUR CAR / THIS DASH / DATA & TESTING, all cards
present.
