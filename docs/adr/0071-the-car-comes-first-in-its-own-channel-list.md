# ADR-0071: The car comes first in its own channel list

Date: 2026-09-14
Status: Accepted — web verified in the browser (connected mock, `--local-stub`,
and RDM Studio's merged build in Offline); firmware builds; not yet flashed
Repos: RDM-7_Dash (`main/web/index.html`, `main/ui/screens/first_run_wizard.c`,
`tools/mobile-dev-server.js`), rdm7-desktop (`src/firmware-base.html`, via
`tools/sync_firmware.py`)
Follows ADR-0032/0034 (adding and removing a channel), ADR-0035 (the source
column speaks provenance), ADR-0069 (one source picker).

## Context

A review of the whole "set up this car's channels" path — the Channels page,
adding an ECU's extra signals, building a multiplexed channel by hand, and the
dash's own first-boot wizard — run the way a customer runs it.

| Where | What happened | Why it matters |
|---|---|---|
| Channels page | A car with 4 channels showed them spread across 16 group headings among 132 greyed catalogue rows; the Lambda channel built by hand sat below all of them | The list is where you check your car; your car was the hardest thing to see on it |
| Channels page, Source column | Coolant, oil pressure and lambda all read `0x3E8` | On a multiplexed id the frame index is half the identity (ADR-0035) |
| Channels page, Source column | "Not set up" printed on 132 rows | Noise, and it hid the one row that deserved the words: a channel on the car with nothing feeding it |
| Add channels from your ECU | Signals the car already had were offered as new, tickable, unmarked | You found out afterwards: "Added 3 of 5 — the rest were already set up" |
| Add channels from your ECU | 38 extras meant 38 ticks | Adding "the rest of my ECU" is the common case |
| Add channels, offline | Opened on whichever make sorted first, with a heading of "Other ECUs" on a fresh install | The car's own channels already say which ECU it runs |
| New channel / decode editor | Byte-0 multiplexing was detected ("this ID looks multiplexed") and then left to the user: open a disclosure, know the index is 8 bits at bit 0, type both, guess a frame number | A frame is recognised by its value ("coolant is about 88"), not by its index |
| New channel | After Create, the drawer heading still read "New channel" over the new channel's editor | |
| Widget channel picker | Same scatter, and custom channels forced into a "Custom" group — the bug the Channels list fixed months ago | Two lists disagreeing about where a channel is |
| Quick ECU Setup | "Auto" ticked showed *more* presets; unticked hid them. The footer said detection came from "the last Scan Car" while the modal polls live scores every 1.5 s | |
| Dash wizard, step 4 | The list was the catalogue (135 rows) against a 144-row cap, with custom channels after it | **Applying an ECU creates a `custom_` channel for every signal with no canonical home — most of a Haltech's 69, most of a MaxxECU's 100 — and only about seven of them could ever appear.** The cap's comment still said "~90" canonical channels |
| Dash wizard, step 3 | A correct detection of a wide preset printed "5 of 21 CAN IDs matched (23% confidence)" | The screen whose job is "yes, that's your ECU" read as a shrug |

And two in the browser dev server, the same class as ADR-0069's: `/api/ecu/current`
answered `{ecu:…}` where the firmware sends `{make:…}` (so the header said
"ECU preset: None" while the picker said the car ran a Link), and `/api/ecu/list`
answered a list of names no page has read since the picker became cards, so
Quick ECU Setup could not be reviewed at all.

## Decision

**One list, two headings: "On this car", then "Not set up".** Every channel with
a record on the dash — built-in or custom, bound or not — under the first,
grouped as before; the rest of the catalogue under the second. This keeps the
reason 39cc91e removed the old *mode*: nothing is behind a view, search filters
both halves, and the answer to "where is X?" is still "in the list". It only
changes what you see first. The widget's channel picker uses the same split
(`_chSplitByCar`), so the two lists cannot disagree.

**The dash does the same**, with "+ Add custom channel" moved up to sit with the
car it adds to. The row cap stays at 144 deliberately: in LVGL v8 every row
costs an `lv_obj_is_valid()` walk of the whole object tree per refresh, and a
longer list is not something to take on without a dash to measure frame rate
on. With the car first, a full list truncates the catalogue's tail and says so
("N more in the standard list — add them from Channels in RDM Studio"), never a
channel that exists. The refresh loop now skips ghost rows before that walk
rather than after it. No counts in the dash's headings: adding a channel
updates its row in place so nothing jumps under your finger, which would leave
a count stale.

**The source column prints the frame** (`0x3E8 · frame 2`), is blank under
"Not set up", and says "No source yet" for a car channel with nothing bound.

**The add picker knows what the car has.** A row whose decode (frame, bit
offset, frame index) is already a channel is shown ticked, muted, "on this car",
and cannot be ticked again; inside the version the car runs, a bound signal of
the same name also counts, so a stream moved to another base id still matches.
"Tick all N not on this car" adds the rest in one press. With no dash to say
which ECU the car runs, the picker opens on the version that already feeds the
most of its channels.

**The probe fixes what it detects.** "Use byte 0 as the frame index" sets the
gate in one press. With a gate on, the probe lists every frame index it has
heard as a button showing what the current bit window reads *in that frame*;
pressing one selects it. Changing only the index re-renders from frames the
dash already bucketed — the focus buffer depends on the gate's geometry, not its
value — so choosing frames is instant. In a channel's drawer these go through
the same buffered edit as typing, and apply on Save.

**Plain words where the numbers misled.** The ECU filter box says what it does
("Only ones the dash can hear", the inverse of the stored `auto_mode`, which the
dash's own picker shares), and the wizard reports "Heard 5 of the 21 frames this
ECU can send" instead of a percentage the ranking itself never trusted.

## Consequences

- `_refreshChannelsList` no longer builds `grouped`/`order`; add grouping
  changes to `_chSplitByCar`.
- `_probeAttach(srcFn, elId, setMuxFn)` takes an optional third argument. A
  consumer without one gets the old read-only probe.
- Mux inputs carry `data-mux="…"` so the probe can update them without
  re-rendering a form mid-edit.
- **Not changed, and worth a look with a dash on the bench:** the wizard asks
  about OBD2 (step 2) before detecting the ECU (step 3). The comment on
  `_btn_apply_cb` says ECU detect "gets its accumulation window" during the
  OBD2 step, but `_show_step_ecu_detect` resets the ID tracker on entry, so that
  reason no longer holds, and "anything your ECU doesn't broadcast will be
  filled in from OBD2" reads more naturally once the ECU is known. Swapping them
  moves CAN bitrate and filter state between steps, which is not a change to
  make blind.
- **Not changed:** the wizard's ECU picker rows print `make  version`
  (`MaxxECU  1.3`) rather than the preset's display name.

## Verification

- Browser, connected mock: list halves and counts; frames in the Source column;
  a custom channel created entirely from the probe (0x3E8 → "Use byte 0" →
  frame 8 → offset −50 → Create) lands as `custom_oil_temp_2` with
  `mux_bit_length 8, mux_value 8`, filed under On this car › Engine; a frame
  button in Coolant Temp's drawer buffers `{mux_value: 3}` behind "Unsaved
  changes", and Cancel restores frame 2; add picker marks 3 of Link Generic
  Dash's 41 rows and offers "Tick all 38".
- Browser, `--local-stub`: picker opens on Link ECU › Generic Dash from the
  mirror alone; Tick all → Add queues one import of 38 labels and the list shows
  the waiting banner.
- RDM Studio merged build (Offline, fresh storage): empty state under On this
  car, all 135 under Not set up, no console errors; `check_syntax`,
  `check_dash`, `check_offline`, `check_transport`, `check_shell`,
  `check_chanfix`, `check_chansolo`, `check_chanquiet` pass.
- Firmware: `idf.py build` clean with the wizard changes. Not flashed.
