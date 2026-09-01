# ADR-0051: One layout file, drawn two ways

Date: 2026-09-01
Status: Proposed — decided 2026-09-01, not yet built
Repos: RDM-7_Dash (`main/web/index.html`), rdm7-desktop (overlay + designer)
Rests on [0007](0007-html-source-of-truth.md) (the base/overlay merge and its
anchors), [0042](0042-a-bundle-should-say-whether-it-carries-your-car.md) (the
`.rdm` container and its flavour byte) and
[0048](0048-extracted-firmware-code-keeps-the-scope-it-was-written-for.md) (why
touching `importRdm` needs care).
Plan: `rdm7-desktop/docs/briefs/03-one-layout-format.md`.

## Context

Since 2026-08-31 the video overlay and the dash editor share a widget
vocabulary. `GP_HUD_MADE` lists thirteen user-instantiable overlay types whose
**id strings and display names are the dash's, duplicated by hand**, and the
icons are not duplicated at all — `gpHudDashIcon` returns `PALETTE_ICONS[t]`,
the dash editor's own icon object, reachable because both live in the merged
`dist`.

Sharing the vocabulary makes the two editors feel like one act. Sharing the
**file** would make a layout worth twice as much: design once, use it on the
dash and on your videos — and it is what makes the Marketplace interesting,
since a shared layout would work in both places.

The obvious way to do that is to make the two surfaces share geometry. They
cannot, and the code already knows it:

- The dash is the WASM renderer at a fixed 800×480 with absolute centre-origin
  pixels.
- The overlay is hand-rolled 2D canvas at any aspect, laid out by an
  **algorithmic flow** in `S` units, and stored only as *nudges from that flow*
  — `{dx, dy, k}` per widget, with the entry deleted when it is back at
  factory. That relativity is load-bearing: it survives aspect changes and it
  survives the default layout being re-tuned.
- And the existing dash→overlay importer already refuses geometry, for a better
  reason than either: *"copying the geometry would give a HUD that covers the
  footage."* A dash layout fills its screen because that is all the screen is
  for. An overlay that filled the frame would hide the driving.

Half the converter exists and solves the hard part — the two-namespace problem
(dash widgets bind a registry `signal`, overlay widgets bind a recording
channel id), matched three ways and **reporting what it cannot match rather
than inventing it**, measured at 10 of 26 across on his own layout. But it
predates the thirteen-type vocabulary and still collapses every dash widget
into `"bar"` or `"value"`.

## Decision

**One file carries both placements. The widget list is shared; the geometry is
not.**

1. **A new `.rdm` entry type `4`, carrying the overlay placement as JSON.** The
   container is already built for this: the entry loop is an `if/else if` chain
   with no `else`, and 0042's own format comment states the contract — *"older
   importers skip unknown types gracefully."* Every existing reader, and the
   firmware, ignores it.

2. **Not a new key in the layout JSON.** Dash layouts have a hard 32 KB ceiling
   that the dash needs. Spending it on data the dash cannot use is how that
   ceiling turns into a bug report.

3. **The payload is `gp.cam.hud` plus its two siblings**, not a new schema —
   `w`/`z`/`lock`/`st`/`add`/`seq`, plus the widgets switched *off* and the
   minimap ground, which live beside `hud` on `gp.cam` rather than inside it.
   They are listed as exceptions because absent means on.

4. **The firmware parses and ignores it.** One `else if` branch in
   `main/web/index.html`, and nothing else — the dash has no HUD. Then
   `sync_firmware.py` brings it back, and `import-rdm-head`/`-tail` are
   re-anchored. A failed merge there is the drift detector working.

5. **The desktop consumes it through the existing setters** — `gpCamPut("hud",
   …)` and `gpCamSet` — never by writing `localStorage` directly. `gpCamLoad`
   rebuilds `gp.cam` key by key and silently drops anything it has not been
   told about; that trap was written down before the code was and has bitten
   once already.

6. **An imported overlay is offered, not imposed.** An overlay layout is
   hand-tuned work. `gpConfirm`, because `window.confirm` under Tauri is
   broken (0016).

7. **The dash→overlay importer stops collapsing.** One-to-one for all thirteen
   shared types, keeping the existing exclusions and keeping the reasons on
   screen: `toggle`/`button` are pressable and meaningless on video, `image`
   needs the device image store, `pathbar`/`anim` are device chrome. Widening
   the type map must not widen what it is willing to invent — the importer's
   best property is that it reports a gauge the recording cannot feed.

8. **Overlay → dash is deliberately not built.** It needs S-units → 800×480
   centre-origin pixels, expansion of the dozens of `WIDGET_DEFS` keys a made
   widget does not carry (a made widget has ~10 flat keys; a dash `meter` has
   40+), and the whole recording-channel → dash-channel adoption flow, which is
   its own planned work and must stay an explicit review-then-write with the
   signed flag riding along. Saying so here so its absence does not read as an
   oversight.

## Consequences

- A `.rdm` written by this version opens unchanged on an older Studio and on
  the dash. The overlay simply does not arrive.
- The overlay layout can leave the machine for the first time. It has only ever
  existed in one browser's `localStorage`.
- The Marketplace payload becomes worth more without any Marketplace code being
  written. It is still link-out only — `openMarketplace()` opens a URL and
  there is no upload or download anywhere.
- Colours must be read back through `firmwareToWebFormat`, not converted by
  hand: the layout JSON inside a `.rdm` is the *firmware payload*, so its
  colours are RGB565, not the editor's RGB888.
- This is the one item of the eight that touches the firmware repo, so it is
  also the one that can break the desktop by moving underneath it — which is
  exactly what 0048 is about.
- Two placements in one file means a layout can be half-wrong: good on the dash
  and cluttered on video, or the reverse. That is the honest consequence of the
  two renderers differing in kind, and it is better than one placement that is
  wrong on both.

## References
- Code: `main/web/index.html` — the `.rdm` format comment, `exportRdm`,
  `importRdm`'s entry loop; `rdm7-desktop/src/tauri-overlay.html` —
  `_processRdmBytes` and the `import-rdm-*` / `export-rdm-native` blocks,
  `GP_HUD_MADE`, `gpHudDashPlan`, `gpHudMatchChan`, `gpCamLoad`/`gpCamPut`
- Tests: `rdm7-desktop/tools/check_import.js` (unknown entry types skip
  cleanly), `tools/check_hudedit.js` (the overlay round trip)
- Docs: `rdm7-desktop/docs/VIDEO_HUD_EXPORT_2026-08.md` (the shared-vocabulary
  table and the exclusion reasons), `docs/LAYOUTS_FROM_RECORDINGS_2026-08.md`
  (the unbuilt other direction)
- Related ADRs: 0005, 0007, 0016, 0042, 0048
