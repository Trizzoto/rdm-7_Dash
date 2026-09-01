# ADR-0054: `prompt()` does not come back — the editor asks with its own overlay

Date: 2026-09-02
Status: Accepted
Repos: RDM-7_Dash (`main/web/index.html`), rdm7-desktop (consumes it verbatim per ADR-0007)

## Context

ADR-0016 dealt with `window.confirm` and left `window.prompt` alone, noting
only that the desktop webview "does not implement it". That understated it by
a long way.

Measured on 2026-09-02 against the dev build, driven over CDP:

- `String(window.confirm)` is the dialog plugin's shim, as ADR-0016 found:
  `async function(i){return await n("plugin:dialog|confirm",…)}`.
- `String(window.alert)` is the plugin's too — fire-and-forget, so the code
  after it keeps running, but something does appear.
- **`String(window.prompt)` is `function prompt() { [native code] }`** — the
  webview's own. The plugin never shimmed it, and WebView2 does not implement
  it.

Calling it does not return `null`. It does not return at all. After a
`prompt()` call the page would no longer evaluate `1 + 1`; the script thread
never resumed. `EnumWindows` over the process found one `Tauri Window` and
three hidden IME helpers — **no dialog of any kind had been created**. The
editor freezes exactly where it stands, mid-click, and the only way out is
Task Manager.

Two shipped controls did this, both of them ordinary things to click:

| Control | Function |
|---|---|
| Layout menu → **New dashboard…** | `layoutEditorNew` (the ECU-aware override) |
| Channels → a channel's unit list → **Custom…** | `_chUnitSelect` |

The first is the primary way to make a layout. On the device's own web editor
both work fine — `prompt()` is a real dialog in a real browser — which is why
the bug survived: it only exists in the desktop app, and the desktop app is
not where this file is maintained.

The editor already had `_showPromptOverlay(title, value, onConfirm, opts)`,
used by New Layout, Save As and New Splash. It had no cancel callback, so it
could not back a promise; its Escape handler was bound to the input alone, so
Escape did nothing once focus reached a button; and it addressed its own
controls by `getElementById`, so a second overlay drove the first one's input.

## Decision

The same shape as ADR-0016, one layer up:

- `_showPromptOverlay` gains `opts.onCancel` and one hardened exit path — OK,
  Cancel, backdrop **mousedown** and Escape all funnel through a single
  `close(value)` that unhooks the key listener and cannot answer twice. The
  key hook runs in the **capture phase** so the editor's own Escape (deselect,
  close the modal behind) does not also fire. It finds its input and buttons
  with `overlay.querySelector`, by class, not by document id.
- A promise form wraps it: `promptAsync(title, defaultValue, opts) →
  Promise<string|null>`, resolving `null` on every dismissal — so a converted
  call site reads like the one it replaces:
  `const name = await promptAsync(…); if (!name) return;`
- Both `prompt()` call sites are converted. The unit label asks with
  `{ raw: true }`, because the default sanitiser (`[^a-zA-Z0-9_-] → _`) turns
  `°F/min` into `__F_min`, and a unit label is not a name.
- **The overlay escapes what it is given.** Title and seed value are somebody's
  data — a layout name, a unit label — and the seed goes into an attribute.
  So does `_chUnitFieldHTML`'s free-text box and its "(custom)" option, which
  never escaped: with `Custom…` now accepting arbitrary text, a label
  containing a quote closed the attribute early and spilled the rest of itself
  into the tag as further attributes. `deg "C"/s` read back as `deg `.

`window.prompt` is not shimmed, for the same reason `window.confirm` was not:
a shim cannot restore synchronous semantics, and the overlay is plain DOM that
behaves identically on the device and under Tauri.

## Consequences

- `_chUnitSelect` and the `layoutEditorNew` override were already `async`; no
  caller changed.
- Empty is no longer a way to say "back to native" through `Custom…` — the
  overlay refuses an empty value, and the native unit is its own entry in the
  list one line above. Cancel re-renders and writes nothing, as before.
- `_showPromptOverlay`'s three existing callers (New Layout, Save As, New
  Splash) get the capture-phase Escape and the escaping for free. None of them
  passed markup in a title or subtitle, so escaping breaks nothing.
- rdm7-desktop re-syncs (`python tools/sync_firmware.py`).
- `rdm7-desktop/tools/check_controls.js` now scans the **built page**, not just
  the desktop overlay. Its old comment said the firmware base "is guarded in
  its own repo" — true, but it is guarded there for the device, where both
  dialogs work. The freeze only exists on the desktop, so the desktop is where
  it has to be noticed.

## Verification

`node --check` on the extracted editor script. In the real desktop app (dev
build, `--remote-debugging-port=9222`, isolated `WEBVIEW2_USER_DATA_FOLDER`,
Local virtual dash asserted `serial: LOCAL` inside each probe):

- **The freeze, before:** `prompt()` armed in a `setTimeout`, then `1+1`
  evaluated on the same socket — no answer in 15 s, twice; `EnumWindows`
  showed no dialog window.
- **New dashboard…, after:** the overlay appears; Escape closes it, leaves the
  ECU modal shut and `/api/layout/list` unchanged at one entry; and the probe
  returns — which is the whole point. Typing `probe "new" 1` and pressing OK
  opens the ECU modal, and *Custom (No ECU)* creates `probe__new__1` with
  0 widgets. Deleted again by the same probe.
- **Custom…, after:** the overlay appears, `°F/min` reaches `_chEdit` intact
  (stubbed to a pure counter — nothing left the app), and Escape writes
  nothing.
- **The escaping:** `_chUnitFieldHTML` lifted out of the built page and run
  both ways on `deg "C"/s`. New: the option and the input both read back
  `deg &quot;C&quot;/s`. Old: both read back `deg `.

`node tools/check_dialogs.js` in rdm7-desktop pins all of it — 19 checks, each
negative-tested by putting the original code back.

## References

- ADR-0016 (the confirm half, and the overlay this copies)
- ADR-0007 (why the desktop repo consumes this file verbatim)
