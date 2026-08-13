# ADR-0032: Adding a channel is a decision, and destroying one leaves a copy behind

Date: 2026-08-13
Status: Accepted
Repos: RDM-7_Dash (`main/web/index.html`)
Follows ADR-0031, which made Channels a page. This makes it safe to use badly.

## Context

Two irreversible actions on the channels surface could be reached by accident.

**A single click created a permanent channel.** Clicking a greyed catalogue row
activated that canonical channel immediately — deliberate behaviour, described
in the code as "discovery and config in the same view", and fine when the list
was a small modal. It stopped being fine for a reason nobody had connected to
it: `channel_manager_delete()` refuses any non-custom id, with a stated
contract that "deactivating a canonical channel means clearing its signal, not
deleting it". So an activated built-in channel **can never be removed**. It
holds one of 128 slots for the life of the config, and the only way to undo it
is to restore a backup of the whole registry.

ADR-0031 then put ~100 catalogue rows one click away on a full-width page. A
mis-click was a trap with no way out.

This is not hypothetical: two channels (`afr_bank1`, `dtc_count`) appeared on
the bench dash during ADR-0031's own testing, from scripts clicking "whatever
row is first". Both times the fix was to restore a backup.

**Restore destroyed the current setup with nothing to fall back on.** "This
cannot be undone" was accurate, and the way back — a copy of the current
channels.json — was a file the user had to have known to make in advance.

## Decision

**Selecting a channel never creates one.** Clicking a row that isn't set up
opens a preview instead: what the channel is, its units, its typical range, its
group, how many of the 128 slots are used, and whether a matching signal is
already arriving (in which case it says the source will be connected
automatically). Adding it is a button press.

The preview states the consequence plainly rather than burying it: *"Adding is
permanent: built-in channels can have their source cleared, but they can't be
removed from the dash afterwards."* When the registry is full the button is
disabled and says so, instead of failing on press.

The prevention is deliberate rather than a confirmation dialog. A dialog per
click would be noise while browsing the catalogue, and it trains people to
dismiss it. Making the *creating* act a separate press means the accident
cannot happen at all, and the preview is genuinely useful on the way — you can
see what a channel measures before committing a slot to it.

**Restore saves the outgoing setup first.** Before the confirm is even shown,
the current channels.json is exported to the user's downloads as
`channels_<serial>_before-restore.json`, and the confirm then names that file
as the way back. If the export fails, the confirm says so — "the current setup
could NOT be saved first, so there is no way back from this" — rather than
implying a safety net that isn't there.

## Consequences

- `_selectChannel` no longer performs I/O for canonical channels; the activate
  + strict-signal-autobind path moved verbatim into `_chActivateSelected`.
- The widget inspector's picker still activates on pick. That is explicit
  intent ("bind this channel to this widget"), not browsing, and is unchanged.
- The spillover heading changed from "Click one to add it" to "Click one to see
  what it is", which is now what happens.
- Canonical channels remain undeletable. Prevention removes the accident, but
  someone who deliberately adds thirty channels still cannot tidy up
  afterwards. Left alone here: allowing canonical deletes changes a firmware
  contract with real blast radius (widgets bind by channel id), and it is the
  user's call, not a side effect of a UI fix. **Open question, flagged.**

## Verification

Against the real dash (`RDM-DCB4-D926`) through the `--device` proxy, with
`window.fetch` trapping every `/api/channels/activate`:

- Clicked a catalogue row (`absolute_load`) in the Available view: **zero**
  activate calls, channel count unchanged at 32, preview rendered — "Units %,
  Typical range 0 – 300 %, Group Engine — Core, Slots used 32 of 128", the
  no-signal-yet note, and the permanence warning.
- Pressed "Add this channel": one activate call, 32 → 33, toast "Added Absolute
  Load", and the panel became the normal editor.
- Dash restored to its 32-channel backup afterwards.
- 0 console errors throughout.
