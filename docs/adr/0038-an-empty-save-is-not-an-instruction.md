# ADR-0038: An empty save is not an instruction

Date: 2026-08-14
Status: Accepted
Repos: RDM-7_Dash (`main/net/web_server_layout.c`, `main/web/index.html`),
rdm7-desktop (resynced base)
Found while investigating "the preset allocations didn't work and the
default layout has stopped working" — one fault reported as two.

## What happened

The bench dash was showing unlabelled panels, `BAR1`/`BAR2` and no gear
readout. Its `default` layout had **26 widgets replaced with zero**, and
with the layout gutted the channels were bound to a signal registry whose
entries all carried `can_id = 0` — so nothing decoded, and the ECU preset
looked broken. Two symptoms, one cause.

`POST /api/layout/save` persisted whatever `widgets` array arrived. The
handler said so in its own comment:

> Factory-default protection lives client-side now […] Firmware just
> persists whatever name the client requests.

And the client that would eventually get it wrong was already there:

```js
let currentLayout = { name: "default", widgets: [], signals: [] };
```

That is the editor's state **before anything loads** — a layout named
`default` with no widgets. Any save fired while the page sat in Channels
mode, where Design never initialises, posted exactly that. The dash then
falls back to a bare slot skeleton, and nothing in the response, the log
or the screen says a layout was destroyed.

## Decision

**Emptying a layout has to be said out loud.** A save whose `widgets` array
is empty, against a stored layout that has widgets, is refused with `409`
and a sentence, unless the body carries `"allow_empty": true`.

Unaffected, because there is nothing to lose: layouts that don't exist yet,
layouts already empty, and splashes (a one-widget splash being cleared is
not the same kind of loss, and the splash editor legitimately saves empties).

**The client only claims intent once it has something to be intentional
about.** `_layoutEverLoaded` flips true when a layout is actually loaded —
from the device, a draft, an import, or a deliberate New — and
`buildFirmwarePayload` sets `allow_empty` only then. That is precisely the
line between "the user deleted every widget" and "we never had any".

ESP-IDF's `httpd_err_code_t` has no 409, so the status line is set directly
rather than reaching for a wrong-but-available code. The request is
well-formed; it conflicts with what is stored. That distinction is the
whole point of the response.

## Why not client-side only

That is what was there. Protection lived in `_resolveProtectedName` in the
editor, and it protected against the user *naming* something wrong — not
against the editor sending a state it never meant to send. A guard in the
client cannot cover the desktop app, a stale browser tab, a marketplace
import, or `curl`. The dash owns the data, so the dash refuses.

## Consequences

- Any client that genuinely wants an empty layout must be updated to send
  the flag. Ours is; RDM Studio inherits it through the shared base.
- A layout can still be emptied — this is a speed bump for a bug, not a
  policy against deleting things.
- The failure that prompted it is now loud: a 409 with a sentence, instead
  of a silently gutted dashboard discovered days later.
- `POST /api/layout/reset_default` regenerates the factory layout, which is
  how the bench dash was recovered. Worth knowing before diagnosing "the
  preset stopped working": check the active layout's widget count first.

## Verification

On the live dash: the exact destructive payload
(`{"name":"default","widgets":[]}`) returned **409** with all 26 widgets
still on disk; a scratch layout with one widget refused the same payload,
then accepted it with `allow_empty: true` and went to zero; the restored
default survived a reboot and rendered its labelled panels, gear and bars.
