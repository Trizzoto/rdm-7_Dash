# ADR-0017: One channel list, two ticks — Log and Graph

Date: 2026-07-30
Status: Accepted
Repo: rdm7-desktop (GPS workspace)

## Context

There are exactly two questions anyone asks about a telemetry channel:

1. **Does the puck record it?** Costs storage (2 bytes a sample), needs a Send,
   and clears the puck's ring because the record changes size.
2. **Is it drawn in the graph?** Free, instant, and reversible.

Studio answered them on different screens, in different shapes, and answered
the second one badly:

- **Logging** was a dense wrap of pill *chips* in Setup — fine for picking from
  ninety, but not a list of channels, and it said nothing about the GPS
  channels (which are always recorded and so never appeared at all).
- **Graphing** was a single button in the playbar that toggled `gp.lanesOpen`:
  show *every* empty lane, or *none* of them. There was no way to hide Yaw
  rate, no way to look at speed against throttle alone, and no way to see one
  empty lane without seeing all four. Combined mode (every channel on one set
  of axes) is unreadable at nine traces and was effectively unusable because of
  it.

Nothing on either surface was a checkbox, and neither surface mentioned the
other.

## Decision

**One component — a ruled table, one row per channel, two tick columns** —
mounted in two places with identical behaviour:

| Channel | Log | Graph |
|---|---|---|

- **Log** is tickable only for CAN channels. The puck's own block reads as a
  muted dashed tick ("always recorded — the fixed 12-byte record"), Delta as a
  dash ("worked out here from two laps"), and a channel that cannot be logged
  as a dash with the reason (wider than the 16-bit slot; no CAN decode). These
  states are **printed, never blank**: an empty tickbox reads as "off, click
  me", which is the opposite of both truths.
- **Graph** is tickable for anything that has a lane, including empty ones —
  showing an empty lane displays its name and what it is waiting on, which was
  the only thing the old all-or-nothing button was good for.
- Mounted in **Setup → Channels** (configuration lives there) and in a
  **popover from the "Channels · n/m" button over the rack** (that is where
  your mouse is while you read traces). Same markup, same handlers, so "which
  screen was it on" has no wrong answer.
- Grouped **From the puck / Worked out here / From the car's bus / Nothing
  recording these**, with a search box once the list passes fourteen rows.
- The three summary rows (chosen count, resulting recording time, what the puck
  actually holds) are kept, and **Send only appears when there is a difference
  to push** — it is the one destructive control on the surface.

**Visibility is stored as explicit deviations, not as a visible-set.**
`gp.laneShow` holds a decision only where it differs from the default (a
channel with data is drawn; an empty one is not). Two consequences fall out
for free:

- A channel that starts arriving tomorrow appears by itself. Nothing to tick,
  and no stale entry to clean up when the selection changes.
- Hiding an *empty* lane reverts to automatic (the default already hides it),
  so it returns the day it carries data. Hiding a lane *with* data is a real
  opinion and is kept. Both readings of "hide this" are satisfied by one rule.

`gp.lanesOpen` and its localStorage key are gone. The rack's cache key now
takes a signature of the drawn lanes (`gpLaneSig`) rather than that boolean.

## The ordering rule, learned the hard way

Rows are ordered by **fixed sources** — `GP_LANES` order for the puck's block,
the **dash's own list order** for the car's channels — and never by whether a
channel is currently a lane.

The first cut ordered the car's group by "is it a lane yet", which put a channel
at the top of its group the instant Log was ticked. The list re-drew under the
cursor, and clicking three boxes in a row produced **one** tick, because clicks
two and three landed on rows that had moved. Found by doing it. A checkbox list
must not move while it is being ticked; the harness now pins this.

## Consequences

- Combined plot mode becomes usable: pick three channels, see three traces.
- The Setup rail's group nav was matched to the groups **by position** and had
  never listed Channels, so "CAN bus" and everything below it scrolled to the
  wrong card. Fixed as part of this (nine groups, nine entries).
- `GP_LANES`'s roll/pitch label held `&amp;`, which the rack draws with
  `fillText` — it would have printed the raw entity on the canvas. Now plain
  text, escaped at the one place that puts it in HTML.
- One surface can now be wrong in two places at once if a future change edits
  only one mount. It cannot: there is one function.

## Verification

180 harness checks (`tools/check_autotrack.js`), including: default visibility
by channel kind, the deviations-only storage rule in both directions, row
grouping, every Log-column state and its stated reason, a recording carrying a
channel that is no longer chosen, Send appearing only on a difference, and
order stability across ticking Log and hiding a lane.

Live in the running app against the real node: ticks in both columns changing
the rack immediately, lanes regrowing, the count on the button following,
persistence across a reload, the Setup and popover mounts agreeing, Esc and
outside-click closing the popover, and — with a twenty-channel stub dash
registered — the search box, the cost accounting (4 channels → 8 bytes a
sample → 366 min becomes 219), and three consecutive ticks landing on the three
intended rows.
