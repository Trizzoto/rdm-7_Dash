# The dash as a keypad or IO box — plan

Date: 2026-09-15
Status: **Phase 1 built** (2026-09-15, ADR-0077). Phases 2-4 not started.
Where Phase 1 differs from this plan:
- the JSON uses the layout's own signal keys (`can_id`, `bit_start`, `bit_length`,
  `endian`), not `byte`/`bits`/`order`;
- the simulator doesn't pause a device: buttons keep working, and only
  channel-fed fields send their no-data value;
- a field can carry `min`/`max`, because 0-5 V sits in a 16-bit slot;
- adding from the glass covers the Haltech templates only. The web editor has
  a JSON editor per device, not yet the table editor for My own spec;
- the TX queue is 16, not 32;
- a widget whose control is missing doesn't press anything, but doesn't yet
  say "not set up" on the glass.
Repos: RDM-7_Dash (firmware + web editor), rdm7-desktop (sync + keypad art),
rdm7-wasm-editor (Phase 3 widget port).

## 1. Why

Tom Dostal (Nissan Skyline / Infiniti G35, VQ35 petrol, Haltech) wants cruise
control buttons on the dash's touchscreen. Haltech's cruise control reads its
buttons as **one analog input with a different voltage per button**, and the
cheapest way to hand Haltech an analog input over CAN is to pretend to be a
**Haltech IO 12 Expander** ("IO Box A" or "B"). The dash already sits on the
Haltech's CAN bus, so no wiring and no hardware mod.

The owner's brief: don't build a one-off for Tom. Make it generic, **so
anyone's spec can be set up**, with a keypad menu in Device Settings, and a
Haltech / Blink Marine keypad drawn on the screen as a nice touch.

What "generic" has to cover, from real requests and the market research:

| Pretend to be | Read natively by | What the ECU gets |
|---|---|---|
| Haltech IO 12 Box A / B | Haltech Elite, Nexus (NSP) | 4 analog inputs (0–5 V), 4 switch inputs; sends 4 outputs back |
| Haltech CAN keypad (Grayhill 3K hardware, node 0x0C) | Haltech NSP | key presses; sends 3 LEDs per key back |
| MoTeC keypad (Grayhill 3K, node 0x0A) | MoTeC M1 / dash | key presses; LEDs back |
| Blink Marine PKP, CANopen node 0x15 | Link G4X, ECUMaster, others | key presses; LED colours back |
| Blink Marine PKP, J1939 | MaxxECU | key presses |
| Anything else | any ECU with a CAN input map | whatever the user's spec says |

## 2. What exists today (and why it isn't enough)

| Piece | What it does | Gap |
|---|---|---|
| Button / toggle widget `tx_*` fields | Sends one bit field, all-ones or zero, 8-byte frame | 11-bit IDs only; can't send a *value* (a voltage); **two buttons on one ID wipe each other's bits** (`widget_button.c:71`, `widget_toggle.c:115` start from a zeroed frame); no keep-alive; blocking 5 ms send on the render task; no simulator/suspend guard; can transmit on OBD2 and RDM-bus IDs that `/api/can/send` refuses |
| `can_forward.c` (fuel sender → CAN) | Periodic packer with rate clamp, fail counter, 5 s backoff, suspend + simulator guards, extended IDs | Fuel only — but it is the right pattern |
| `keypad_lights.c` | Plays a baked LED boot show to a real Blink keypad | Deliberately not a keypad manager (ADR-0068) |
| Studio keypad workspace | SVG PKP picture, key modes, CANopen/J1939 encoders and decoders, DBC export | DOM/SVG in Studio only; the dash's LVGL can't use it |
| `_tx_refused_reason` | Refuses OBD2 request IDs, RDM bus block, discovery ID | Lives in `web_server_can.c`, applies to `/api/can/send` only |

**Budgets:** URI handlers 160 of 192. App partition **110 KB free (3%)** on
the 2026-09-15 build. That one matters most: see §9.

## 3. The idea in one paragraph

