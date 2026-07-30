# ADR-0019: Splits you name — the name belongs to the gate that opens the stretch

Date: 2026-07-30
Status: Accepted
Repo: rdm7-desktop (GPS workspace). Stage 5 of
`docs/LAP_ANALYSIS_REDESIGN_2026-07.md`, and the last of its stages.

## Context

Sector times were anonymous. "You lost 0.3 s in sector 2" is a fact you have
to translate before you can act on it — go and find which third of the lap
that was, remember what is in it, then decide what to do. "You lost 0.3 s
through the Esses" is an instruction. Same number.

The plan called for splits that can be "placed, merged, divided, named and
typed". Placing, merging and dividing already worked. This is the naming.

## Decision

**A name belongs to the GATE THAT OPENS the stretch** — including the
start/finish line, which opens the first one. `gpSectorGates()` is therefore
`[start_finish, ...sectors]`, exactly one longer than `sectors`, which is
exactly the number of stretches.

That one choice makes every edit correct with no index arithmetic anywhere:

- **Insert** a split inside a stretch and the first half keeps its name (its
  opening gate has not moved), while the second half begins at the new gate
  and is unnamed.
- **Delete** a gate and its stretch merges into the one before, which keeps
  its own name; the deleted gate takes its name with it.
- **Reorder** the gates and the names travel with them, because they *are*
  the gates.

The obvious alternative — an array of names on the track, indexed by sector
position — needs all three of those handled by hand, and would silently
relabel someone's track the first time one was missed.

**Gates are now sorted into the order the car crosses them.** The array held
them in the order they were *added*: drop a split at the far side of the
circuit and it was still "Split 1" everywhere a person reads, while the
timing — which already sorts crossings (ADR-0017 era fix) — disagreed. That
was survivable while the labels were "S1/S2/S3" and wrong only in position;
it is not survivable once "Split 1" carries a name someone chose. So
`gpSortSectors()` reorders them whenever a lap exists to define the order:
after a split, and after a gate is dragged. It preserves the current
selection (the index would otherwise point at whichever gate slid into the
slot mid-edit), and refuses to guess when a gate was not crossed or no lap is
loaded.

**Where names appear:** the Tracks inspector (a *Sectors* list, and a name
field on the selected gate), a legend above the split-times grid, the sector
chips' tooltips in the lap list, and the coach findings.

**Not done: typing splits** (corner entry / corner exit / straight, as Race
Studio has). The corner-phase attribution already classifies braking, entry,
apex and exit automatically per corner (ADR-0012), so a hand-set type would
be a second, manually-maintained opinion about the same thing — and one that
goes stale the moment a gate moves.

## Consequences

- Names ride along in the track sent to the node and are ignored there (the
  node's `lap_line_t` has no name field, and cJSON ignores unknown keys). At
  ~24 characters per gate against an 8 KB payload cap, this is not close to a
  problem. It does mean **Read from the node** produces an unnamed track,
  which is honest: the node genuinely does not have them.
- `gpLinesAgree` compares geometry only, so renaming a sector does not make
  the readiness panel report the node's copy as out of date. Correct — the
  node's timing is unaffected by a name.
- Names are per-track in Studio's library, so they follow the track rather
  than the recording, and every past session of that circuit gains them.

## The design correction made mid-build

The names went into the split-times **column headers** first. That was wrong
on this surface and the running app said so immediately: the Session rail is
about 290 px, three names plus a Total do not fit, and the Total column —
the number people actually read — was pushed off the edge behind a
horizontal scrollbar. Headers went back to S1/S2/S3, with the names stated
in full, once, in a legend directly above the grid. The grid stays a grid of
numbers; each header still carries its name in its tooltip.

The same correction, smaller, in the coach card: the name was appended to the
title with a separator, and the titles already run to two lines. It is now
its own muted line under the title, reading as a place rather than as part of
the finding.

## Verification

286 harness checks (`tools/check_autotrack.js`), including: one opening gate
per stretch; the numeric fallback and what counts as named (whitespace does
not); start/finish naming the first stretch; the insert / delete / reorder
behaviours above asserted directly against the model; `gpSortSectors`
reordering back-to-front gates, carrying names with them, preserving the
selection, being idempotent, and refusing when there is no lap or a gate was
missed; `gpSectorOfSample` placing samples either side of both splits and
returning null with no splits; and the grid showing a legend only for names
that exist while keeping short headers.

Live in the running app: named three stretches of the fixture track from the
Tracks inspector, watched the Sectors list update as they were typed, saw the
legend and the tooltips appear in the split-times grid, saw a coach finding
name where it happened, and reloaded the app to confirm the names came back.
