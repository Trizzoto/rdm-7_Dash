# Multiplexed CAN in RDM-7

How the dash decodes a CAN ID that carries more than one payload, what the
contract is, and what you have to touch when you add a new surface — another
firmware version, a different editor, or an import path from someone else's
CAN tooling.

Read this if you are adding mux support somewhere, importing from a new file
format, or debugging a channel that reads plausible-but-wrong numbers.

---

## 1. What the problem actually is

Most ECUs give each quantity its own CAN ID: `0x5F0` is always RPM in bytes
0-1, forever. Decoding is `(id, bit_start, bit_length)`.

A **multiplexed** ECU cycles several different payloads through **one** ID and
tags each with a *frame index* — usually byte 0. Link's Generic Dash sends 14
of them on `0x3E8`:

```
0x3E8  00 00 1A 0C 00 64 00 32   frame 0 → bytes 2-3 = RPM,     4-5 = MAP
0x3E8  02 00 00 5A 00 00 00 00   frame 2 → bytes 2-3 = ...,     6-7 = coolant
0x3E8  08 00 00 46 00 00 03 E8   frame 8 → bytes 2-3 = oil temp, 4-5 = oil press
```

Bytes 2-3 mean something different in every frame. Decode `0x3E8` bits 16-31
without checking byte 0 and you get RPM, then oil temp, then a knock count,
several times a second — a number that looks like a live reading and is
garbage. **That is the whole hazard: multiplexing fails convincingly, not
obviously.** Everything below exists to make that failure impossible to hit by
accident.

## 2. The model

RDM stores the frame gate **on every multiplexed signal**, not as a separate
"multiplexor" object. Three fields:

| Field | Type | Meaning |
|---|---|---|
| `mux_bit_length` | 0-16 | Width of the frame-index field. **0 = not multiplexed** |
| `mux_bit_start` | 0-63 | Bit position of the frame index (0 = byte 0) |
| `mux_value` | 0-65535 | Decode only frames whose index equals this |

Constraint: `mux_bit_start + mux_bit_length <= 64`.

`mux_bit_length == 0` is the "off" state everywhere, and it is also what you
get from any older client that does not send the keys at all. That is
deliberate — every layer treats absent and zero identically, so nothing breaks
when an old editor talks to a new dash.

### Why on each signal rather than a separate switch object

DBC keeps the multiplexor as its own signal (`M`) and tags the others `mN`.
That is a fine file format but a poor runtime model: the decoder would have to
resolve a cross-reference per frame. Storing the resolved geometry on each
signal makes `signal_dispatch_frame()` a straight-line test with no lookup.
The cost is that the geometry is duplicated across a message's signals — which
only matters when converting **to** a format that wants a switch object, and
§6 covers that.

### The gate, in the decoder

`main/widgets/signal.c`, in `signal_dispatch_frame()`:

```c
if (sig->mux_bit_length) {
    uint8_t mux_end = (sig->mux_bit_start + sig->mux_bit_length - 1) / 8;
    if (dlc <= mux_end) continue;                 /* frame too short */
    int64_t mux = can_extract_bits(data, sig->mux_bit_start,
                                   sig->mux_bit_length,
                                   sig->endian, false);
    if ((uint16_t)mux != sig->mux_value) continue; /* not our frame */
}
```

Two details worth copying exactly if you reimplement this:

- The mux field is read with **the signal's own `endian`**, and always
  **unsigned**. A frame index is a small positive number; nothing else would
  make sense, and disagreeing with this makes previews differ from reality.
- The DLC guard runs **before** extraction, so a short frame is skipped rather
  than read out of bounds.

## 3. Where the fields live

The same three keys, unchanged, in five places:

| Layer | Location | Notes |
|---|---|---|
| Runtime | `signal_t` (`main/widgets/signal.h`) | set via `signal_set_mux()` |
| Channel | `channel_t` (`main/data/channel_manager.h`) | set via `channel_manager_set_mux()` |
| Persistence | `/lfs/channels.json`, inside each channel's `decode` object | **defaults-only**: omitted when `mux_bit_length == 0` |
| Presets | custom preset JSON on LittleFS, per signal | stored verbatim by the server |
| Wire | `/api/channels/create`, `/api/channels/update`, `/api/signal/update` | all accept the triple |

The defaults-only rule matters: an unmultiplexed channel must serialise
**exactly** as it did before mux existed, so existing `channels.json` files
round-trip byte-identical. Emitting `"mux_bit_length": 0` would churn every
file on the fleet for no reason.

### Clearing the gate

Send `mux_bit_length: 0` to `/api/channels/update` to remove a gate. Omitting
the key **leaves the existing gate alone** — that is what lets an editor with
no mux UI edit a muxed channel's scale without silently un-gating it. The
distinction between "absent" and "zero" is the only place those two differ;
everywhere else they mean the same thing.

## 4. The API

Create a multiplexed channel:

