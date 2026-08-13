# ADR-0030: Channels are one page, and it works with the dash unplugged

Date: 2026-08-13
Status: Accepted
Repos: RDM-7_Dash (firmware + web editor), rdm7-desktop (`src/transport.js`,
resynced base)

## Context

A customer wrote in. Paraphrased only where it shortens:

> It doesn't seem possible to set up channels while offline.
>
> It's a bit confusing how you have the presets page, the custom CAN signals
> page and the normal channels page. They are all sort of the same thing. Like
> all that could be a single channels page. Then from there you can go "import
> channels from DBC" or "import channels from ECU". When clicking the ECU one,
> you have a list of all the ones you have on the dash, maxxecu, link, haltech
> etc. Or maybe call them pre-defined instead of ECU. Then there's a separate
> one for custom.
>
> The channels, how do you back these up? "Is that what .RDM bundle is?" Make
> what this is clearer, rename or whatever.
>
> Then most of the settings related to channels are on that one page at the
> top and a complete list of channels and their values on the device
> underneath. For some reason you have backup and restore channels at the
> bottom of the setup page under files and storage.
>
> You could treat channels like MaxxECU tune files — it's all live while the
> device is connected, but while offline and you make changes it prompts when
> coming back online.
>
> The data logger page and the live signals page can be one single page, opens
> up live then you click record to actually save a log.

Every one of these was correct, and each was a symptom rather than a
preference. Setup's "Vehicle & channels" section held five cards — Channels,
Live Signals, Import DBC, Custom CAN Signals, Create Preset — of which four
produced the same artifact: a channel. Backup and Restore Channels sat three
sections further down under "Files & storage", next to the .rdm bundle, which
is a different thing that happens to contain a copy of them. And every channel
screen was a thin skin over live HTTP, so with nothing on the other end the
list came up empty and the feature was unavailable at a desk.

## Decision

**One Channels page owns channels**, and its top bar carries the one menu that
every way of making a channel now hangs from:

| Add channels | |
|---|---|
| From a pre-defined ECU… | the customer's naming, kept |
| From a DBC file… | was the "Import DBC" card |
| Custom channel… | was the "Custom CAN Signals" card, and a duplicate `+ New channel` button |
| **Channel setup file** | |
| Back up to a file… | was under Files & storage |
| Restore from a file… | was under Files & storage |

The "Create Preset" card is gone from Setup: authoring a reusable ECU
definition is a different job from setting this car up, and it was the least
used of the five while sitting in the most prominent place. `openPresetAuthorModal`
is untouched and still reachable from the source picker.

**Bulk ECU import is a firmware endpoint, deferred to the LVGL task, not
JavaScript.** "From a pre-defined
ECU" needed something Studio never had: turning a whole preset into channels.
The one-at-a-time source picker could only bind a preset signal to a channel
that already existed, so setting up a car meant hand-creating every channel
first. The dash's own setup wizard had done the bulk apply since day one —
alias table, canonical resolution, custom fallback, collision guard. Rather
than reimplement that resolution in the browser, the wizard's function is now
exported (`first_run_wizard_apply_ecu`) and `POST /api/channels/import-preset`
calls it. Two copies of an alias table would have disagreed about some ECU
eventually, and the failure would have been a silently mis-decoded channel.

The endpoint takes a `replace` flag. The wizard passes `true` — this ECU *is*
the car's, so clear prior vehicle bindings and record it as the active ECU.
Studio's menu passes `false`: a menu item that says "Add" must not wipe
someone's other bindings, and importing a few Haltech signals must not relabel
a MaxxECU car.

It answers **202 with the matched count** and does the work on the LVGL task
via `rdm_async_call`, rather than 200 with an applied count. That is not a
style choice — see "Then flashed to the bench dash" below. The client waits for
the channel count to settle and reports the real delta, which is both the
number the user cares about and the one that cannot be wrong.

**Live values and recording are one page.** The "Signal Dashboard" modal was a
read-only twin of a table the Data Logger had no reason not to show. Merged:
the page opens live, Record is the primary control above the numbers it
captures, and the file/replay machinery collapses into a disclosure. The dash's
own settings made the same merge — the "Peak Hold" card was its live-signal
view, and it now opens from inside Live Data & Logging.

