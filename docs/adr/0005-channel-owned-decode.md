# ADR 0005 — Channel-owned CAN decode (portable layouts)

Status: Accepted (2026-06-09) — implementation in progress on `feature/widget-sys`.

## Context

A layout JSON currently mixes two concerns:

1. **Visual** (portable): widget types, positions, colours, and `channel` refs
   (canonical ids like `rpm`).
2. **Device signal config** (NOT portable): the full `signals[]` array with
   per-signal CAN decode (`can_id`, `bit_start`, `bit_length`, `scale`,
   `offset`, `endian`, `unit`), specific to the *source* dash's ECU/wiring.

The runtime signal registry — the CAN dispatch engine (`signal_dispatch_frame`)
— is populated from the active layout's `signals[]` (`_load_signals`). The
registry MERGES across layout loads and re-registration UPSERTs decode
(latest-layout-wins). `channels.json` stores channel→signal *bindings* and
display metadata but **not** the decode.

**Symptom:** sharing a layout from a configured dash A to a configured dash B
imposes A's `signals[]` decode on B (overwrites/pollutes B's registry), so
gauges misalign — "layouts have stale setups embedded."

## Decision

The **channel** owns its CAN decode. `channels.json` (device-local) becomes the
authoritative source of decode. **Layouts drop `signals[]` entirely** and carry
only visual + canonical channel references. Each dash decodes the bus via its
own channel config, so a shared layout renders against the *target* dash's
channels.

Chosen options (2026-06-09): decode moves to the device; layouts fully drop
`signals[]` (a fresh/unconfigured dash shows no data until its channels are set
up — the "configured dash" case is fully solved).

## Design

### Data model
- `channel_t` gains: `uint32_t can_id; uint8_t bit_start, bit_length; float
  scale, offset; bool is_signed; uint8_t endian; char unit[8];` (meaningful when
  the channel is CAN-sourced).
- `channels.json` schema **v2 → v3**; decode emitted defaults-only (omit when
  `can_id == 0`).

### Registry population (decode → registry)
- New `channel_manager_register_decoded_signals()`: for each channel with a
  decode, `signal_register_with_source(signal_name, can_id, …)` and subscribe
  the channel's `chm_signal_cb`. This makes the dispatch engine **channel-driven**.
- Runs at boot after `channel_manager_load_from_lfs()`, and after each layout
  load.

### Migration v2 → v3 (one-time, like the v1→v2 heal)
On the first v3 boot, after the active layout's `signals[]` are loaded into the
registry: for every CAN signal in the registry, ensure a channel owns it —
copy decode into the bound channel, or channelize it (canonical via
`ecu_signal_name_to_canonical` / `canonical_channel_find_ci`, else `custom_*`,
reusing the wizard's resolution + collision guard). Then persist v3 and stop
relying on the layout. No user action.

### Load path
`layout_manager` stops emitting `signals[]` on save and treats any embedded
`signals[]` (legacy/foreign layouts) as a no-op for authority — the channel
registry is the source of truth.

### Web editor
- `buildFirmwarePayload()` stops emitting `signals[]`; widgets bind via
  `channel` (canonical). `signal_name` becomes legacy/ignored for decode.
- The Channels-tab per-channel decode editor writes to the channel
  (`/api/channels/update` extended with decode fields + live re-register).

## Consequences

- ✅ Layouts are portable across configured dashes.
- ✅ Decode survives layout changes + reboot (device-local).
- ⚠️ A fresh/unconfigured dash shows no live data from a shared layout until its
  channels are configured (apply an ECU preset / OBD2 first).
- ⚠️ Legacy widgets bound to a raw `signal_name` with no channel are channelized
  by the migration; brand-new such bindings are discouraged (editor pushes
  channel binding).
- Internal signals (FPS, CALCULATED_GEAR) and OBD2 PIDs are unaffected — they're
  registered by `signal_internal.c` / `obd2.c`, not layouts.

## Phasing

- **A (firmware, behaviorally invisible):** decode fields + json v3 +
  `register_decoded_signals` + migration. After migration, channel decode ==
  former layout decode, so the dash renders identically.
- **B (flip authority + web):** stop emitting `signals[]` (firmware save + web
  payload); decode editor → channels; binding precedence.
