# Channels: the source picker and the channel editor

Status: **Built, 2026-09-12.** Phases 0-4 shipped; see
[`docs/adr/0069-one-source-picker-and-a-decode-editor-you-can-see.md`](adr/0069-one-source-picker-and-a-decode-editor-you-can-see.md)
for what was decided and the two bugs the redesign exposed. This document is
kept as the analysis that led there — §1 is still the clearest account of what
was wrong. The phase table in §5 is annotated with what actually landed.

Written from a walk through the running editor and the code behind it; every
claim below points at the line that proved it. Line numbers are from before the
rewrite and will have moved.

Scope: `main/web/index.html` (and its port in `rdm7-desktop/src/firmware-base.html`),
`tools/mobile-dev-server.js`, `rdm7-desktop/src/transport.js`, and one small
change to `main/net/web_server_channels.c`. No schema change, no codegen.

---

## 1. What is actually wrong

Tommy's report, in his words: *"pick a source is not a good layout … no option
for new/custom … too many spots for new source, new channel … not even fully
populated? Generic dash not there, just says Link G4X … the side menu thing
when you click a channel can be bigger."*

Every one of those is real. But two of them are not the bug they look like, and
separating them first stops the fix landing in the wrong place.

### 1a. "Not fully populated / Generic Dash missing" — the dev server was lying

The picker is fed by `GET /api/channels/source-options`. On a dash that
handler enumerates `preconfig_items[]` — the same catalogue baked into the
page (ADR-0033) — grouped by make → version → signal
(`web_server_channels.c:1182`). That table has **Link ECU → Generic Dash (41
signals) and AiM Stream (14)**, all on `0x3E8` with their mux gates, plus
Haltech Nexus (69), MaxxECU, ECU Master, both Fords, MS3-Pro, GT86, RDM-7.

What Tommy saw was `tools/mobile-dev-server.js` `DEV_ECU_MAKES`
(`mobile-dev-server.js:210`) — a hand-typed stub, "trimmed to a few signals
each", whose Link entry is **"G4X, 5 signals" on `0x3E8`/`0x3E9`/`0x3EA`**. That
is the pre-1.4.1 mis-model of Link as consecutive ids, the exact bug the
firmware fixed. The numbers in the screenshot (MaxxECU 8 signals · 2 ver,
Haltech 6, Link 5) match the stub line for line.

So: **a real dash is fully populated.** The tool we evaluate the UI with is not,
and it is stale in a way that re-teaches a fixed bug. Fix in §4.

### 1b. "No option for new/custom" — true, and it is worse offline

`#chSignalPicker` has no create action of any kind. The "Custom" make it
shows lists **existing** CAN-sourced signals only (`web_server_channels.c:1319`).
To type a decode you have to know that an *unbound* channel's drawer grows a
"+ Add CAN signal" button under **More** (`_chAddCanSignal`,
`index.html:~23370`), or leave the picker for the "+ Add channels" menu.

And in RDM Studio with no dash: `_openSignalPicker` fetches source-options
with no fallback (`index.html:25418`), and `transport.js` has no local route
for it — the call reaches `throw new Error('USB: unmapped endpoint')`
(`transport.js:1921`). Result: **"Failed to load sources."** Meanwhile the
bulk-import picker one menu away *does* fall back to the baked catalogue
(`_ecuMakesFromBaked`, `index.html:25506`). Two pickers, same data, one works
offline and one doesn't.

### 1c. "Too many spots" — there are three pickers and eleven create buttons

Three separate ECU → Version → Signal drill-downs, all reading the same data,
all styled the same, each with its own state, search and selection code:

| Surface | Purpose | Data | Offline |
|---|---|---|---|
| `#chSignalPicker` "Pick a source" | bind ONE channel | source-options | broken |
| `#chEcuImpModal` "Add channels from a pre-defined ECU" | create MANY channels | source-options | baked fallback |
| `#signalModal` "Custom CAN Signals" (the Signal Manager) | browse/author presets | `/api/presets` + custom | n/a |