**The .rdm bundle says what it holds.** "Import .rdm" / "Export .rdm" became
"Open dashboard bundle" / "Save dashboard bundle", and both spell out the
contents: layout, images, fonts, *and* the channel setup. That last item is why
the customer's guess was so reasonable — a .rdm really does carry channels.json
inside it. The channel-only file is now consistently "the channel setup file"
and lives on the page that owns channels.

## Offline: mirror plus queue, and the invariant

Channels are treated as the customer described — a tune file.

- **MIRROR** — the last channel set and canonical registry actually read from a
  dash, in localStorage.
- **QUEUE** — mutations made since, stored as *the API calls themselves*.

```
INVARIANT:  what you see  ==  mirror + queue replayed over it
```

Queueing the call rather than a diff is the load-bearing choice. Replaying it
on reconnect runs the firmware's own resolution, so an offline edit and an
online one take the same code path and there is no second implementation to
drift. It also means the interception sits in one place: a `window.fetch`
wrapper over a strict allowlist of five channel mutation paths, which covers
all 21 existing call sites and any added later — otherwise this would be
"offline for the three screens someone remembered".

Anything that genuinely needs the device stays blocked and says why: bulk ECU
import resolves against the dash's preset catalog, and restore replaces
channels.json and reboots. Backup, by contrast, works offline — refusing to
save a copy of what is on screen would be obtuse — and names the file's vintage
so a desk copy is not mistaken for a fresh read off the car.

Offline rows render with values blanked, never with the last-seen number. A
stale reading shown as current is how someone comes to trust a figure the car
never sent.

### Two things found by doing it

**A 200 is not proof of a dash.** RDM Studio's local mode answers
`/api/channels` itself with an honest empty collection so the editor doesn't
404 on the `tauri://` origin. Read as "connected", that blanks the mirror and
disables offline editing — on the client most likely to be at a desk, which is
the whole point of the feature. The local stub now carries `offline: true` (as
`/api/device/info` and `/api/selftest` already did) and the editor keys off the
flag, not the status code.

**"Did we just reconnect" is the wrong trigger.** The first cut prompted on an
offline→online transition. Edit offline, close Studio, come back tomorrow at
the car: the page loads fresh with the online flag already true, there is no
transition to notice, and the offer is never made — the pending edits just sit
there. Keyed on "offered since the dash was last absent" instead, which covers
both that and a mid-session reconnect.

## Consequences

