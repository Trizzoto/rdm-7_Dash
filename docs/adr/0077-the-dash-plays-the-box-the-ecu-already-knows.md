# ADR-0077: The dash plays the box the ECU already knows

Date: 2026-09-15
Status: Accepted, Phase 1 of `docs/CAN_EMULATION_PLAN_2026-09.md`. Built and
walked on the bench dash `RDM-D926-1B8C` over USB; frame bytes proven by native
tests. **Not yet on a car or a Haltech.**
Repos: RDM-7_Dash (`main/can/can_emu{,_core}.{c,h}`, `main/can/can_emu_templates.json`,
`main/ui/settings/can_emu_settings.{c,h}`, `main/can/can_manager.{c,h}`,
`main/net/web_server_can.c`, `main/net/serial_commands{,_capture}.c`,
`main/widgets/widget_{button,toggle}.{c,h}`, `main/ui/settings/device_settings.c`,
`main/ui/dashboard.c`, `main/web/index.html`, `schema/`, `tools/`,
`tests/native/test_can_emu.c`), rdm7-desktop (`src/firmware-base.html`)

## Context

A customer with a Nissan Skyline / Infiniti G35 on a Haltech wants cruise
control buttons on the dash's touchscreen. Haltech cruise control reads its
buttons as **one analog input with a different voltage per button**. The
cheapest analog input to hand a Haltech over CAN is a **Haltech IO 12
Expander ("IO Box A/B")** — and the dash is already on that bus.

The owner's brief: not a one-off. Anyone's spec should be possible, from a
keypad menu in Device Settings.

What existed could not do it. A button widget sent one bit field, all ones or
zero, on an 11-bit ID. It had no value per button, no keep-alive, and no way to
read anything back. Two buttons on one ID also wiped each other's bits, because
each send started from a zeroed frame.

The IO box protocol comes from PT Motorsport's open-source emulators, which run
on Haltech ECUs today.

| Frame | Box A ID | Content |
|---|---|---|
| Analog inputs | 0x2C0 | four 12-bit counts (0–4095 = 0–5 V), each in a big-endian 16-bit slot, every 20 ms |
| Switch inputs | 0x2C2, 0x2C4 | bytes 0 and 4: 250 = on, 0 = off, every 20 ms |
| Keep-alive | 0x2C6 | 5 bytes `10 09 0D 01 00`, every 100 ms |
| Outputs (from the ECU) | 0x2D0, 0x2D2 | bytes 0 and 4 |

Box B is every ID + 1.

## Decision

**One engine; devices are data.** A device (`can_emu_core.h`) has three parts:
- **Controls:** momentary, latch, or select. A select is several buttons
  sharing one input, each holding it at its own state.
- **Frames it sends:** fields whose value comes from a control's state table,
  a constant, a counter, or a live channel. Values are in the ECU's own units,
  with scale, offset and an optional min/max.
- **Frames it listens to:** decoded into ordinary channels, so the ECU's
  outputs can colour buttons through the existing widget rules.

Bit positions use the layout `signals[]` keys (`bit_start`, `bit_length`,
`endian`), so a field reads the same as a channel decode.

**Templates are the same data, stored once:** `main/can/can_emu_templates.json`.
The firmware embeds a minified copy; the web editor is served it; the native
test reads the source file. Phase 1 ships:
- **Haltech cruise buttons:** Set / Resume / Cancel on analog 1 at PT
  Motorsport's proven 5.00 / 2.50 / 4.29 V, and on/off on switch 1.
- **Haltech IO 12 Expander:** four analog and four switch inputs, plus four
  outputs back.
- **My own spec:** blank.

Each Haltech template comes as Box A or Box B.

**Per car, not per layout:** the devices live in `/lfs/can_emu.json`. Changing
page must not unplug the ECU's IO box.

**Rules about when not to use the bus** (`can_emu.c`):

| Situation | Behaviour |
|---|---|
| Timing | One 10 ms LVGL timer. If the UI freezes, frames stop, so the ECU sees the box vanish and never a stuck "SET" |
| Sending | Single-shot and non-blocking (`can_try_transmit_frame_ext`) |
| Press latency | A press makes that control's frames due on the next tick |
| Bus bitrate isn't the device's | Refused, with both rates in the message; the dash's rate is never changed |
| Something else already sends one of its IDs | Refused: *"Something else is already sending 0x2C0. Is the real device fitted?"* The dash never hears its own frames, so hearing the ID means a real box. It resumes after 3 s of quiet. The send IDs are added to the hardware filter so the clash can be heard |
| OBD2 request IDs, RDM device-bus block | Refused when the file is stored. `_tx_refused_reason` moved into `can_manager` as `can_tx_refused_reason`, shared with `/api/can/send` |
| Nothing ACKs | 25 failures in a row, then a 3 s pause, logged once per streak |
| Simulator on | Buttons still work; channel-fed fields send their "no data" value. Simulated data never reaches an ECU |
| Screen changes / widget destroyed mid-press | The widget lets go of its control |

