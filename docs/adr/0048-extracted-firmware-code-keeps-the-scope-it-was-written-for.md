# ADR-0048: Extracted firmware code keeps the scope it was written for

Date: 2026-08-26
Status: Accepted
Repos: rdm7-desktop (`src/tauri-overlay.html`)
Refines the overlay pipeline of [ADR-0007](0007-html-source-of-truth.md).
Prompted by a customer: *"With the desktop studio app should I be able to
import layouts like the examples on your site — I get import failed: file is
not defined"*.

## What broke

Desktop Studio could not import a `.rdm` at all. Every attempt — the sidebar
Import button, a marketplace download, a double-clicked file — ended at:

> Import failed: file is not defined

It had been broken since 2026-08-14 and shipped in 0.7.0 and 0.7.1.

## Why

`importRdm()` in the firmware editor is an `<input type=file>` handler. Its
whole body runs inside an `onchange` where `const file = input.files[0]` is in
scope, and the body reads `file.name` freely.

The desktop app has no file input to hand — under Tauri the bytes arrive from a
native dialog, a file association, or drag-drop. So the overlay extracts that
body into `_processRdmBytes(u8, fileName)` and calls it from all three routes.
The body is copied **verbatim** from the firmware base; only anchored blocks
differ. And in that new home, `file` does not exist.

The overlay handled this by anchoring the two `file.name` mentions that existed
and rewriting them to `fileName`. That works exactly until the firmware adds a
third. [ADR-0042](0042-a-bundle-should-say-whether-it-carries-your-car.md) added
one — the line that announces which flavour of bundle arrived:

```js
updateStatus(isDashboard
    ? `${file.name} — full dashboard (includes channel setup)`
    : `${file.name} — layout only`);
```

Correct in the firmware. In the extracted body it is a `ReferenceError`, thrown
inside the import's own `try`, reported as the customer's message, and the
import dies. Note where it sits: after parsing, before the first device call. A
customer with a dash connected never got as far as talking to it, which is why
the failure looked identical online and offline.

The merge did not catch it because there was nothing to catch. Drift detection
in this pipeline is anchor mismatch — a block whose anchor text no longer
appears. This block's anchors still matched perfectly. The firmware added a new
line in an unanchored region, the merge copied it faithfully, and the result was
a build that parsed, passed syntax checks, and failed on the customer's machine.

## Decision

**Give extracted code the scope it was written for, rather than editing it to
suit its new home.**

`_processRdmBytes` now opens with a stand-in:

```js
const file = { name: fileName, size: u8.length };
```

The verbatim body reads `file.name` and gets the right answer. The anchored
`import-rdm-name` block that rewrote two mentions is gone — it has nothing left
to do, and every anchor removed is one fewer thing that can drift.

The same reasoning fixed the file-association route in passing. It fed bytes to
`importRdmInput` and fired a `change` event, but that input only gets an
`onchange` in `importRdm()`'s browser branch — under Tauri the function returns
at the native-dialog branch first, so the event landed on nothing and
double-clicking a `.rdm` did nothing at all. It now calls `_processRdmBytes`
directly, like the other two routes.

## Consequences

- Firmware may add `file.name` mentions to the import body forever; the desktop
  build keeps working with no overlay change.
- The stand-in must carry every property the firmware body reads. `name` and
  `size` are covered. If the firmware ever calls a real `File` **method** —
  `file.arrayBuffer()`, `file.slice()` — the stand-in must grow, and it will
  fail loudly at import rather than silently.
- The general lesson for `merge_overlay.py`: **a block that relocates firmware
  code changes its scope, and the merge cannot see scope.** Anchors catch text
  drift, not semantic drift. When a block moves code somewhere else, prefer
  reconstructing the environment the code expects over patching the code.
- Verification for this pipeline cannot stop at "it merges and parses". The
  failing line parsed fine for twelve days. Importing a real `.rdm` end to end
  is the check that would have caught it.
