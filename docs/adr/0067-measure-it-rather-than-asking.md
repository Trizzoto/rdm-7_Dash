# ADR-0067: Measure it rather than asking, and call it what people call it

Date: 2026-09-04
Status: Accepted — built 2026-09-04
Repos: rdm7-desktop (`src/tauri-overlay.html`, `tools/check_setupwiz.js`,
`tools/check_pairs.js`)
Follows: [0055](0055-a-device-on-the-wrong-bitrate-is-invisible-not-broken.md)
(the keypad's wizard), [0066](0066-the-app-says-what-it-knows.md),
[0011](0011-nothing-is-drawn-that-was-not-measured.md)

## Context

ADR-0066 finished the analysis side. Looking again at what was left, the gaps
were no longer in the maths — they were at the two ends nobody had gone back
to: the first five minutes with a new puck, and the words the app uses for
things that already have names.

## 1. There was a wizard for the keypad and none for the puck

`grep "Set it up for me"` returned six hits, **all keypad**. It exists because
a device on the wrong bitrate is invisible rather than broken (ADR-0055), and
the puck has exactly that class of failure: a CAN base id that collides is
silent, a bitrate that does not match is silent, `record_on_boot` set for the
wrong kind of car costs a session, and a puck bolted in rotated produces angles
nothing downstream can question. All of it lived across **nine groups** of a
Setup page a new owner has to decode.

### The mounting step is the one that could not be done any other way

`gpMountHtml` asks two questions — *"board +Z points"* and *"board +X points"* —
that nobody who has just bolted a box under a seat can answer, and getting them
wrong is undetectable afterwards. `gpDriftAngle` self-calibrates scale and bias
from ordinary driving, but its 0.8–1.25 refusal band **papers a rotated puck
over by falling back to `scale = 1`** rather than diagnosing it. That is the
deferred "item 1b" from `EIGHT_FEATURES_PLAN_2026-09.md`, and this is what it
turned out to want.

So the wizard measures it, off raw axes the firmware already sends *for this
exact purpose* — the comment in `rdm-gps-node` says so:

> `imu.board` is the same instant straight off the part, which is what a
> mounting-setup screen needs

- **Parked**, four seconds: gravity names the vertical axis. Refuses if the car
  moved, if no axis reads near 1 g, or if more than 350 mg is leaning onto
  another axis — that last one is a slope, and a slope would tilt the answer.
- **Rolling**, one firm stop: the axis whose acceleration correlates with the
  GPS speed derivative is the longitudinal one. The vertical axis found above
  is excluded before correlating; a run needs at least 22 km/h of change and
  |r| ≥ 0.55, and where it does not get them it says which it was short of.

**Which axis is convention-free. Which direction along it is not.** Whether a
part reads +1 g on the axis pointing up is a convention this session cannot
verify without hardware, so the wizard does not assert it: it states what it
believes as a sentence anybody can check by looking at the car — *"the top of
the puck is facing the sky"* — with one button to turn it round. Measure what
can be measured, ask about what cannot, guess at nothing.

Nothing is written until the button that writes it. The node re-validates the
whole set as a right-handed rotation and refuses anything that is not.

## 2. Corners had numbers, and nobody uses numbers

Every surface said `T4`; the drift board generated `"Turn " + first.n` and
`"Turns 3–5"`. Nobody standing at a circuit says either.

A name belongs to the **track**, not to a lap and not to a recording — corners
are detected per lap and their indices move with the detector, so numbering
them is the one key guaranteed to break. The key is **where**: the apex's
position on the earth, matched within 45 m. That survives a re-split, a
different lap, a different day and a different car.

One lookup, used by every surface at once, so naming a corner in the Corners
panel renames it in the repeatability panel, the session verdict, the coach
line and the drift board together. Proved on the committed Donington recording:
naming T4 *The Melbourne Hairpin* turned the verdict into *"The Melbourne
Hairpin is the one to practise: it is worth 0.70 s between your best lap through
it and your worst"*, and stored it at 52.8322, −1.3742 — which is where
Donington's Melbourne Hairpin actually is.

Editing is inline. `prompt()` is not shimmed in this webview and never returns
(it has cost a session before).

## 3. The ring warned you afterwards

`wrapped`, `dropped` and `holes` are captured into session meta and reported by
the trust panel — after the driving is gone. Two changes:

**It admits when it cannot say.** The "Recorded X min" row is built on
`used_samples`, and that counter **read zero over a ring holding thirty-two
minutes of real driving** (measured 2026-08-12; the data was only found by
sweeping `trace.read`). A counter reading zero beside a ring that has already
wrapped is contradicting itself, and printing "0.0 min recorded" from it is
worse than printing nothing — it is the reassurance you would act on right
before losing a session.

**And it says so before, not after.** Under 45 minutes of room with recording
on, the readiness card carries the download button and the reason.

## 4. Live was a bench view

Cards of PDOP, satellites and horizontal accuracy — right for a puck on a desk,
useless on a pit wall a metre away. The same reply already carries
`lap_time_current`, `lap_time_best` and `lap_delta`; they were four more small
rows among fifteen. The delta now leads at 58 px because it is the number that
changes what you say over the radio, with the current lap under it and last and
best beneath that. The four rows that duplicated them are gone.

The delta only exists once there is a best lap to be ahead **of**, and where
there is not it says so rather than printing a confident zero.

## 5. There was a Driver field and nothing read it

Cross-session comparison was already complete — `ghostFence`, `gpGhostShow`, and
`gpRefName` even puts `car · driver` in the reference label. The gap was
discovery: the list was called *"From another day"*, in no particular order.

Now it is sorted **quickest first** — the reference you want is almost never the
one that sorts first by date, and on a track with a season of visits the good
one was off the bottom — and split into *Your other days* and *Other drivers*
when there is somebody else to split off. A heading over one group is noise, so
it only splits when it means something.

History gained **Best here, by driver**, drawn only when the recordings name
more than one. The trend above it is every session at this track regardless of
who drove it, which is right for "am I quicker than I was" and wrong the moment
two people share a car — one sets the personal best and the other reads it as
their own.

## Consequences

- Corner names are stored on the track in `rdm7_tracks_v1`, which is real user
  data. Naming is additive and an empty name removes the entry; nothing else
  touches the structure.
- The wizard writes only `record_on_boot` and `mounting`, both through the
  existing setters, and only on an explicit press.
- The mounting measurement runs off the 2 Hz status poll. That is coarse for a
  signal and ample for this question, which is not "how many g" but "which of
  three".

## Checks

`tools/check_setupwiz.js` — 22 assertions. The wizard's whole claim is that it
measures rather than asks, so the axes are synthesised: a puck at each of the
**24 reachable orientations** produces known readings and the wizard has to name
the orientation back, with the third axis matching what the firmware would
derive — anything else and `node.config.set` refuses the lot. Plus: a brake
correlates, lateral and vertical noise do not, a backwards puck correlates
negatively, and one measurement alone writes nothing. Corner names are pinned
against a lap that clipped a different line, a re-name from another lap, and the
empty-name removal.

`tools/check_pairs.js` still passes, as do `check_autosync`, `check_carown`,
`check_lookbar`, `check_clips`, `check_focus`, `check_carglyph`, `check_health`,
`check_laps`, `check_drift`, `check_shell`, `check_gridfit` and
`check_playhead`.

**Not proved here:** the wizard has never seen a puck, so the accelerometer
sign convention behind *"the top is facing the sky"* is stated rather than
verified — which is why it is a sentence with a button under it rather than a
value written silently. One bench session settles it.