Plus `#assignModal` (legacy widget signal assign, with its own "+ New Custom
Signal" / "+ New ECU" footer), `#presetAuthorModal` "Create Preset", and
`#ecuPresetModal` behind the context strip's *ECU preset* chip — a fourth
list of ECUs, with live match dots.

Ways to author a source or channel, counted from the DOM:

1. + Add channels → From a pre-defined ECU…
2. + Add channels → From OBD2…
3. + Add channels → From a DBC file…
4. + Add channels → Custom channel…
5. Drawer → More → "+ Add CAN signal" (unbound channels only)
6. Drawer → Source → "Calculate…"
7. Signal Manager → "+ New ECU"
8. Signal Manager → "Create Preset"
9. Signal Manager → "+ Add Signal" (custom presets)
10. Assign modal → "+ New Custom Signal"
11. Assign modal → "+ New ECU"
12. OBD2 scan → "Add a PID by hand…"

Items 7–11 are the Signal Manager and Assign modal, which ADR-0035 kept
deliberately *as the preset library* — but nothing in the UI says so, and both
still present themselves as "pick a signal for this thing". Items 5 and 10 are
the same form as item 4 with fewer fields.

### 1d. The drawer is 440 px, whatever the screen

`#chShared.ch2-page .ch2-editor { width: 440px }` (`index.html:1867`). On
Tommy's 1915 px display that is 23% of the width, with the list beside it
getting the rest and doing nothing with it. Inside, the CAN decode block is a
**two-column** grid (`index.html:26406`) — measured at 1280 px: pane 440,
the Signed and Endian `<select>`s **89 px** each, hence "Unsigne" and
"Little (In". Fourteen controls in the pane, and the one block a CAN owner
actually edits — the decode — is collapsed under **More** beneath Label.

The drawer is also the one surface that got *none* of the new-channel form's
work: no live bus probe, no mux fields, no "is my bit offset right" answer. You
get that when creating a channel and lose it the moment you save.

---

## 2. Rules this has to obey

Not restated in full — read the ADRs — but these are the ones that decide the
shape:

- **ADR-0030/0031** — Channels is one page; `+ Add channels` is *the* menu every
  way of making a channel hangs from; the editor is a drawer on the page and a
  pane in the modal, **one markup, one renderer**, layout by CSS off `.ch2-page`.
- **ADR-0035** — the Source cell speaks provenance; the Signal Manager survives
  *as the preset library*, not as a picker.
- **ADR-0039** — group by the question you arrived with; outcome first, wiring
  second; advanced fields fold.
- **ADR-0014** — monochrome chrome, one filled primary per panel, accent marks
  the active thing.
- **ADR-0033** — the catalogue travels with the page; offline must show it.
- Tommy's standing preferences (memory): plain-English cards, one primary
  action, detail behind a collapsed Advanced; dense headers; no emojis;
  mobile-first.
- Do not re-add `_flushChannelEdits()` inline; edits buffer against
  `_chPendingId`, never the selected row. Never `setInterval` against the dash.

---

## 3. The design

### 3.1 One picker, with a mode

Delete two of the three drill-downs. Keep **one** `#chSourcePicker` component
that opens in one of two modes, sharing markup, data loading, offline fallback,
search and keyboard handling:

- **bind** — "Choose a source for *Coolant Temp*". Single-select. Primary
  button: *Use this source*.
- **add** — "Add channels from your ECU". Multi-select with tick boxes and a
  count. Primary button: *Add 6 channels*.

`#chEcuImpModal` becomes the add mode. `#chSignalPicker` becomes the bind
mode. The Signal Manager's column browser (`#sigViewPresets`) is removed;
what the Signal Manager keeps is in §3.4.

### 3.2 The layout: sources on the left, signals on the right

Three columns were the wrong shape because the ECU/version choice is a
**car-level** fact, not a per-channel one. Nobody binds Coolant Temp to
"Haltech Nexus" and Oil Temp to "MaxxECU 1.3". The picker should open already
inside the car's ECU and let the user pick a signal; the ECU list is there for
the rare exception, not as step one of three.

```
┌ Choose a source for Coolant Temp ─────────────────────────────────────── × ┐
│ [ Search signals… (all sources)                                          ] │
├───────────────────────┬───────────────────────────────────────────────────┤
│ YOUR ECU              │  Link ECU · Generic Dash            41 signals    │
│ ● Link ECU            │  Version: [Generic Dash ▾]  (only when >1 and     │
│   Generic Dash        │            none is the car's active one)          │
│                       │                                                   │
│ OTHER ECUS            │  COOLANT TEMP          0x3E8 · frame 2     88 °C  │  ← in use
│   Haltech             │  ECT                   0x3E8 · frame 2       —    │
│   MaxxECU             │  IAT                   0x3E8 · frame 3     24 °C  │
│   ECU Master          │  OIL TEMP              0x3E8 · frame 8     95 °C  │
│   Ford BA/BF          │  …                                                │
│   Ford FG             │                                                   │
│   MegaSquirt          │                                                   │
│   Toyota 86 / BRZ     │                                                   │
│                       │                                                   │
│ THE CAR (OBD2)        │                                                   │
│ CUSTOM & DBC          │                                                   │
│ BUILT IN (RDM-7)      │                                                   │
├───────────────────────┴───────────────────────────────────────────────────┤
│ + Type in a CAN decode…        Link ECU › Generic Dash › COOLANT TEMP      │
│                                                     [ Use this source ]   │
└───────────────────────────────────────────────────────────────────────────┘
```

Rules:

- **Left rail** is grouped by the question ("which box is this from?"), with
  the car's active ECU first and marked, exactly the ordering
  `_renderSrcPickerColumns` already computes (`orderHint`, `is_active`). One
  entry per *make*; the version is a segmented control at the top of the right
  pane, shown only when a make has more than one version and none is active.
  Link's two streams are the motivating case: "Generic Dash | AiM Stream" is a
  question you answer once, not a column you scroll.
- **Right pane** is one flat list: label, frame id (+ `frame N` for a muxed
  row — the mux fields are already in the response since this month), live
  value with unit when the dash has it, and the same "in use" state the
  picker has today. The frame id stays — ADR-0035's rule, it is the one fact
  that tells two same-named decodes apart.
- **Search is global.** Typing filters every source at once and the right
  pane shows matches with their make as a prefix — that is how you find
  "coolant" without knowing whether your ECU calls it ECT. The rail collapses
  to only the makes with hits. `_renderSrcPickerColumns` already does the
  cross-make filter; it just throws the result away by requiring you to click
  the make afterwards.
- **In add mode** every row has a tick, the rail shows a count per make, the
  header gets "All / None", and the footer reads "Add 6 channels". This is the
  current `#chEcuImpModal` behaviour on the new layout.
- Keyboard: ↑↓ move, Enter picks, `/` focuses search. Mobile: the rail becomes
  a horizontal chip row above the list; nothing else changes.

### 3.3 "Type in a CAN decode…" lives in the picker

The one thing the picker lacks. It is the same form that "+ Add channels →
Custom channel…" opens (`_renderChNewForm`, probe and all), rendered **inside
the right pane** with the source rail dimmed, and its submit does the thing the
current context needs:

- **bind mode** — creates the signal (`/api/signal/update` with the decode,
  mux included) and binds *this* channel to it (`bind-source`, `custom`). This
  is what `_chAddCanSignal` does today, with a real form instead of a blank
  decode you edit afterwards. `_chAddCanSignal` and its "+ Add CAN signal"
  button under More are deleted.
- **add mode** — creates a new custom channel, same as the menu item does.

The Assign modal's "+ New Custom Signal" (`#assignStepCustom`) routes here
and its inline form is deleted. That removes items 5 and 10 from §1c and
leaves *one* decode form in the page.

### 3.4 The Signal Manager becomes the Library, and says so

ADR-0035 kept it as "the ECU-preset browser and custom-signal author". Make
that true in the UI:

- Rename the modal **Preset library**. Remove `#sigViewPresets`'s column
  browser and the "Add Signal" bar — it is not a picker any more.
- It lists custom presets (one row per ECU/version, signal count, `.dbc`
  export) with **+ New ECU**, **Create preset**, **Import DBC** and, per
  preset, **Edit** (opens `#presetAuthorModal` prefilled) and **Delete**.
  That is the authoring toolset that currently sits in `preset-col-actions`
  (`index.html:6674`), on a surface whose only job is authoring.
- Reached from exactly two places: Setup → Your car → ECU & CAN bus →
  "Manage presets", and the picker's rail under CUSTOM & DBC ("Manage…").
  The "Open Signal Manager…" links in the legacy widget signal dropdown
  (`index.html:16421`, `16494`) route to the picker in bind mode instead.

Items 7, 8, 9 of §1c stay, but on a surface named for what they do, reached
on purpose.

### 3.5 The drawer

**Width.** `#chShared.ch2-page .ch2-editor { width: clamp(520px, 40vw, 760px) }`.
Above 1400 px the list has room to spare and the drawer should take it. Keep
`max-width: 92vw` for phones. The modal mount (`.ch2-shell` 1100 px) keeps its
pane at `flex:1` as today.

**Order — outcome first.** Sections become, top to bottom:

1. **Header** — label, live value (as today).
2. **Source** — provenance line, *Change…* (opens the picker in bind mode),
   *Unbind*, *Calculate…*.
3. **Decode** — *only for CAN-sourced channels*, promoted out of More, with
   the **live bus probe and mux fields** from the new-channel form
   (`_chNewProbeRender` and the `.ch2-mux` disclosure, parameterised on the
   channel's decode instead of `_chNewDraft`). This is the single biggest
   usability win in the plan: the "is my bit offset right" answer currently
   exists only for the first ten seconds of a channel's life. Grid:
   `repeat(auto-fit, minmax(150px, 1fr))` — three columns at 520 px, two on a
   phone — and `select { min-width: 0; width: 100% }` so nothing truncates.
4. **Range & alerts** — as today.
5. **More** — Label, Group, Decimals only. Collapsed.
6. **Footer** — Duplicate · Reset to defaults · Remove/Delete (ADR-0034 words).

The "(applies when you Save)" note stays on Decode; the buffered-save model
does not change.

### 3.6 Naming: one string per ECU

Today the picker's version line is `"%s %s"` of make and version
(`web_server_channels.c:1218`) → "Link ECU Generic Dash", while the wizard,
the ECU & CAN bus page and the setup cards use `ecu_presets.c`'s
`.display` → "Link ECU (G4+ / G4X Generic Dash)". The same ECU has two names
depending on which door you came through.

Firmware change, small: in `channels_source_options_handler`, when a
(make, version) pair matches an `ecu_presets[]` entry, emit that entry's
`.display` as `display`. Fall back to the concatenation otherwise (RDM-7
GPIO rows have no wizard entry). Then `_ecuMakesFromBaked` does the same by
joining on the baked catalogue — which means the baked JSON needs the wizard's
display strings too; `tools/gen_channel_catalog.py` already compiles
`ecu_presets.c`'s neighbour, so add `display` to the emitted preset rows and
rerun the codegen (never hand-edit the block).

---

## 4. Parity: the dash, the browser, Studio offline

All three must show the same picker with the same data, or the UI cannot be
evaluated — this is how §1a happened.

- **Dev server.** Delete `DEV_ECU_MAKES`. Build the `source-options` mock's
  `makes[]` from the `presets` array inside the baked catalogue in the very
  `index.html` it serves (parse `window.RDM_BAKED_CATALOG = {...};` out of the
  file at startup — it is one line, JSON after the `=`). Mark one make
  `is_active` and sprinkle `live_value`s so the live column renders. Then the
  mock cannot drift from firmware: it *is* the firmware table, via the codegen
  guard in `schema-check.yml`.
- **The page.** `_openSignalPicker` gets the same `try / catch → baked`
  fallback the bulk picker has (`index.html:25713`) — trivially, once §3.1
  makes them one loader.
- **Studio.** `transport.js` `_localRouteApiCall`: route
  `/api/channels/source-options` to `{ makes: [], offline: true }` so the
  fallback fires, instead of throwing "unmapped endpoint". Same shape the
  `/api/channels` stub already returns and the reasoning in its comment
  applies word for word.

---

## 5. Phasing

Each phase ships on its own and is checkable without the next one.

| # | Work | Landed? |
|---|---|---|
| 0 | Dev server mock from baked catalogue; Studio local route; picker offline fallback | **Yes.** The mock now parses `window.RDM_BAKED_CATALOG` out of the HTML it serves. `bind-source` turned out not to be mocked *at all* — it fell through to a catch-all `{ok:true}`, so binds looked like they worked; now implemented faithfully, mux included. |
| 1 | Drawer width + decode promoted + probe/mux reused + grid/select fix | **Yes.** `clamp(520px, 40vw, 760px)`; selects went 89px → 190-290px; decode is its own section with the live probe. Exposed the unit bug (§ADR-0069). |
| 2 | One picker, two modes, new layout, global search | **Yes.** Rail + chips + global search. Exposed the false "in use" bug (§ADR-0069). |
| 3 | "Type in a CAN decode…" in the picker | **Yes**, routed to `_chAddCanSignal` for bind mode rather than a new form — the drawer's decode editor is now good enough to be the authoring surface, so a second one was not needed. `_chAddCanSignal` therefore survives (the plan had it deleted). |
| 4 | Signal Manager → Preset library | **Partly.** Renamed and reframed as authoring; its column browser stays, because it is how you choose which preset to edit. The plan's reroute of the two "Open Signal Manager…" links was dropped on inspection: they sit inside the legacy raw-signals panel, which is the right destination for them. |
| 5 | Display-name unification | **Not done, and largely moot.** With the make on a rail and the version on a chip, the picker reads "Link ECU" / "Generic Dash" — clearer than either of the two strings the plan proposed to unify. Revisit only if the wizard and the picker are seen side by side. |
| 6 | Desktop port, docs, ADR | **Yes.** Three-way merged (the desktop already carried this session's mux work, so a patch would not apply); 17 touched functions byte-identical. |

Phase 0 is an afternoon and should land first regardless — it is the
precondition for judging any of the rest by looking at it. Phase 1 is the
change Tommy will feel most. Phases 2–4 are one piece of work in three commits.

---

## 6. Not doing, and why

- **Not merging the picker into the drawer.** A source list of 40–70 rows with
  search wants the whole screen; the drawer's job is one channel's settings.
  They stay two surfaces, one opens the other.
- **Not touching `ecuPresetModal` (the ECU & CAN bus page).** It answers "is
  the dash talking to my car" with live match scores — a different question
  from "which signal feeds this channel", and ADR-0039 put it where it is on
  purpose. It gains the "Manage presets" door, nothing else.
- **Not changing `import-preset` or `bind-source`.** Both endpoints are right;
  the front end just reaches them through too many doors.
- **Not building a resizable drawer.** `clamp()` covers 1280–2560 px without
  a drag handle to get wrong.

## 7. Open questions for Tommy

1. In add mode, should picking a make with **no** active ECU offer "make this
   the car's ECU" (the wizard's `replace:true`), or stay strictly additive as
   ADR-0030 chose? Proposal: stay additive; the chip on the context strip is
   the place to declare the car's ECU.
2. Should the Preset library be reachable from the top bar at all, or only
   from Setup? Proposal: Setup only — it is a once-a-year tool.