**Buttons and toggles press controls by name.** The new `control` field takes
`"device:control[:state]"`. The web inspector shows it as **Presses**, a
dropdown of the dash's controls. A latch control keeps its state in the engine,
so it survives a layout reload.

**The old `tx_*` outputs run through the same engine**, composed per ID. This
fixes the shared-frame bug. The OFF burst and `tx_rate_hz` behave as before.
A latched normal button now shows as pressed while it is on (`LV_STATE_CHECKED`).

**Where people set it up:**
- **Dash:** Device Settings → Your car → **Keypads & IO** (new keypad icon;
  the page's last free cell). It lists devices with a status (ON THE BUS /
  STOPPED / PAUSED), a switch, and add-from-template. The device view has
  hold-to-test buttons, the bytes each frame carries right now, and the ECU's
  replies.
- **Web editor, and Studio after a sync:** Setup → Your car → **Keypads & IO
  boxes**. One table per device:

  | Input | Button | Goes to | Sends |
  |---|---|---|---|
  | Cruise buttons | Set | 0x2C0 bytes 1-2 | 5.00 V |

  The page also has:
  - add/rename/remove buttons on a select;
  - warnings when two buttons are under 0.3 V apart or outside the input's range;
  - the "Set up the ECU" steps for NSP;
  - hold-to-test;
  - a JSON editor per device and for the whole file, for any spec the tables
    don't cover.

  There is no `confirm()`: Studio's webview returns a truthy Promise.

**Bundles:** a full-dashboard `.rdm` carries the devices as entry type 5
(`can_emu.json`). Importing one asks before replacing this dash's devices,
and asks ahead of the channel restore, which reboots the dash. Layout-only
bundles never include them.

**API:** `GET /api/can/emu[?templates=1]` and `POST /api/can/emu`, which takes
either a whole file or an action: test, set, add, enable, remove, release_all.
Two URI handlers. The USB twin is serial RPC `can.emu` with the same
request/reply.

## Consequences

- **Flash.** The app partition had 110 KB free this morning. This work costs
  about 40 KB (`idf.py size-files`: `can_emu.c` 9.7, `can_emu_core.c` 9.6, the
  settings popup 7.1, templates 5.1 minified, icon ~1, plus the web page and
  widget/API changes). Another session's symbol font landed the same day.
  After both, **48 KB (1%)** is left. The engine's tables are in PSRAM
  (`EXT_RAM_BSS_ATTR`), not the internal RAM WiFi needs. The partition table can't change over OTA. Phases 2–3 (keypad
  templates, on-screen keypad widget) need space found first. That is the
  owner's decision.
- **Verified on the bench dash, over USB (`can.emu` + touch + screenshot):**
  - A cold boot with a stored device is clean. TWAI installs, and the device
    goes on the bus 300 ms after init.
  - Wrong bitrate is refused with a reason.
  - Clash detection works: with the CAN simulator broadcasting 0x360 onto a
    test device's ID, the device stopped with the message above and resumed
    3 s after the simulator went quiet.
  - Layout buttons bound to `cruise_a:cruise:set` / `resume` hold the ladder
    at `0F FF` / `08 00` and release to `00 00`.
  - The CRUISE latch puts `FA` on 0x2C2 and stays on through a layout reload.
  - Popup views look right, and remove and add-from-template work. Adding the
    same box twice is refused by name.
- **Proven off the car:** frame bytes against PT Motorsport's layout, Box B
  offsets, ladder release order, the range clamp, and the refusals
  (`tests/native/test_can_emu.c`, 14 tests).
- **Not verified:**
  - Nothing on the bench bus ACKs, so no frame has been seen by another node.
  - No Haltech has read this box.
  - Unknown whether NSP needs the keep-alive, and what a Haltech does when the
    box disappears.
  - The customer's car is the test (plan §8).
- **Found on the way:** a remote-touch "down" auto-releases after 350 ms of
  silence (`remote_touch.c`). A test that holds a button over USB must repeat
  "down" faster than that.
- **Not changed:**
  - `rdm7-wasm-editor`'s button/toggle ignore `control`. Harmless: Studio's
    canvas draws them the same.
  - The phone app's vendored schema doesn't have the field yet.
  - A button bound to a control that isn't on this dash does nothing and says
    nothing. The inspector does show it as "(not on this dash)".
  - Blink/Grayhill keypads, the keypad widget and DBC import are Phases 2–4.
