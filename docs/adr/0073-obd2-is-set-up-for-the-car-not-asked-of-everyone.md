# ADR-0073: OBD2 is set up for the car, not asked of everyone

Date: 2026-09-14
Status: Accepted — firmware builds; web verified against the dev server; not
yet run on a dash or a Falcon
Repos: RDM-7_Dash (`main/data/obd2_autosetup.{c,h}`,
`main/ui/screens/first_run_wizard.{c,h}`, `main/net/web_server_layout.c`,
`main/net/web_server_obd2.c`, `main/storage/config_store.{c,h}`, `main/main.c`,
`main/ui/settings/device_settings.c`, `main/web/index.html`,
`tools/mobile-dev-server.js`), rdm7-desktop (`src/firmware-base.html`)
Follows ADR-0037 (a scan is a source), ADR-0071 (which flagged the step order).

## Context

The setup wizard asked every customer about OBD2 as step 2 of 5, before it had
even looked for their ECU. The owner's summary: OBD2 was there mainly for Ford
Falcons.

That is the shape of it. A standalone ECU (Link, Haltech, MaxxECU…) broadcasts
everything a dash shows, and its owner gains nothing from an OBD2 screen. The
cars that need OBD2 are of two kinds:

| Car | What it broadcasts | What OBD2 adds | What it needs from us |
|---|---|---|---|
| Ford Falcon BA/BF, FG | RPM, speed, coolant, throttle, battery (FG: gear) | Ignition timing, lambda, fuel trims, oil/fuel pressure; on an FG also MAP and intake temp | Nothing to decide — the preset already says which car it is |
| A factory ECU the catalogue has no preset for | Nothing we can decode | Everything | One question: "does my car use OBD2?" |

The old step also carried the ADR-0071 oddity: its stated reason ("ECU detect
gets its accumulation window" during it) no longer held, because the ECU step
resets the ID tracker on entry.

## Decision

**The wizard is four steps: CAN scan, ECU, channels, connect.** OBD2 is not a
step.

**A Falcon preset sets OBD2 up by itself.** `obd2_autosetup_for_ecu()` runs
after an ECU is applied as the car's whole setup — from the wizard, from
Studio's Quick ECU Setup (`/api/ecu/set`), from Studio's import with
`replace`, and when the dash's default layout is reset. For a preset that wants
it (make `Ford`) it runs the discovery scan in the background, takes every
reading the car answers that fills a channel nothing else feeds, and binds it.
Any other ECU cancels a setup still owed from a previous car. It reuses the
existing scan (`obd2_discovery_start`), resolver (`channel_obd2_matches`) and
binder (`channel_apply_obd2`) — the module only decides when.

**A car that doesn't answer yet is still set up.** Presets get applied at a
desk with the ignition off. The setup is persisted as owed
(`obd2_auto/pending`), retried every 45 s up to six times per boot, and resumed
15 s after every boot until the car answers once. A dash on ignition power
therefore retries each drive without polling the bus for the whole of one.

**Bus safety comes from the scan, not from here.** Discovery tries the other
CAN bit rate only when the bus is silent at the current one; on a live Falcon
bus it never transmits at a rate the car isn't using.

**A car with no preset says "My car uses OBD2" at the ECU step** — a button on
the "No ECU detected" card, which is where that car lands, and the first row of
the ECU picker. That opens an OBD2 screen that scans on arrival (they chose
OBD2; pressing Scan too would be a second question with one answer), binds
what the car reports, and offers Scan again, Continue, and Back. It clears the
channels and stored ECU first, as skipping the ECU does.

**The owner's own choice wins.** Adding readings by hand from a scan list — the
dash's sheet or Studio's `/api/obd2/adopt` — cancels a setup still owed, so a
background attempt never binds what they left unticked. Merely opening a scan
and closing it does not.

**Where OBD2 lives now:** Device Settings → Your car → **OBD2 readings** opens
the scan directly and reads WAITING FOR CAR while a setup is owed. Studio's
Quick ECU Setup says a Falcon's OBD2 is being set up, and the Channels header
shows "· OBD2 waiting for the car" (`/api/ecu/current` → `obd2_autosetup`). The
wizard's channels step says "adding OBD2 readings…" and refreshes its list when
they land.

## Consequences

- `WANTS_OBD2_MAKES` in `obd2_autosetup.c` is the one list of such presets.
  Matched on make so a future Falcon preset inherits it; add a make there, not
  a special case at a call site.
- The wizard's `s_wiz_obd2_*` state, the old step's scan button and the scan
  sheet's `s_obd2_auto_apply` are gone. The channels editor's "Scan for OBD2"
  sheet is unchanged.
- `/api/ecu/set` may answer `{"ok":true,"obd2_autosetup":true}`.
- A Falcon's OBD2 channels join the car a few seconds after the preset, not at
  the same moment — said on every surface that shows the preset.
- **Unverified on hardware:** the retry cadence, the scan completing on a real
  Falcon while its broadcast is flowing, and the channels step refreshing mid-
  review. The layout lines up with the previous OBD2 step's positions but has
  not been seen on glass.