The dash gets one small engine that plays **devices**. A device is data, not
code: a list of **controls** (things a finger presses), the **frames it sends**
(where each control's value goes, how often), and the **frames it listens to**
(what the ECU sends back, turned into ordinary channels). Haltech IO Box A,
a Blink PKP and "Tom's custom box" are all the same thing: a ready-made
**template** of that data, which the user can open and change. Buttons,
toggles, and a new keypad widget press controls by name. The engine composes
every frame from all the controls that write into it, so shared frames just
work.

```
 layout button "SET" ─┐                         ┌─> 0x2C0  every 20 ms  (AVI 1 = 5.00 V while held)
 layout button "RES" ─┼─> control "cruise" ─────┤
 keypad widget key 3 ─┘   (idle/set/res/cancel) ├─> 0x2C2  every 20 ms  (switch inputs)
 layout toggle "ON"  ───> control "cruise_on" ──┘   0x2C6  every 100 ms (keep-alive)

 ECU ─> 0x2D0 (DPO 1 = cruise lamp) ─> channel IOA_DPO1 ─> widget rule colours the ON button
```

## 4. Design decisions

### 4.1 One engine, devices as data — `main/can/can_emu.{c,h}`

- **Stored per car, not per layout:** `/lfs/can_emu.json`. Switching from the
  street page to the track page must not make the ECU think its IO box was
  unplugged. A layout's widgets refer to controls by id (`cruise:set`); a
  widget whose control doesn't exist shows "not set up" instead of sending.
  (The layout root would also be the wrong home for a second reason: the
  on-device save rebuilds the root in `layout_manager_build_json` and drops
  keys it doesn't know.) "Save full dashboard" / `.rdm` bundles include the
  file.
- **Runs on the LVGL task**, from one timer, like `can_forward`. If the UI
  freezes, frames stop, and the ECU sees its box go missing — which is the
  failure we want, not a box stuck sending "SET".
- **Sends with the non-blocking single-shot transmit** (`can_try_transmit_frame`
  gains an `_ext` twin). A periodic frame that misses one slot is resent by the
  next; a bench dash alone on a bus doesn't generate an error storm, and the
  render thread never waits.
- TWAI `tx_queue_len` goes from the IDF default (5) to 32: IO Box A alone is
  ~160 frames/s.

### 4.2 The device format

Values are stored in the ECU's own units, with the scale on the field, so a
spec stays readable ("5.00 V", not "4095"):

```json
{
  "v": 1,
  "devices": [{
    "id": "cruise_box", "name": "Cruise buttons", "template": "haltech_io12_a",
    "enabled": true, "bitrate": 1000,
    "controls": [
      { "id": "cruise",    "name": "Cruise",        "kind": "select",
        "states": ["idle", "set", "resume", "cancel"] },
      { "id": "cruise_on", "name": "Cruise on/off", "kind": "latch" }
    ],
    "send": [
      { "id": "0x2C0", "every_ms": 20, "dlc": 8, "fields": [
        { "byte": 0, "bits": 16, "order": "big", "scale": 0.0012210, "unit": "V",
          "control": "cruise",
          "values": { "idle": 0.00, "set": 5.00, "resume": 2.50, "cancel": 4.29 } } ] },
      { "id": "0x2C2", "every_ms": 20, "dlc": 8, "fields": [
        { "byte": 0, "bits": 8, "control": "cruise_on", "values": { "off": 0, "on": 250 } } ] },
      { "id": "0x2C6", "every_ms": 100, "dlc": 5, "data": "10090D0100" }
    ],
    "listen": [
      { "id": "0x2D0", "stale_ms": 2000, "channels": [
        { "name": "IOA_DPO1", "byte": 0, "bits": 8, "scale": 0.4, "unit": "%" } ] }
    ]
  }]
}
```

| Control kind | Pressed by | Value |
|---|---|---|
| `momentary` | button held | `on` while held, `off` otherwise |
| `latch` | toggle, or a latch button | `on` / `off`, optionally remembered across power cycles |
| `select` | several buttons, one state each | the state of the button held **most recently**; back to the next still-held button, then `idle` |
| `channel` | nothing — a live channel (e.g. the fuel sender) | the channel's value, with a stated value for "no data" |

| Field source | Example |
|---|---|
| a control's state → value table | AVI ladder, key bit, switch 250/0 |
| a channel | send the dash's own fuel sender as a Haltech AVI |
| constant bytes | keep-alive, CANopen boot-up |
| counter | Blink's 100 ms tick byte |

`listen` channels are **ordinary channels** (same registry, same hardware
filter, same rules and logging). So a lamp, an LED colour, or "cruise active"
colours a button through widget rules that already exist — no new binding
system. A listened frame that goes stale reads as no data, and the button
shows grey.

### 4.3 Templates are data, one copy

Templates are JSON files in `main/data/can_emu_templates/`, embedded in the
firmware and served to the web editor (`GET /api/can/emu?templates=1`), so the
dash, the web editor, Studio and the phone app all show the same ones. Studio
offline bundles the copy that `sync_firmware.py` pulls. A template is only a
starting point: "Add Haltech IO Box A" copies it into the car's file, and from
then on it is the user's device to change.

Phase 1 ships: **Haltech IO 12 Box A**, **Box B** (every ID +1), and
**Blank device** (the "anyone's spec" start). Phase 2 adds the keypads.

### 4.4 Safety rules

Buttons press instantly — no hold-to-confirm (owner's decision, earlier). The
rules below are about the bus, not about the finger.

| Situation | What the engine does |
|---|---|
| Finger slides off, screen changes, layout reloads, edit mode opens, screen sleeps | Every momentary and select control goes back to idle **immediately**; frames keep flowing, so the ECU sees "released", never "box gone" |
| Another node is already sending one of our IDs (a real IO box fitted) | The ESP32 never hears its own frames, so any received frame on one of our IDs is someone else. The device stops and says so: *"Something else is already sending 0x2C0 — is a real IO Box A fitted? Use Box B."* Our send IDs are added to the hardware filter so this can be seen |
| Dash bitrate ≠ the device's | Device doesn't start: *"Haltech's bus runs at 1 Mbit/s; this dash is set to 500 kbit/s."* Never changes the bitrate by itself |
| Simulator on | Buttons still work; a field fed from a channel sends its no-data value (as built — the plan said pause) |
| Bus scan / suspend | Device paused, status says why |
| Sends keep failing | `can_forward`'s backoff: 20 fails → 5 s pause, counted and shown |
| An ID the dash must not send (OBD2 requests, RDM bus block, discovery) | Refused. `_tx_refused_reason` moves into `can_manager` so the engine, `/api/can/send` and old-style buttons share it |
| ECU's replies stop (`listen` stale) | Channels read no data. For anything the ECU *drives through us* (a future RDM IO output), outputs fall to their safe value after 2 s — the same rule PT Motorsport's newer box follows |

### 4.5 Old button/toggle `tx_*` fields keep working, through the engine

Existing layouts are untouched. At load, each widget's `tx_*` fields become a
hidden one-field device in the engine, so they get the composer (**fixes the
shared-ID bug**), the guards, and the non-blocking send. The inspector shows
the old fields only on widgets that already use them, with a "Move to a
device" button.

## 5. Where the user sets it up

### 5.1 On the dash — Device Settings → Your car → **Keypads & IO boxes**

The "Your car" page has exactly one free cell (Odometer is stretched into it).

Tile: **Keypads & IO boxes** · *Buttons your ECU reads as its own keypad or IO
box* · status: `2 ON` / `OFF` / `CHECK` (red when a device refused to start).

Popup:

| Device | Pretends to be | Status | |
|---|---|---|---|
| Cruise buttons | Haltech IO Box A | SENDING · 160/s | on/off |
| Pit keypad | Blink PKP (Link) | REFUSED · bus is 500k | on/off |

Tap a device → its controls, each with a **hold-to-test** pad and the value
being sent right now ("AVI 1 · 5.00 V"), plus what the ECU sent back ("DPO 1
· on"). Adding from the glass: pick a template, answer "Box A or B", done —
button voltages default to PT Motorsport's proven cruise values. Anything more
(custom specs, editing frames) is the editor's job; typing a spec on a 7"
touchscreen is not a good time.

### 5.2 In the editor (dash web editor, and Studio after a sync)

Setup → Your car → new card **Keypads & IO boxes**, beside ECU & CAN bus. It is
authored in the firmware web editor (the dash's own function, like the fuel
sender forwarding — firmware-first per STUDIO_SHELL_PLAN §2.0), so phones and
the web editor get it too. Studio's keypad workspace stays the place for
**real** keypads.

The page, plain language throughout (no "bitfield", no "u16" — ruled tables):

1. **Add** — cards: Haltech IO Box A · Haltech IO Box B · *(Phase 2)* Haltech
   keypad · MoTeC keypad · Blink keypad for Link / ECUMaster · Blink keypad for
   MaxxECU · **My own spec**.
2. **Haltech IO box** view: one table per input type.

   | Input | Used for | Buttons on this input |
   |---|---|---|
   | Analog 1 | Cruise buttons (one wire, a voltage per button) | Idle 0.00 V · Set 5.00 V · Resume 2.50 V · Cancel 4.29 V |
   | Analog 2 | — | |
   | Switch 1 | Cruise on/off | on = on |

   Voltage fields warn when two buttons on one input are closer than 0.3 V
   (Haltech's minimum is 0.1 V; a margin avoids a mis-read). Outputs table:
   *Output 1 → channel IOA_DPO1 (e.g. cruise lamp)*.
3. **Set up the ECU** — step-by-step for that ECU's software, e.g. NSP: enable
   IO Box A → Analog 1 function "Cruise control multi-button" → for each
   button: **hold it on the dash**, click Calibrate. Marked *unverified* until
   checked against real NSP.
4. **My own spec** — the same format in tables: *Frame · ID · Sends every*,
   then *Where in the frame · Value when idle · when pressed · or from channel*.
   The byte/bit picker and live probe are the channel decode editor's
   (ADR-0071), reused. A live row shows the exact bytes as you click the
   picture's buttons. Import / export the device as JSON; DBC import in Phase 4.
5. **Try it** — pressing a button in the editor presses it on the dash (hold
   to test), and the ECU's replies show underneath from `/api/can/monitor`.

### 5.3 API

`GET /api/can/emu` (devices + live status, `?templates=1` for templates) and
`POST /api/can/emu` (save; `{"action":"test","control":"cruise:set","held":true}`
for hold-to-test, which releases itself if the editor stops asking for 1 s).
**Two** URI handlers → 162 of 192.

## 6. The keypad on the screen (the "nice touch")

A new widget, **keypad**:

- **Look:** *Blink PKP* (black housing, round caps, colour ring — geometry from
  Studio's `kpBuildSvg`: 104 px cell, cap/ring/well radii, legend backlight)
  or *Haltech / Grayhill* (square caps, three LEDs per key). Sizes follow the
  real parts: PKP 2×2, 2×4, 2×6, 3×5; the Grayhill/Haltech sizes to be
  confirmed from a real unit.
- **Each key** has a legend (icon or text) and presses one control state.
- **The key's lights come from the ECU when the device listens for them** —
  that is the emulation: NSP lights a Haltech key amber because the function
  it controls is on, and the dash draws exactly that. Without a listening
  device, widget rules colour the ring.
- Press: the cap sinks, like Studio's picture.
- Needs the full new-widget path: firmware widget + schema + both codegens +
  offline SVG preview (`_pvKeypad`) + a port to `rdm7-wasm-editor` so Studio's
  canvas isn't blank + the phone app's vendored schema.

Studio's keypad workspace later gets **"Put this keypad on the dash screen"**
(its design → a keypad widget + a Blink device), so a customer can design one
keypad and have both the real part and the on-screen twin.

## 7. Phases

| # | What | Ships to | Exit |
|---|---|---|---|
| **0** | Facts we don't have (§8) | — | Tom's answers; a raw CAN capture from his car |
| **1** | Engine + `/lfs/can_emu.json` + API; Haltech IO Box A/B + Blank templates; `select`/`momentary`/`latch` controls; button/toggle bind to controls; old `tx_*` moved onto the engine; shared refusal policy; collision + bitrate + backoff guards; Device Settings tile; editor card for Haltech IO boxes with the NSP guide | **Tom** (test build) | Tom's cruise sets, resumes and cancels from the screen; box-loss behaviour observed and written down |
| **2** | Keypad templates: Blink PKP CANopen (Link/ECUMaster), Blink J1939 (MaxxECU), Haltech keypad, MoTeC keypad; `listen` LED feedback; `counter` fields | Link / MaxxECU customers; Haltech keypad marked experimental until captured | Our emulated PKP's frames match a real PKP's byte for byte on the bench |
| **3** | Keypad widget (firmware, WASM, schema, preview, phone) with both looks; Studio "Put this keypad on the dash" | everyone | Studio canvas and dash draw the same keypad; ECU-lit keys light |
| **4** | "My own spec" table editor, JSON import/export, DBC import; fuel forwarding moves onto the engine as a `channel` field; same device format adopted by RDM IO Pico/Pro's "CAN emulation modes" (PLATFORM_PLAN §6.3) | power users | A spec from a customer's PDF set up with no firmware change |

Phase 1 is the smallest thing that answers Tom and still lays the generic
foundation; nothing in it is Haltech-specific C code.

**Tests, every phase:** native tests in `tests/native/` for the composer
(template in, exact bytes out; a golden fixture of IO Box A frames matching
PT Motorsport's open-source emulator), select-control release order,
collision and refusal. A `check_can_emu.js` in rdm7-desktop runs the editor's
encoders against the same fixture (the ADR-0068 cross-repo pattern). Bench
on `RDM-A2EC-C854` over USB with a second CAN node as the "ECU".

## 8. What we don't know yet — Phase 0

| Question | Why it matters | How we find out |
|---|---|---|
| Which Haltech (Elite 1500/2500, Nexus) and NSP version? | Cruise and IO Box support differ by ECU | Ask Tom |
| Manual or auto? Is the throttle drive-by-wire, and is a vehicle speed input set up? | Haltech cruise needs DBW and speed, and a clutch switch on a manual | Ask Tom |
| Bus bitrate, and is a real IO box already on the bus? | Box A vs B; 1 Mbit | Ask Tom; the dash's CAN monitor shows it |
| Does NSP need the keep-alive (0x2C6) to accept the box? What does it do when the box vanishes — cancel cruise, flag an error, hold? | The whole safety story | Test build on Tom's car, engine off first: unplug the dash, watch NSP |
| At key-on, the ECU is up before the dash (~a few seconds). Does it log "IO box missing"? | A nuisance error at every start | Same test |
| What a real Haltech keypad sends at start-up (boot-up frame, heartbeat, NMT) | NSP may ignore a keypad that only sends key frames | A capture from a real Haltech keypad on a Haltech ECU — dealer, forum, or a customer; the dash's raw CAN logger can record it |
| Blink CANopen key frame length (5 or 8) and tick | Link may check it | Our own PKP on the bench |

Tom's own AI-written spec has two mistakes worth telling him about: the analog
value is **12-bit counts, not millivolts** (1 V = 819 = bytes `03 33`), and
**Box B is 0x2C1**, not 0x2C4 (0x2C4 is Box A's second switch frame).

## 9. Risks

- **Flash is the tightest budget.** 110 KB free. Rough cost: engine + store +
  API 15–20 KB, on-glass popup ~10 KB, templates 3–5 KB, keypad widget
  10–15 KB — most of what's left after Phase 3. Each phase reports its build
  size in the ADR. The partition table can't be changed over OTA (units in the
  field would need a USB flash), so if space runs out that is an owner
  decision, not something to slip in.
- **Pretending to be someone else's hardware.** If a Haltech update changes
  what NSP expects, our "IO box" goes quiet. The status line must say *"no
  reply from the ECU"* rather than look fine, and the guide says this is an
  emulation, not a Haltech part.
- **A cruise button is a driving control.** We don't gate presses (owner's
  call), but release is immediate and loss of the dash reads as loss of the
  box. Phase 1 ships to Tom as a test build with the box-loss test done on his
  car before he drives with it.
- **The editor could grow into a second DBC tool.** Phase 4's spec editor
  reuses the decode editor's pieces; it must not become its own.

## 10. Considered and not done

| Option | Why not |
|---|---|
| Wire a dash pin to a Haltech analog input (remove the spare input's pull-up, drive a voltage) | The ESP32-S3 has no DAC — it would be PWM through an RC filter, a hardware mod per unit, and noise on a line that sets cruise. CAN needs no hardware |
| Extend the button widget's `tx_*` fields | Can't send a value per button, extended IDs, a shared frame, a keep-alive, or read anything back |
| Hand-written C per device (a `haltech_io.c`, a `blink_pkp.c`) | Flash per device, and every customer spec becomes a firmware release |
| Store devices in the layout | Changing page would unplug the ECU's IO box; on-device save drops unknown root keys |

## References

- Haltech IO 12 protocol: PT Motorsport table + github.com/ptmotorsport/IObox-emulator-haltech (MIT) — AVI 0x2C0 (4× 12-bit, big-endian 16-bit slots, 50 Hz), switch inputs 0x2C2/0x2C4 (250 = on), outputs from ECU 0x2D0/0x2D2 (0x00 off, 0xFA on, between = PWM), keep-alive 0x2C6 `10 09 0D 01 00` at 10 Hz; Box B = every ID +1
- Haltech cruise multi-button calibration (NSP docs); PT Motorsport IO Nano cruise example (idle 0 V, Set 5 V, Resume 2.5 V, Cancel 4.29 V)
- Grayhill 3K CANopen manual (TPDO1 0x180+node key bits, RPDO1 0x200+node 3 LED bits per key, RPDO2 0x300+node brightness)
- Blink PKP: `docs/research/keypad-datasheets/`, rdm7-desktop `docs/BLINK_MARINE_PKP2200_CANOPEN_2026-08-28.md`
- `docs/research/2026-07-keypad-market.md`, `docs/PLATFORM_PLAN_2026-07.md` §6.1/§6.3, `docs/KEYPAD_WORKSPACE_PLAN.md`
- ADR-0055 (dash as the CAN interface), 0060, 0062, 0063, 0068, 0071