```bash
curl -X POST http://<dash-ip>/api/channels/create \
  -H 'Content-Type: application/json' \
  -d '{"label":"Oil Temp","units":"°C","decimals":0,"min":0,"max":150,
       "decode":{"can_id":1000,"bit_start":16,"bit_length":16,
                 "scale":1,"offset":-50,"endian":1,
                 "mux_bit_start":0,"mux_bit_length":8,"mux_value":8}}'
```

The channel id is derived from `label` (prefixed `custom_`), so you do not
pass one. Request bodies are capped at 768 bytes — one channel per call.

Read them back with `GET /api/channels`; the triple appears inside `decode`
only when the channel is multiplexed.

> There are **no channel methods in the serial RPC** (`tools/rdm_serial_rpc.py`
> covers device/layout/image/font/log/sd/wifi/signal). Channel work needs the
> dash on WiFi.

## 5. Seeing it on the bus: the focus buffer

`GET /api/can/monitor` returns one frame per ID — *the most recent, whatever
its index*. For a multiplexed ID that is useless for building a channel: poll a
stream cycling 14 payloads and you catch a random one each time, so a live
decode preview flickers across unrelated quantities.

The **focus buffer** (`main/can/can_id_tracker.c`) fixes that for the one ID
you are working on. Aim it from the same GET that reads it:

```
GET /api/can/monitor?focus=0x3E8&mux_start=0&mux_len=8&endian=1
```

The reply gains a `focus` block holding the latest payload for **each** mux
value seen:

```json
"focus": {
  "id": 1000,
  "frames": [
    {"mux": 0, "data": "00001A0C00640032", "dlc": 8, "count": 412, "age_ms": 18},
    {"mux": 2, "data": "0200005A00000000", "dlc": 8, "count": 411, "age_ms": 34}
  ],
  "capacity": 16
}
```

`?focus=0` turns it off. `focus` accepts hex or decimal.

Design notes if you touch it:

- **One ID at a time.** Holding per-mux payloads for all 64 tracked IDs would
  cost 64x for no benefit — only one ID is ever being edited.
- **Lock-free by split.** `set_focus()` publishes a *request* (word-sized
  scalars plus a sequence counter bumped last); the recording task adopts it
  and owns the slots exclusively. No cross-task write can corrupt the count
  while frames are landing. Readers on the HTTP task accept the same benign
  torn read the bus monitor already documents.
- **`set_focus()` is idempotent.** Calling it with identical arguments does not
  discard collected slots, so a UI can call it on every poll.
- **16 slots.** A stream with more distinct mux values keeps the first 16 and
  logs once, rather than thrashing.

### `mux_seen` — the cheap hint

Each `ids` entry carries `mux_seen` when that ID has shown **more than one**
byte-0 value: a bitmap of values 0-15. It is how the editor warns "this ID
looks multiplexed" before the user has configured anything, and how preset
auto-detect scores individual frames of a muxed stream.

It assumes the mux field is byte 0 and values below 16. That covers every
catalogued stream and is *not* what the decoder supports — use the focus
buffer for arbitrary geometry. Do not build decode decisions on `mux_seen`.

## 6. Importing from other CAN tooling

### DBC (Vector, and most tools that export it)

DBC marks multiplexing on the `SG_` line:

```
BO_ 1000 LinkGenericDash: 8 RDM7
 SG_ FrameIndex M : 0|8@1+ (1,0) [0|15] "" Vector__XXX      ← the switch
 SG_ EngineSpeed m0 : 16|16@1+ (1,0) [0|10000] "rpm" Vector__XXX
 SG_ CoolantTemp m2 : 48|16@1+ (1,-50) [0|150] "degC" Vector__XXX
```

- `M` — this signal **is** the multiplexor. Its `bit_start|bit_length` is
  *where the frame index lives*.
- `mN` — valid only when the multiplexor reads N.
- `m3M` — extended multiplexing. Rare; `parseDbcFile()` takes the `m3` part
  and ignores the trailing `M`.

Conversion is therefore **two-pass per message**: collect the block, find the
`M`, then copy its geometry onto every `mN` signal with `mux_value = N`. It
must be two passes — the format does not require `M` to come first, and a
one-pass parser that assumes it does will silently mis-tag every signal above
it. `_dbcResolveMux()` in `main/web/index.html` does this, grouping by the
*ordinal* of the `BO_` block rather than by CAN ID so a malformed file that
repeats an ID cannot cross-contaminate two messages.

**Malformed input:** `mN` signals with no `M` in the message. There is no way
to know where the index sits, and guessing byte 0 is just a different silent
wrong answer. They are imported unmultiplexed and the message names are
reported to the user via `_dbcMuxOrphans`, so the geometry can be set by hand.

**Exporting back to DBC:** DBC needs a switch signal that RDM does not store.
`_presetToDbc()` re-emits the original switch when one is flagged
(`is_mux_switch`, set during import) and only synthesises a `MUX_<id>` when
there isn't one. Without that flag every export→import cycle would add another
synthetic switch and the signal list would grow without bound. If you add a new
export target, preserve the flag.

### Link G4+/G4X (PCLink)

Two streams, and they are **not** two framings of the same data:

