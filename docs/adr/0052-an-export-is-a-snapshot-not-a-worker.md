# ADR-0052: An export is a snapshot, not a worker

Date: 2026-09-01
Status: Proposed — decided 2026-09-01, not yet built
Repos: rdm7-desktop (GPS workspace, video export)
Plan: `rdm7-desktop/docs/briefs/06-background-exports.md`.

## Context

A forty-second video export owns the app for its duration. The obvious reading
is that the work is on the main thread and needs moving off it, and the obvious
fix is a Worker with `OffscreenCanvas`. That reading is wrong, and it is worth
writing down why, because it will be proposed again.

**The export already runs in the background.** Decode and encode are already
off-main-thread inside Chromium. Backpressure is driven by the codecs' own
`dequeue` events with a 250 ms backstop — explicitly not timers, because timers
clamp to 1 s in an unfocused window. That was done deliberately and it worked:
**31.2 s of 1080×1920 footage exported in ~18 s in a throttled, hidden pane.**

What actually stops you using the app is three other things:

1. **A full-screen modal.** Both the options dialog and the progress dialog are
   `.gp-expdlg` — `position: fixed; inset: 0; z-index: 17000`. Every pointer
   event in the app is blocked for the duration, and the only control is Stop.
2. **Shared mutable state, read live, every frame.** `gpHudRender` reads
   `gp.trace`, `gp.selLap`, `gp.cam.hud` and `gp.ghostFence` out of closure
   scope as it paints. Re-select a lap or nudge a widget mid-export and the
   output file changes halfway through. The modal prevents that by construction
   today — so removing the modal without fixing this trades a blocked UI for
   corrupted exports.
3. **The slow path needs the visible `<video>` element.** It records the
   playing element through `captureStream` in real time and dies when the
   window is hidden; a 6 s stall watchdog aborts it.

A Worker would not have fixed any of those, and would have cost the one rule
this subsystem has. `gpHudRender` is the tile, the overlay designer and the
export; `check_hud.js` pins it pixel-identical at two sizes. Porting it into a
worker means forking or rewriting it, and everything it leans on is
document-scoped:

- **Fonts drive layout, not just glyphs.** The HUD's faces come from a document
  `@import`, and `measureText` on the speed number feeds the gear's x position.
  A worker's `OffscreenCanvas` does not see document fonts, so the *geometry*
  moves, not the typeface.
- `HTMLImageElement` does not exist in a worker — the mark and the map tile
  cache both use `new Image()`, and a tile's `onload` reaches back into the DOM.
- The overlay layout's only home is `localStorage`.
- `__TAURI_INTERNALS__.invoke` is main-thread, so `read_file_range` — the
  entire source of video bytes — would need proxying back anyway.

CSP would have permitted it (`default-src` carries `'self'` and `blob:`, and
there is no `worker-src`), and with no bundler it would have shipped as a plain
file on `merge_overlay.py`'s asset list. The cost was never CSP.

## Decision

**Background export is a modal, a state snapshot, and a queue. Not a worker.**

1. **A job captures a snapshot at enqueue, and the frame loop borrows the app's
   state one frame at a time.** `gpHudRender` and `gpHudData` are not
   refactored to take a context; a `gpWithSnapshot(snap, fn)` saves the live
   `gp.*` fields the HUD reads, assigns the job's, calls, and restores in a
   `finally`. Synchronous, symmetrical, testable on its own — and the
   pixel-identity contract is untouched.

2. **`trace` is reference-captured, `cam` is deep-cloned.** The rows array is
   hundreds of thousands of entries and is *replaced* by `gpSessionLoad`, never
   mutated in place, so holding the old array is both cheap and correct. The
   camera settings are small and are edited in place by the designer.

3. **Options stay modal; progress does not.** The options dialog is a decision
   being made, briefly. Progress becomes a non-modal strip — one row per job,
   name, bar, Cancel — keeping the existing ≥120 ms update throttle and the
   reason for it: *the bar is not the job.*

4. **One job at a time.** Two concurrent WebCodecs pipelines contend for the
   same hardware encoder and both get slower. The queue exists so you do not
   wait to press the button again, not to run exports in parallel.

5. **The slow path stays foreground and modal.** It cannot be otherwise. A
   queued job that falls back to it raises the modal for its turn and keeps the
   "keep the window on top" warning — and is marked as needing the foreground at
   enqueue, where `gpFastAvailable()` already knows the answer.

6. **The destination is chosen up front.** Today the save dialog appears when
   the export finishes. In a queue nobody is watching, and a finished job would
   hold its whole MP4 in memory waiting for a dialog. The save is one buffer end
   to end — `Blob` → `arrayBuffer()` → raw IPC body → `std::fs::write` — so a
   queue of finished blobs is not acceptable, and writing on completion is the
   whole fix.

7. **Visible means yield.** When the window is visible the composite is held to
   a budget so the app stays usable; hidden, it runs flat out. Implemented by
   lowering the existing dequeue-event queue-depth threshold — **never with
   `setTimeout`**, which clamps to 1 s unfocused. That lesson is already in the
   code.

8. **Tiles are pre-warmed before a job starts** when the minimap ground is
   satellite. Tiles arrive asynchronously through an image cache; the modal
   makes a cold cache unlikely today, and a job started minutes later would
   otherwise export frames with a blank ground.

## Consequences

- The scope shrinks. This was ranked as the architecturally riskiest of eight
  features; it becomes a dialog change, a save/restore helper and a FIFO.
- `gpHudRender` is not forked, so the tile, the designer and the export cannot
  drift apart — which was the actual risk in the worker plan.
- The corruption case the modal used to prevent by construction now has to be
  prevented by the snapshot, and that is the check the whole design turns on: a
  job's frames must be identical whether or not the lap selection and the
  overlay layout change between frames.
- Per-frame main-thread occupancy with the app *visible* has never been
  measured — only throughput hidden and throttled. The yield budget must be
  picked from an instrumented number, not from taste.
- Files WebCodecs cannot take still export in real time, still modally, still
  needing the window on top. Real time is a fallback, not the design, and this
  does not change that.
- A worker becomes the right answer only if `gpHudRender` ever stops depending
  on document fonts and images. It does not today, and making it stop would be
  a bigger change than the one this ADR describes.

## References
- Code: `rdm7-desktop/src/tauri-overlay.html` — `gpExportFast` (the pump and its
  dequeue-event backpressure), `gpExportSlow`, `gpExportProgress`, the
  `.gp-expdlg` CSS, `gpHudRender`/`gpHudData`, `gpSaveVideoBlob`;
  `rdm7-desktop/src-tauri/src/lib.rs` — `write_binary_file`, `read_file_range`
- Tests: `rdm7-desktop/tools/check_export.js`, `check_hud.js` (pixel identity —
  the proof the renderer was not forked)
- Evidence: `rdm7-desktop/docs/VIDEO_HUD_EXPORT_2026-08.md` — the 31.2 s / ~18 s
  hidden-pane measurement and the IPC numbers behind commit `5dfa9ac`
- Related ADRs: 0007 (no bundler — a worker ships as a plain asset), 0043
