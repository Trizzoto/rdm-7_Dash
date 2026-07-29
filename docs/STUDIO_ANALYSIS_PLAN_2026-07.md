# RDM Studio — closing the analysis gaps (plan, 2026-07-29)

Seven gaps identified against the top-tier tools (see
`docs/research/2026-07-gps-laptimer-market.md` and ADR-0012). All seven are to
be built. This is the order and what each stage means.

## Why this order

Dependencies force most of it:

- **Nothing is stored.** `gp.trace` lives in memory only. History,
  cross-session comparison and video all need a session to be a persistent
  object first. So persistence is stage 1 and is not negotiable as an ordering.
- **Units cut across every readout.** Doing them before the surfaces multiply
  (history charts, export headers, video overlay text) means writing the
  formatter once instead of chasing it through three more screens.
- **Export is cheap and rides on stage 1.** A session that exists as a record
  is a session that can be written to a file.
- **The track library is independent** but makes history meaningful — a trend
  needs a track to be a trend *at*.
- **Video is its own project.** It is the heaviest lift, it needs units and
  persistence, and nothing else is blocked behind it.

| # | Stage | Blocked by | Size |
|---|---|---|---|
| 1 | Session library — recordings persist | — | M |
| 2 | Units — metric / imperial | — | S |
| 3 | Export — CSV + session file | 1 | S |
| 4 | History — per-track trend | 1 | M |
| 5 | Track library — ~100 circuits, search, community | — | M |
| 6 | Cross-session comparison — car, driver, day | 1, 4 | M |
| 7 | Video overlay — GPS-timestamp auto-sync | 1, 2 | L |

---

## Stage 1 — Session library

**The hole.** You download 370 minutes off the node into `gp.trace`, analyse
it, quit Studio, and it is gone. Re-download works only until the node's ring
wraps, after which the session is gone permanently. A $9 phone app stores
sessions.

- **Store**: IndexedDB, mirroring the existing `imageStore` IIFE
  (`rdm7_images_db`) rather than inventing a second pattern. localStorage
  cannot hold this — a full ring is ~4 MB.
- **Format**: parallel typed arrays (`Int32Array` lat/lon, `Uint16Array`
  kph/hdg, `Uint32Array` t with a sentinel for "no timestamp"), which
  structured-clone natively and cost 16 B/sample instead of a JSON object
  graph. `gpRowsPack` / `gpRowsUnpack`, round-trip tested.
- **Dating**: the node reports `utc` in `gps.status` but not `itow_ms`. Add it
  (one line in `serial_rpc.c`) so Studio gets an exact (utc, itow) anchor and
  can date every sample from its `t`. Fall back to download time when the node
  had no fix, and mark the session undated rather than guessing.
- **Auto-save on download.** No "did you remember to save" step — that is the
  behaviour that closes the hole rather than moving it.
- **UI**: a Recordings group at the top of the Session rail — name, date, lap
  count, best lap. Click to load. Rename, delete. Auto-name from track + date.

**Done when** a recording survives a Studio restart, loads back with identical
laps and identical analysis, and the store survives a node swap.

## Stage 2 — Units

One setting (metric / imperial), one formatter layer. km/h ↔ mph, m ↔ ft,
metres of distance-into-corner, gate widths, g stays g. Every readout in the
GPS workspace plus anything else already showing speed or distance. Persisted
per-PC, not per-track.

**Done when** no user-visible speed or distance is formatted without going
through the formatter, enforced by a test that greps the built HTML for the
raw suffixes.

## Stage 3 — Export

- **CSV** per lap or per session: time, distance, lat, lon, speed, heading,
  derived g/yaw/lateral, sector marks. Column headers in the active units.
- **Session file** (`.rdmsession`, JSON + packed arrays) for round-tripping
  between machines and attaching to a marketplace post later.
- Both from the Session rail, next to the recording they belong to.

## Stage 4 — History

Per-track trend across every stored session: best lap over time, consistency
(spread of lap times within a session), personal best with the date it was
set, and which corners have improved or regressed since last time. Keyed on
the track the session was split with.

The corner-level trend is the part no incumbent has — Garmin shows you the
session, not the year.

## Stage 5 — Track library

~100 seeded circuits (AU/NZ first, then popular US/EU), search, and a
community-submission path through the marketplace. Coordinates self-curated —
not derived from AiM (proprietary) or wholesale OSM (ODbL share-alike). Five
circuits is the most visible toy-vs-tool signal in the app.

## Stage 6 — Cross-session comparison

Car and driver as fields on a session, then compare any lap against any other
lap in the library — different day, different car, different driver. The
corner-phase machinery already works on two arbitrary ranges; this is mostly
making the reference picker reach outside the current recording.

## Stage 7 — Video overlay

GPS-timestamp auto-sync (every incumbent does *manual* offset alignment),
gauge rendering over the video, export. Its own project; specced when stage 6
lands.
