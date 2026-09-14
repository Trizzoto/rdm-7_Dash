# ADR-0074: A car that answers OBD2 is told so — and a new ECU takes the old one's decodes with it

Date: 2026-09-14
Status: Accepted — flashed to the bench dash `RDM-A2EC-C854` over USB and walked
on the glass with the OBD2 simulator standing in for a car; Studio side verified
against the dev server
Repos: RDM-7_Dash (`main/data/obd2_autosetup.{c,h}`,
`main/ui/screens/first_run_wizard.{c,h}`, `main/ui/screens/ui_Screen3.c`,
`main/ui/settings/device_settings.c`, `main/can/obd2.c`,
`main/layout/ecu_presets.{c,h}`, `main/layout/layout_manager.{c,h}`,
`main/net/web_server_obd2.c`, `main/net/serial_commands{,_capture}.c`,
`main/storage/config_store.{c,h}`, `main/web/index.html`,
`tools/mobile-dev-server.js`), rdm7-desktop (`src/firmware-base.html`)
Follows ADR-0073 (OBD2 is set up for the car, not asked of everyone).

## Context

ADR-0073 took OBD2 out of the wizard's default path: a Falcon preset sets it up
by itself, and a car with no preset picks "My car uses OBD2". The owner's next
question: for any other car, *"it'd be good to know — hey, there's OBD2
available — do you want to use it, and just the list of what channels can be
populated by OBD that currently aren't."*

Answering it on the real dash (no network, so over USB) turned up five more
things, two of them serious:

| Seen on the glass | Cause |
|---|---|
| **After Haltech → Ford Falcon FG, 30 Haltech decodes were still live, and survived a reboot** (THROTTLE and MAP on 0x360, IGNITION 0x362, OIL_PRESSURE 0x361…); the Falcon's Intake Air Temp read "CAN 0x3E0" | Applying a preset upserts its signals into the registry and the layout's `signals[]` and never removes any. Anything later bound by name landed on the old ECU's bit layout — and a slot with a frame id counts as CAN-owned, so OBD2 never polled it |
| **The wizard forgot the car's ECU at every boot** | It saved the ECU only to NVS; every layout load copies the layout's `ecu` field over NVS, and the wizard never wrote that field |
| An OBD2 reading's source said "CAN 0x0" | OBD2 registration skips a name that already exists, so an idle CAN-tagged slot kept its CAN tag while OBD2 values flowed into it |
| The dashboard's red "NO CAN BUS" badge sat on top of the wizard's CAN step, over its help text | The badge foregrounds itself on every show, on the same screen the wizard overlays |
| The ECU picker's rows were 336 px wide in a 760 px sheet, wrapping names; the OBD2 scan sheet listed "already set up" rows first and pushed the addable ones below the fold, and counted 67 "readings" where the ECU step counted 61 | Row width borrowed from the channels list; resolver order; bitmask PIDs counted |

## Decision

**Every other preset gets a CHECK.** After any non-Falcon preset is applied as
the car's whole setup, `obd2_autosetup` runs the same discovery scan it runs
for a Falcon — a few tries this session, not persisted, so a race ECU that
doesn't speak OBD2 is left alone — but binds nothing. If the car answers and
some readings would fill channels nothing feeds, the answer is kept as an
**offer** (raw PIDs in NVS, resolved on read so it never lists a channel that
was set up some other way since).

**The offer is shown where the car's channels are reviewed:**

- Wizard channels step: the hero says "checking for OBD2…", then the OBD2
  button reads **"+ OBD2 can add 10"** and opens the scan sheet already filled
  with the car's answer — addable rows first, pre-ticked — no second scan.
- "No ECU detected": the card checks while it is up; when the car answers, the
  OBD2 button becomes the primary one — **"Use OBD2 — your car answers 61
  readings"** — and "Pick an ECU manually" steps back.
- Device Settings → OBD2 readings reads **N TO ADD** (or CHECKING, or WAITING
  FOR CAR for a Falcon still owed).
- Studio's Channels page: a banner above "On this car" — *Your car also answers
  OBD2. 9 channels nothing feeds yet could come from it: RPM, Throttle
  Position, Intake Air Temp, Engine Load and 5 more.* — with **Review and add…**
  (the scan dialog, pre-filled) and **Not now** (dismisses). `GET /api/obd2/offer`,
  `POST /api/obd2/offer {"dismiss":true}`. Adding through `/api/obd2/adopt`
  retires the offer.

**A new ECU retires the old one's decodes.** On a full re-setup
(`first_run_wizard_apply_ecu` with `replace`, and the "My car uses OBD2"
path), every signal the previous preset wrote — by name *and* frame id, so a
signal re-aimed by hand is left alone — is made idle in the registry and
removed from the layout's `signals[]` (`ecu_layout_writer_remove`) before the
new preset goes on. Studio's Quick ECU Setup was already safe: it replaces
`signals[]` wholesale.

**The car's ECU lives in the layout.** The same apply writes `ecu` /
`ecu_version` into the layout (`ecu_layout_writer_set_ecu`) and the layout
manager's copy (`layout_manager_set_ecu_context`), so the next load confirms
the choice instead of erasing it.

**Smaller fixes, each seen on the glass:** OBD2 registration claims an idle
CAN slot (no frame id) as OBD2; the NO CAN BUS badge stays hidden while the
wizard is up; the ECU picker's rows use the sheet's width and name presets by
their display name; the scan sheet lists addable rows first and counts
readings the way the ECU step does; Studio's scan dialog sorts the same way.

**The dash can be driven over USB.** New serial RPCs `touch`
(`{enabled?, x, y, state}`, the twin of `/api/touch`) and `obd2.sim`
(`{on?}`, the twin of `/api/obd2/sim`). A dash with no network could be
screenshotted over a cable but not tapped, so the screens only the touchscreen
reaches could not be tested — which is how every bug above had gone unseen.

## Consequences

- `obd2_autosetup_for_ecu()` now always does one of SETUP / CHECK. The offer is
  dropped by `obd2_autosetup_cancel()` — so skipping the ECU step, choosing
  another ECU, or adding readings by hand all retire it — and by "Not now".
- `GET /api/obd2/offer` is two more URI handlers (~160 of 192).
- **Bench, not car:** everything above ran against the firmware's OBD2
  simulator on a dash with no CAN traffic. A real car's discovery timing and a
  real Falcon's broadcast running alongside the check are still unobserved.
- **Not verified on glass:** the Device Settings card (reaching it meant
  finishing the first-run wizard on a shared bench dash).
- **Not changed:** Studio's USB transport still answers `/api/ecu/*` and
  `/api/can/*` with a fabricated `{ok:true}` (`transport.js`
  `_usbRouteApiCall`), the same class of lie `check_offline` removed for Offline.
  Quick ECU Setup over a cable silently does nothing. Worth its own change.
