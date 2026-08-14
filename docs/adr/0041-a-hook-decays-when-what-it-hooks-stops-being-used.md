# ADR-0041: A hook decays when the thing it hooks stops being used

Date: 2026-08-14
Status: Accepted
Repos: rdm7-desktop (`src/tauri-overlay.html`)
Reported by a customer on firmware 1.2: *"on offline mode, the screen
doesn't update when you move a widget. Even when clicking save or sync. I
have to go to a different dashboard then back again."*

## What "the screen" is

Not the dash. RDM Studio's editor canvas is backed by a **WASM build of the
firmware's own LVGL renderer** — the offline preview. Connected, it mirrors
the dash's signal values; in Offline (Local) mode it is the only picture of
the layout there is. It only ever repaints when somebody hands it a layout
via `load_layout_json`.

Three overlay blocks did that handing: `hook-trigger-preview`,
`hook-switch-wasm` (layout switch) and `hook-sync-wasm` (device sync). The
first is the one that mattered for editing, and it fires from
`triggerPreview()` — *re-push the whole layout*.

## Why it stopped firing

`triggerPreview()` was, when that hook was written, how every edit reached
the device. It isn't any more. The live-edit fast paths (added to stop a
full teardown/rebuild on every nudge, which reloaded background images and
fuzzed the LCD) changed a move, a resize and most inspector edits into a
**patch of one live widget** — `/api/widget/transform`, `/api/widget/set` —
that never re-pushes the layout at all.

So the hook kept working perfectly for everything except the edit people
make most often. Nothing broke it; the code underneath simply moved out from
under it, and there is no test that would notice.

The editor's own safety net doesn't cover it either. `_doWidgetTransform`
falls back to a full `triggerPreview()` when the device replies
`found: false` — "that widget isn't on my screen, rebuild everything". But
offline the local router's catch-all answers `200 {ok:true}`, carrying no
`found` at all, so the patch reads as **applied to a device that isn't
there**. Both routes to a repaint were closed at once.

That is the whole reported symptom, line by line: a move repaints nothing;
save and sync were never hooked; and switching dashboards is
`hook-switch-wasm`, which still fires. The workaround people found is the
one code path left standing.

## Decision

Hook `_flushLiveEdits()` as well.

It is the single choke point for both fast paths — transforms and field sets
drain through it — and it is already coalesced on an 80 ms trailing
debounce, so one call there covers drag, resize and the inspector without
adding a rebuild per pointer event.

The rule this restores is worth stating plainly, because it is what got
lost: **every way an edit reaches the dash must also reach the WASM.** There
are exactly two — re-push the layout, or patch a widget — and now both are
hooked.

## What was not done

**The local router still answers `{ok:true}` for `/api/widget/*`.** Making
it honest (`found: false`) would route offline edits through the existing
`triggerPreview()` fallback and fix this too, and it would arguably be
truer — the same "sure, fine" lie the router already corrects for
`/api/channels`. It was left alone deliberately: with `_flushLiveEdits`
hooked, that path adds a redundant full-layout push and a second rebuild per
nudge, to reach a repaint that has already happened. The honest-router
change is worth making on its own merits, not as this bug's fix.

**The firmware web editor needed nothing.** It has no WASM engine; offline
it draws the SVG approximation layer, and `renderEditor()` — which runs on
every drag step — calls `renderPreview()` directly. The staleness is
specific to the surface where an accurate renderer replaced that layer.

## Consequences

- Connected Studio also stops showing stale geometry. The same hole was
  there whenever a dash was attached; the real dash updating made it look
  like only the desktop preview lagged behind by a beat.
- The customer's workaround still works, and now nobody needs it.
- Anyone adding a third way to push an edit has to hook it. That is a real
  cost of the overlay's anchored-block approach: `merge_overlay.py` detects
  when an anchor stops *matching*, but nothing detects when an anchor still
  matches a function that stopped being called.

## Verification

Against the merged `src/dist` build in a browser, in Offline (Local) mode
with the real WASM engine, with the Tauri fetch interceptor installed by
hand so the local router answers exactly as it does in the app —
`/api/widget/transform` → `200 {ok:true}`, confirmed before testing.

Counting `load_layout_json` calls into the engine:

| Action | before | after |
|---|---|---|
| Move a widget | **0** | 1 |
| Resize a widget | 0 | 1 |
| Inspector field edit | 0 | 1 |
| Save | **0** | — |
| Sync | **0** | — |
| Switch layout away / back | 1 / 1 | 1 / 1 |

The three zeros are the bug, and the two that stayed 1 are why the
workaround worked.

Coalescing holds: 40 transform pushes at 8 ms intervals produced **3**
rebuilds, not 40. And visually — the rendered meter moved from the left of
the canvas to the right, with the selection box tracking it, where before
the box moved alone and the render stayed put.
