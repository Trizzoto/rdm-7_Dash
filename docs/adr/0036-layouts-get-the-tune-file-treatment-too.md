# ADR-0036: Layouts get the tune-file treatment too, but last-write-wins

Date: 2026-08-14
Status: Accepted
Repos: RDM-7_Dash (`main/web/index.html`), rdm7-desktop (resynced base)
Completes the asymmetry left open by ADR-0030: channels prompted on
reconnect, layouts did not.

## Context

ADR-0030 gave channels the model the customer asked for — "it's all live
while the device is connected, but while offline and you make changes it
prompts when coming back online". Layouts never got it. In RDM Studio they
at least had a real local store (`LocalTransport`, pre-existing), but in the
browser editor served by the dash a save while the dash was unreachable did
this:

```js
} catch (e) { updateStatus('Apply failed', true); showToast('Apply failed', 'error'); }
```

Two words, no recovery. The edited layout existed only in the
crash-recovery `rdm7_draft` — a mechanism for surviving a reload, never
advertised as "your work is safe", and silently overwritten by the next
edit to any layout. Someone editing at the car when the dash drops off WiFi
gets told their save failed and is left to work out the rest.

## Decision

Layouts stash on a failed save and are offered back on reconnect, with the
same words and the same shape of promise as channels — **but a different
storage model, on purpose.**

**Last-write-wins per layout name, not a replay queue.** Channels queue the
API calls themselves because each mutation is a distinct intent that must
replay in order (ADR-0030). Layout saves *supersede*: the editor autosaves
on a 1 s debounce, so a queue would accumulate forty intermediate saves,
push thirty-nine stale layouts across the wire, and arrive at exactly the
result the last one alone produces. So the store is a map,
`{ layoutName: {payload, isSplash, ts} }`, and a later save for the same
name overwrites the earlier one.

**Only a network failure stashes.** An HTTP error — 413 too-large, a
firmware rejection — is a real answer from a reachable dash and keeps its
existing, specific message. Stashing those would offer to re-send something
the dash has already refused.

**The reconnect moment is one that already exists.** No new poll: Design
mode's screenshot loop already flips `_haveLiveFrame` false→true the instant
a device frame lands again, and the Channels page's 2 Hz poll already
detects reconnection (ADR-0033). Both call the check, which reads one
localStorage key and returns immediately when nothing is waiting. Covering
both matters because someone who lives in Channels would otherwise never be
asked, and someone who never opens Channels likewise.

Declining keeps the work and re-offers on the next reconnect; a failure
partway through keeps everything not yet sent, exactly as the channel queue
does.

## Consequences

- The full offline story is now symmetric: channels and layouts both survive
  a dropped dash and both ask before pushing.
- `rdm7_draft` goes back to being what it is — crash recovery for the
  in-progress edit — rather than the accidental last line of defence.
- RDM Studio's desktop local-layout store is untouched and still the richer
  path there (it can list/load/rename offline); this covers the browser
  editor and the "dash vanished mid-session" case that the local store does
  not.
- A layout stashed under a name that is later deleted on the dash will
  re-create it on send. Judged correct: the user's later intent was to save
  that layout, and silently dropping their work to honour a deletion they
  may not have made would be worse.

## The 304, found by testing on a real dash

The first cut hooked the offer to the screenshot loop's decoded-frame
branch — and would have shipped broken for the commonest case. The loop is a
conditional GET: when the dash's screen has not changed, it answers **304
Not Modified**, which returns early down its own path that also flips
`_haveLiveFrame`. A parked car with a static dashboard produces *nothing but*
304s, so Design mode would have offered pending saves only on a dash whose
screen happened to be animating.

It survived three test rounds looking like a test-harness problem (a leaked
`fetch` stub from a timed-out run, then the live-preview loop racing the
transition) before the real cause showed. Both transitions now call the
check.

## Verification

Against the live dash on the bench (static screen — the 304 case):
a save while unreachable reported "Dash not reachable — *name* saved here,
and offered to the dash when it reconnects" instead of "Apply failed"; the
payload landed in `rdm7_layout_pending`; a second save of the same layout
replaced rather than appended (last-write-wins), payload carrying its
widgets. On reconnect the prompt fired **from the 304 branch**, "Send to
dash" pushed it, `/api/layout/list` showed the layout on the device, and the
pending store cleared. The channels-poll reconnect path was verified the
same way against the mock. Probe layout deleted from the dash afterwards.
