# ADR-0016: Cancel must cancel — the editor asks with its own overlay

Date: 2026-07-30
Status: Accepted
Repos: RDM-7_Dash (`main/web/index.html`), rdm7-desktop (consumes it verbatim per ADR-0007)

## Context

ADR-0015 proved, live on the bench, that `window.confirm` under the Tauri
desktop webview does not confirm: the dialog plugin remaps it to an **async**
function, and a Promise is always truthy, so every `if (!confirm(...)) return`
guard silently stopped guarding. Cancel changed nothing; the destructive action
ran while the dialog was still on screen. The GPS workspace fixed its ten
guards with its own `gpConfirm()` and named the firmware editor's remaining
guards as the top follow-up.

The editor had **19** such guards, and they sit in front of exactly the actions
a user would want to back out of: delete image / font / layout from device
storage or SD card, delete ECU presets and their signals, delete a signal bound
to widgets, overwrite another layout on save, apply an ECU preset over the
current decode, delete custom channels and PIDs, reset a channel, delete log
files, and both fuel-calibration writes.

The editor also already had a styled dialog, `_showConfirmOverlay(title,
message, confirmLabel, onConfirm)` — used by splash delete, layout delete and
the OTA install — but it had no cancel callback, so it could gate nothing that
needed a "no" answer, and its Escape/OK paths leaked the keydown hook.

## Decision

Do not touch `window.confirm` (shimming it can't restore synchronous
semantics anyway). Instead:

- `_showConfirmOverlay` gains `onCancel` and `cancelLabel` parameters and one
  hardened exit path: Confirm, Cancel, backdrop **mousedown** and Escape all
  funnel through a single `close(confirmed)` that unhooks the key listener and
  cannot fire a callback twice. The key hook runs in the **capture phase**:
  Escape cancels without also firing the editor's own Escape handlers, and
  Enter is swallowed because the button that opened the overlay may still hold
  focus — Enter would "click" it again straight through the dialog. Focus
  starts on Cancel. Message newlines render as `<br>`. (All of this mirrors
  the desktop `gpConfirm()`, ADR-0015.)
- A promise form wraps it:
  `confirmAsync(title, message, confirmLabel, cancelLabel) → Promise<boolean>`,
  so a converted guard reads like the one it replaces:
  `if (!(await confirmAsync(...))) return;`
- All 19 `confirm()` call sites are converted to `confirmAsync`, each with a
  real title and a verb on the red button ("Delete", "Overwrite", "Apply",
  "Set Empty" …) instead of OK.
- The save-time factory-protection chooser (`_resolveProtectedName`), which
  abused OK/Cancel as a two-way choice ("OK = overwrite, Cancel = save as
  default_modified2"), now labels the buttons with the actual choices:
  red **Overwrite** vs **Save as "…"**. Escape/backdrop takes the safe branch
  (the fresh name), exactly as Cancel did before.

This works identically on the device and under Tauri because the overlay is
plain DOM — no environment detection, no plugin.

## Consequences

- Three formerly-sync functions became `async` (`deleteSignal`,
  `customPidDelete`, `_resolveProtectedName`). Every caller is a fire-and-forget
  inline `onclick` except `saveActiveLayout`, which now awaits
  `_resolveProtectedName`. No caller reads a return value synchronously.
- Enter no longer answers a confirm. That is deliberate: on the editor the
  focused element behind the dialog was the delete button itself, so Enter was
  never a safe "yes". The red button is a click (or Tab+Space) away.
- The two shadowed legacy `fuelSetEmpty/fuelSetFull` definitions (~line 15200)
  are dead code and were left alone; the live pair (~line 22860) is converted.
- rdm7-desktop must re-sync (`python tools/sync_firmware.py`) — done the same
  day on the desktop side.
- **Closed same day, desktop repo:** the four raw `confirm(` guards in
  `../rdm7-desktop/src/tauri-overlay.html` outside the GPS workspace (revert
  layout, transfer overwrite, reboot device, restore layouts) are converted to
  base `confirmAsync` — desktop commit `39332c3` on
  `claude/youthful-gagarin-330d33`, after the overlay WIP had landed. All four
  enclosing functions were already async, so no caller changed. Probed in the
  merged dist: Cancel on "Reboot Device" leaves `RDM.reboot` uncalled; Confirm
  fires it.

## Verification

`node --check` on the extracted editor script. In the merged desktop dist
(browser pane): all four dismissal paths unit-probed on `confirmAsync`
(Cancel/Escape/backdrop → `false` + closed; red button → `true`; Enter
swallowed with the dialog still up; focus starts on Cancel), plus the
`deleteSignal` flow end-to-end (Cancel keeps signal + widget binding,
Confirm removes both). In the real desktop app (worktree dev build driven
over CDP, `window.confirm` confirmed still remapped, Local virtual dash):
`_smDeleteInternal('layout', …)` — the device-file delete — showed the
styled overlay; **Cancel left the file in `/api/layout/list`, Confirm
removed it**. On-device the overlay is the same DOM path the splash/layout
deletes already used.

## References

- ADR-0015 (the finding, and `gpConfirm()` — the pattern this copies)
- ADR-0007 (why the desktop repo consumes this file verbatim)
