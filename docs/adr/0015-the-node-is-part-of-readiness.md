# ADR-0015: The node's own state is part of readiness — and dialogs must actually ask

Date: 2026-07-30
Status: Accepted
Repos: rdm7-desktop (Studio GPS workspace), rdm-gps-node (status honesty + GNSS recovery)

## Context

A full afternoon of "why won't it time my laps" was reproduced on the bench, from
the customer's chair, and every cause found was the software knowing something
and not saying it — or claiming to ask something and not asking it.

What the walk-through found, in the order it was found:

1. **The readiness checklist graded Studio's library, not the car.** It said
   `Track ✓ / Gates ✓` when a track existed *in Studio's localStorage*. But the
   device that senses the start line is the GPS node, timing against whatever
   track sits in *its* NVS — and nothing compared the two. On the bench the node
   was holding a track literally named "New track" while the checklist showed
   "Barker Test 2 ✓✓". Driving across the start line did nothing, exactly as the
   user reported.

2. **The one button that fixes it was unreachable.** "Send to the node" lives at
   the bottom of the Tracks inspector — and the once-a-second lap poll rebuilt
   that panel with `innerHTML = h`, resetting its scroll to the top. While a
   node was connected (the only time Send works), the panel snapped up faster
   than anyone could scroll down.

3. **`window.confirm` under Tauri does not confirm.** The dialog plugin remaps
   it to an async call returning a Promise — always truthy — so every
   `if (!confirm(...)) return` guard in the app had silently stopped guarding.
   Proven live: "Node recording cleared." appeared *behind* a dialog nobody had
   answered. Cancel changed nothing. Ten destructive actions in the GPS
   workspace were affected (clear recording, replace track, clear channel
   table, factory-reset CAN, delete sessions/tracks, …).

4. **An empty download did literally nothing visible.** Press Download on a
   node with an empty ring → no message, no change. Reads as "the button is
   broken", when the truth is "the node only logs above the idle gate, and the
   car never moved".

5. **A dead receiver looked like a good fix.** The u-blox stream died mid-
   session on the bench (root cause unknown; observed: `ubx` counter and iTOW
   frozen for an hour). The serial status kept reporting the *cached* PVT —
   "3D fix, 16 sats" — with only the `link:false` flag telling the truth, and
   Studio's chip/checklist didn't read that flag. The CAN heartbeat path had
   already learned this exact lesson (it nulls the PVT when the link drops);
   the serial path had not. The GNSS task also had no recovery: it spun on
   empty reads forever, waiting for a power cycle.

## Decision

**Studio (rdm7-desktop, overlay):**

- The Session readiness checklist gains an **"On the node"** row built from the
  node's own `lap.status` (name, armed, lap number) plus a lazily-fetched,
  cached copy of `lap.track.get` for geometry comparison
  (`gpNodeTrackState()`: nolocal / checking / none / other / stale / match).
  "Stale" means the same track was edited in Studio since it was sent —
  compared gate-by-gate with float32-tolerant thresholds (`gpLinesAgree`,
  ~2 m / 3° / 1 m width). Every not-ready state carries a one-click
  **Send "…" to the node** button in place.
- The row updates on a *signature* (track name, armed, lap number), never on a
  timer — panel rebuilds fight scroll position, which is bug 2.
- `gpRenderTrackInspector` preserves `scrollTop` across rebuilds.
- `gpConfirm()` — the workspace's own promise-based modal (Esc/backdrop/Cancel
  ⇒ false, focus starts on Cancel, Enter swallowed) replaces all ten
  `window.confirm` guards. The firmware-base editor still carries ~20 broken
  `confirm()` guards under Tauri (device-file deletes among them); that repair
  belongs in the firmware repo and is tracked separately.
- An empty download says what it means and what to do
  (`gpEmptyDownloadMsg`), leaves any loaded session on screen (it used to
  clobber `gp.trace` to `[]`), and no longer re-saves the loaded session as a
  duplicate.
- The Session empty state carries its own **Download from node** button; the
  ring's state (`recording / minutes used / minutes free`) is fetched at
  connect, not first-download.
- The fix chip, readiness Fix row and hint bar all treat `link === false` as
  outranking any cached fix ("receiver quiet", not "3D fix"), with wording that
  distinguishes "hasn't started yet" from "stopped mid-session" (ubx counter).
- The dead WiFi-dash lap fallback (`/api/lap/*`, removed from the dash
  firmware 2026-07-30) is gone from `gpLapApi()`; the node is the only lap
  authority, as ADR-0008 already decided.

**Node (rdm-gps-node):**

- `method_gps_status` applies the CAN heartbeat's staleness rule: every
  PVT-derived field dies with the link that produced it (`have = s_have_pvt &&
  link`).
- `gnss_port` recovers on its own: after 5 silent seconds the GNSS task re-runs
  the boot bring-up (baud probe → configure → baud switch → configure),
  retrying every 15 s for as long as the silence lasts. Safe because the GNSS
  task is the UART's only reader.

## Consequences

- The checklist can now contradict the user's mental model out loud ("On the
  node: New track — not 'Barker Test 2'"), which is the entire point.
- One extra RPC per connect (`lap.track.get`), one per track-send. No polling
  cost: the status poll already carried the name and armed flag.
- Old-firmware nodes (stale-fix reporters) are still rendered honestly, because
  Studio checks `link` itself — belt and braces on both sides of the cable.
- `gpConfirm` is GPS-workspace-scoped. The global confirm problem (firmware
  editor under Tauri) remains open and is the top follow-up.

## Verification

135 harness checks (`tools/check_autotrack.js`), including: every
`gpNodeTrackState` transition, readiness rows for each state, send-button
presence, empty-download wording, and link-false outranking a cached fix.
Live on the bench against the real node: modal gating (Cancel cancels),
scroll preservation, the "New track ≠ Barker Test 2" catch, one-click send
flipping the row to match, and the quiet-receiver state — which occurred
naturally mid-session and was diagnosed with these very tools.