- "Vehicle & channels" is one card. Setup has six fewer cards overall.
- A new endpoint: the dash reports **147 of 160** `max_uri_handlers` used, 0
  failures (`uri_registration` in `GET /api/selftest`, read off the hardware —
  CLAUDE.md's "~138" was stale). 13 spare.
- `GET /api/channels/source-options` now takes `?id=` as **optional**. It was
  mandatory, and used for one thing: marking the row matching that channel's
  bound signal `is_current`. The bulk picker is not editing one channel, so it
  has no id to send and got a 400 — which the modal, correctly but unhelpfully,
  reported as "this needs the dash connected".
- `_apply_ecu_preconfigs` is a two-line wrapper over the exported function, so
  the wizard's behaviour is unchanged by construction.
- The `openSignalDashboard()` / `closeSignalDashboard()` names survive as
  aliases; the OBD2 menu entry that used them now reads "Live Data".
- The merged page polls two endpoints per tick, so it uses a self-rescheduling
  timeout rather than `setInterval` — a fixed interval against the dash overlaps
  itself and piles up.
- `_jsArg()` replaces the `'${x.replace(/'/g,"\\'")}'` idiom for the new inline
  handlers: that pattern handles an apostrophe but a double quote in a label
  ends the HTML attribute early.
- Firmware 1.2.0 → 1.3.0; RDM Studio 0.5.0 → 0.6.0.

## Verification

Firmware builds clean (`0x2fd530`, 15% free). 19 native suites pass.

Driven in the browser against the dev server, trapping console errors and
asserting rendered effects rather than just successful posts:

- Setup shows one Channels card and one Live Data & Logging card; no channel
  backup cards remain under Files & storage.
- The ECU picker lists MaxxECU / Haltech / Link and filters out the virtual
  OBD2 / RDM-7 / Custom buckets, which carry no importable decode. It lands on
  the active ECU with every signal ticked; each row prints its frame and bit
  position.
- Unticking Lambda and importing added exactly the other 5, and the one already
  present was reported as such rather than counted.
- **Offline, for real** — server stopped, not stubbed: the page loads 9 channels
  from the mirror with values blanked, the pill reads "Offline · your copy", and
  the two dash-only menu items grey out with the reason.
- An offline threshold edit queued as `edit coolant_temp`; the mirror stayed at
  105 while the page showed 108, across a close and reopen.
- Server restarted, **fresh page load**: the prompt fired, "Send to dash"
  replayed the queue, the queue cleared, the pill returned to "Live on the
  dash", and 108 was confirmed in the server's own store.
- Record shows its state: "■ Stop recording" on red with a live sample count,
  back to "● Record" on stop. Found only after making the dev server's logger
  status stateful — it had answered `active: false` unconditionally, so the
  button could be clicked but never showed anything.
- 20-step sweep across both pages: 0 console errors.
- The merged desktop build (all 28 overlay anchors still resolving) loads with
  the overlay present, the ECU picker populated, and 0 errors.

One bug caught this way that review would not have: the ECU import modal's id
was `chEcuImportModal` in the markup and `chEcuImpModal` in the JavaScript. It
threw on open.

### Then flashed to the bench dash (RDM-DCB4-D926), which found two more

The browser work above ran against the dev server's mocks. Real hardware
disagreed with them twice, both in ways the mock had papered over:

- **`source-options` required `?id=`.** The mock did not. On the dash the bulk
  picker's id-less call 400'd, and the modal reported the dash as unreachable —
  a correct message for the wrong reason. `id` is now optional, because it was
  only ever used to mark one row `is_current` and the bulk picker has no
  current channel.
- **A zero-match import hung for the full client timeout.** `import-preset`
  called the apply unconditionally, and the apply is unconditional work: it
  opens and commits the layout, re-resolves every channel and rebuilds the CAN
  acceptance filter. So a request matching nothing mutated state the caller
  never asked to change. Matching rows are now counted first, from the const
  preconfig table with no lock, and zero returns 404 having touched nothing —
  35 ms instead of a timeout.
- **…and then a matching import hung too**, which is what the first fix
  revealed rather than caused. Two real MaxxECU signals: 19 s, no response,
  nothing applied, dash otherwise answering `/api/selftest` fine. The apply
  ends in `can_set_promiscuous_mode()`, which reconfigures the TWAI driver;
  doing that from the httpd worker **while holding the LVGL mutex** inverts
  lock order against the CAN RX task, which needs that same mutex to drain its
  queue. The wizard never hit this because it has always run on the LVGL task,
  where the recursive mutex makes it a non-issue.

  CLAUDE.md states the rule outright — *"Use `lv_async_call()` from any other
  context (web server, etc.)"* — and this ignored it. The endpoint now
  validates, counts, copies its arguments, answers 202, and defers via
  `rdm_async_call`. **A handler that takes the LVGL lock and calls into driver
  reconfiguration is a deadlock waiting for the right request**; the lock is
  for short, bounded state reads, not for whole subsystem operations.

Also corrected off the hardware: `RDM-7` is excluded from the ECU picker, but
not for the reason first written. It is a *real* preset set (12 rows), not a
synthesised bucket like OBD2 and Custom — it is the dash's own GPIO and
internal signals (fuel sender, indicators, calculated gear, FPS), all with
`can_id` 0 because they never come off the bus. Importing them as CAN channels
would decode nothing.

The dash lists 7 real ECU makes for the picker — Ford, Haltech, MaxxECU, ECU
Master, MegaSquirt, Link ECU, Toyota — with MaxxECU the largest at 146 signals
across 2 versions.
