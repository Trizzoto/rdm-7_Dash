# v2 Cleanup Log

**Not kept up to date.** This was meant to be a running log of stale code
removed during the v2 channel architecture rollout (`docs/v2_channel_architecture.md`,
now shipped — see that file). The "Landed" section below was never
populated even though the rollout clearly happened (channels are live in
`main/data/`, referenced across most widgets, and described as current
architecture in `CLAUDE.md`). Treat the table below as a record of *original
intent* only, not of what actually got removed — for that, use
`git log -- main/data/ main/widgets/` or check the current widget headers
directly against the "expected change" column.

Format: each entry is `YYYY-MM-DD | phase | file | change | reason`.

## Pending (planned during planning phase)

These are candidates identified during planning. Confirmed and removed as
each phase lands.

| File / Area | Expected change | Phase |
|---|---|---|
| widget_meter.c — min/max/threshold defaults | Move to canonical channel | 2 |
| widget_bar.c — min/max/threshold defaults | Move to canonical channel | 2 |
| widget_panel.c — threshold logic | Channel event subscription | 2 |
| widget_rpm_bar.c — redline + limiter | Channel high_warn / high_critical events | 2 |
| widget_arc.c — range fields | Channel-derived | 2 |
| widget_warning.c — alert thresholds | Channel events | 2 |
| widget_indicator.c — signal-based active | Channel boolean events | 2 |
| widget_text.c — signal binding | Channel binding | 2 |
| Inspector min/max/threshold fields (web UI) | Show as "from channel" badge | 2 |
| ecu_presets.c — signal-only seeding | Extend to seed channels | 3 |
| widget rules per-widget eval | Channel-level threshold-crossed events where possible | 5 |
| Layout JSON size | ~30% reduction expected from omitting channel-owned fields | 5 |
| `data_logger.c` — log all signals | Channel-based logging (channel display values, channel labels) | 5 |
| `signal_replay.c` — replay raw signals | Optionally surface as channel injection | 5 |

## Landed

*(never populated, despite the rollout landing — see the note at the top of
this file. Don't read the empty list below as "nothing was cleaned up.")*

---

## Performance opportunities tracked

| Opportunity | Status | Notes |
|---|---|---|
| Shared threshold evaluation across widgets | Phase 2 | Today each widget evaluates same threshold per signal callback. Channel event = one eval, N notifications. |
| Channel staleness as single check | Phase 2 | Today each widget tracks signal staleness; channel-level fan-out cuts work |
| Layout JSON budget freed | Phase 2 | Per-widget min/max/threshold removed; channel-owned fields cut ~80 bytes per widget |
| Inspector render time | Phase 5 | "From channel" mode shrinks inspector visible fields |
| Signal subscription cleanup on layout reload | Phase 1 | Existing path keeps; verify no listener leaks via the channel manager's subscribe/unsubscribe symmetry |

---

## Removal checklist per file (run at phase end)

Each widget that gets a channel binding should also have its old:
- [ ] `min` / `max` / `threshold` fields removed from header (if channel-only)
- [ ] `to_json` / `from_json` no longer emit/parse those fields (defaults-only emit means dropping them entirely once channel is the source of truth)
- [ ] Inspector get/set no longer surfaces them
- [ ] Widget rules referencing channel-owned fields rerouted to channel events
- [ ] Web UI WIDGET_DEFS field list pruned

---

*Updated as each removal lands. Cross-reference commit SHA + phase number.*