| | Generic Dash | AiM Stream |
|---|---|---|
| IDs | `0x3E8` — one | `0x5F0`-`0x5FF` — 16 consecutive |
| Multiplexed | **Yes**, byte 0, 14 frames | No |
| Geometry | `mux_bit_start 0, mux_bit_length 8` | n/a |

Both are re-basable — the tuner sets the base ID in PCLink, so never hardcode
`0x3E8`. A car running two dashes off one bus moves one of the streams.

A tuner can also build a fully user-defined transmit stream in PCLink. Those
are normally *not* multiplexed (one ID per group of channels), which makes them
the easiest thing to bind: plain custom channels, no gate.

### AiM, MoTeC, Haltech

The AiM CAN protocol and MoTeC's standard sets are not multiplexed — plain
consecutive IDs with fixed word offsets. Haltech's broadcast set likewise. If
you are importing from these and reach for the mux fields, re-check: you
probably want `mux_bit_length = 0`.

### Anything else

The conversion is always the same three questions:

1. **Where is the frame index?** → `mux_bit_start`, `mux_bit_length`
2. **Which index does this signal belong to?** → `mux_value`
3. **Is the index read with the same endianness as the signal?** RDM assumes
   yes. If the source format says otherwise, you must normalise at import —
   the runtime has one endian per signal and uses it for both.

Formats that describe the mux as a *byte offset* rather than a bit offset:
`mux_bit_start = byte_index * 8`, `mux_bit_length = 8`.

## 7. Adding a new authoring surface — the checklist

Mux is carried by hand at every layer; nothing generates it. A surface that
forgets one link produces the convincing-garbage failure from §1, so work
through all of these:

- [ ] **Draft/defaults** include `mux_bit_start`, `mux_bit_length`, `mux_value`
      (zeroed).
- [ ] **Duplicate / copy-from-existing** carries them across. A duplicated
      muxed channel that loses its gate is the easiest way to ship this bug.
- [ ] **Form inputs** exist, behind a disclosure (see §8).
- [ ] **Submit payload** spreads them in defaults-only:
      `...(d.mux_bit_length ? { mux_bit_start, mux_bit_length, mux_value } : {})`
- [ ] **Any "rebuild the whole list and re-save" path** preserves them. In the
      web editor this is `_presetSigFields()` — one helper, three call sites.
      Field-picking rebuilds are how `unit` got silently dropped for a while;
      add new persisted fields to the helper, never at the call sites.
- [ ] **Live-register path** (`/api/signal/update`) sends them, or the signal
      decodes ungated until the next layout reload.
- [ ] **Live preview** gates on the mux value (§5) — a preview that decodes
      whatever frame arrived last actively misleads.

Existing surfaces, for reference:

| Surface | Where |
|---|---|
| New custom channel | `_chNewDefaults` / `_renderChNewForm` / `_chCreateSubmit` |
| Custom preset signal | `cusPreSig*` / `saveCustomSignal` |
| Legacy custom signal | `customSig*` / `applyCustomSignal` |
| DBC import | `parseDbcFile` / `_dbcResolveMux` |
| Preset application | the `preset.mux_bit_length ? {...}` spreads |

## 8. UI guidance

Fold it away. Most ECUs are not multiplexed, and three bit-level numbers on the
main decode form read as required setup rather than the rare special case they
are. Both editors use a `<details>` disclosure that auto-opens when the channel
is already multiplexed.

Label for humans, not for the spec — "Index bits", "Index at bit", "This
frame", not "mux_bit_length". The one-line explanation that tests well is:

> Some ECUs send several different payloads through one CAN ID and tag each
> with a frame index. Turn this on and the channel only decodes frames carrying
> the index you name.

And when the entered index has not arrived, say exactly that plus which indices
*have* — never fall back to decoding a different frame.

## 9. Debugging

**Symptom: a channel reads plausible values that jump between unrelated
ranges.** Almost always an ungated muxed ID. Check
`GET /api/channels` → the channel's `decode` for `mux_bit_length`, then
`GET /api/can/monitor` → that ID's `mux_seen`. Several bits set with no gate on
the channel is the diagnosis.

**Symptom: a channel reads nothing at all.** Either `mux_value` names an index
the ECU never sends (check the `focus` block's `frames[].mux` list), or
`mux_bit_start`/`mux_bit_length` point at the wrong field so every frame's
computed index misses.

**Symptom: it worked, then stopped after an edit.** Something on the save path
dropped the gate — walk §7's checklist for the surface you used.

**Symptom: preview disagrees with the running dash.** The preview must extract
the mux field with the signal's endian, unsigned (§2). A preview using a fixed
endianness will diverge on big-endian signals.

## 10. Related

- [`docs/adr/0005-channel-owned-decode.md`](adr/0005-channel-owned-decode.md) —
  why decode lives on the channel and not in the layout
- [`docs/handover/04-signal-and-can.md`](handover/04-signal-and-can.md) —
  the frame→widget path this gate sits in
- `main/layout/ecu_presets.c` — the Link Generic Dash rows, the worked example
