# ADR-0040: A backup has to stand on its own

Date: 2026-08-14
Status: Accepted
Repos: RDM-7_Dash (`main/data/channel_manager.c`)
Reported by a customer on firmware 1.2: *"I did a channel backup then went
offline and restore. They all show up but they're all unassigned."*

## What was actually wrong

Not the offline part, and not the restore. The **backup file was never
self-contained**.

`channel_to_json` emits a channel's `signal` name, and its `decode` block
only if the channel owns one. On the reporting dash — and on the bench dash
— **no channel owned a decode**: every CAN binding was a bare name, and the
bit-level decode lived in the active layout's `signals[]`. So a restore
re-installed 27 names pointing at nothing the new layout defined.

Reproduced exactly: restore a backup while a layout that doesn't define
those names is active, and 11 `can` channels come back as **8 `obd2` and 3
`unknown`**. The eight are the cruellest part — `RPM`, `COOLANT_TEMP`,
`THROTTLE` and friends match standard OBD2 PID names, so they are silently
re-homed onto OBD2 rather than reported as broken. The rest read `unknown`.
Everything "shows up"; nothing is assigned.

## Why the channels had no decode

ADR-0005 says channels own their decode, and
`channel_manager_migrate_decode_from_registry()` is the one-shot pass that
moves it off the layout. It ran. It copied nothing. It declared success:

```c
if (skipped == 0) {
    s_decode_migration_pending = false;
    s_disk_schema_version = CHM_SCHEMA_VERSION;   /* stamp v3 */
}
```

The only question it asked was whether anything had been *skipped* — never
whether anything had been *done*. Boot once with a layout carrying no CAN
signals (a track-map layout, a fresh dash, a layout whose `signals[]` were
stripped by an earlier migration) and the loop finds nothing to copy,
`skipped` is zero, and the file is stamped `schema_version: 3` — "decode
already migrated" — permanently. The gate at the top of the function is
`!s_decode_migration_pending`, so it never runs again.

From then on the channels work *only while a layout happens to define their
names*, and every backup they produce is unrestorable. The file asserts a
migration that never happened.

## Decision

**Don't declare a migration complete when it had nothing to migrate.** The
completion test gains `can_seen > 0`. Copying nothing because the registry
was empty is "not yet", not "done", and it retries on the next layout load.

**Adopt decode wherever it is missing, on every layout load, whatever the
file version says.** A channel bound to a registered CAN signal but holding
no decode of its own is the broken state regardless of how it got there —
including the dashes already stamped v3 in the field, which the version gate
would otherwise never revisit. The top-up pass walks the live channels, not
the registry, so it only fills gaps: it never invents channels (that stays a
migration behaviour) and never clobbers decode a channel already owns.

Cheap enough to not think about: a few hundred string compares once per
layout load, and it writes only when it changed something.

## Consequences

- Existing dashes heal themselves on the next boot after this firmware,
  provided a layout defines their signals. Nothing to run, nothing to ask
  the customer to do.
- Backups become genuinely portable — the decode travels with the channel,
  which is what ADR-0005 promised and what makes a `.rdm` bundle carry a
  working setup to another dash.
- A dash whose layout never defined the signals still cannot invent decode
  it never had. Those channels stay unbound and say so, which is honest;
  re-applying the ECU preset gives them names and decode again.

## Verification

On the bench dash, before the fix: 0 channels owning a decode; restore onto
a signal-less layout gave `8 obd2, 3 unknown, 0 can`.

After: the layout load adopted decode for 11 channels
(`rpm` 0x520, `coolant_temp` 0x530, `vehicle_speed` 0x522 …); the export
went from 0 decode blocks to 11; and the identical restore onto the same
signal-less layout returned **11 `can`, 11 decodes**, unchanged from before
the restore.

Online restore was verified separately and was never the fault — 27 bound
before, 27 bound after, byte-identical.

## An aside worth keeping

Switching the active layout away and back **stripped the default layout's
`signals[]` on disk** (20 → 1) as a side effect of the ADR-0005 migration
re-saving it. With the channels also holding no decode, that destroyed the
decode entirely — it existed in neither place. Re-applying the ECU preset
restored it. That is a second reason the decode belongs on the channel and
not in the layout, and it is now the only place it needs to be.
