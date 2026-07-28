# RDM Studio shell plan — navigation, and where lap timing lives

**Status:** proposal, 2026-07-27. Extends `PLATFORM_PLAN_2026-07.md` §5.3
("Suite shell"), which specified a device-tree shell that was never built.
Applies ADR-0011; **retires a placement rule wrongly attributed to ADR-0007**
(§2.0); leaves ADR-0007's actual subject intact.

Written after adding the RDM GPS workspace, which is when the seams started
showing.

**Strategic premise (owner, 2026-07-27):** RDM Studio is the star of the show —
the single connection to every product. The differentiator is integration and
software ease of use, not the device's built-in web UI.

**A mobile app is planned before release.** Desktop is the focus now, but the
device must be able to talk to either client. That reframes the device as an
**API with interchangeable clients** (§6) — and it dissolves, rather than
merely shrinks, the shared-HTML problem ADR-0007 documents.

---

## 1. What's actually wrong

### 1.1 The Home grid holds three different kinds of noun

Six cards, presented identically:

| Card | What it really is |
|---|---|
| RDM-7 Dash | a **device** (and the layout editor) |
| CAN Keypad | a **device** configurator |
| IO Expander | a **device** configurator |
| RDM GPS | a **device** configurator |
| GPS Lap Timer | a **capability** — needs the dash's engine *and* the puck's data |
| CAN Bus Analyzer | a **tool** — an instrument, not a thing you own |

The visible symptom is that "GPS Lap Timer" and "RDM GPS" now sit next to each
other and nobody can tell which one to open. That is not a naming accident to be
patched with better words; it is a category error becoming legible. A flat grid
of peers can't express "this capability runs *on* that device".

It will get worse on schedule: the platform plan has IO shipping as **three**
SKUs (Pico / Core / Pro), plus fleet update, plus `.rdmcar` car projects. That's
a 10+ card grid of mixed nouns.

### 1.2 The lap timer workspace is fiction — and the real one already exists

`ltWorkspace` is 1396 lines and makes **zero** calls to `/api/lap/*`. It
synthesises laps with a curvature-limited speed model and `Math.random()`
per-lap jitter, then renders lap times, sector splits, a predictive delta, an
analysis graph and a broadcast HUD from them. Its inspector subtitle reads
*"lap engine on the dash"* while every number on screen was computed in the
browser.

Meanwhile the firmware has the real thing. Commit `e80452d`
(*"release: 1.1.29 — lap timing, …"*) added a **Lap Timing** modal to
`main/web/index.html` backed by six real endpoints:

```
GET  /api/lap/status          presence, fix, track, live timing
GET  /api/lap/track           full track detail
POST /api/lap/track           replace track / clear
POST /api/lap/capture         set a line from the car's live position + course
POST /api/lap/session/reset   clear times, keep track
POST /api/lap/gps             (test injection)
```

**The desktop has not synced it.** `src/firmware-base.commit` is pinned at
`c3241a0`, exactly one `index.html` commit behind — and that commit is the lap
timing release. So Studio is hand-maintaining a fictional lap timer while the
real one — and the API behind it — sits one `sync_firmware.py` away.

This is ADR-0011's anti-pattern, one workspace over. ADR-0011 (accepted two days
ago) removed synthetic traffic from the CAN analyzer because *"the one moment a
diagnostic tool must be loudest is exactly when it cannot see the thing it is
diagnosing."* Every word applies here, and the stakes are higher: a fabricated
`1:23.45` with a predictive delta is precisely the screenshot a customer would
post, and lap accuracy (±0.02 s) is the product's headline claim. Shipping both
the ADR and the sim would be incoherent.

### 1.3 Studio can only hold one device at a time

`RDM._transport` is a single slot and `setMode()` replaces it. Connecting the
puck over USB **disconnects the dash**. But "This car" — the shell the platform
plan specifies — needs both at once, and lap timing inherently spans them.

Physically there is no problem: the dash is on WiFi and the puck is on USB, two
different pipes. This is purely a software limitation, and it is the single
thing most blocking the shell.

---

## 2. The organising idea

### 2.0 First, retire a rule that was never real

`PLATFORM_PLAN` §5.3 states a **placement rule** — *"device-config workspaces
(keypad, IO, lap setup) belong in the firmware web editor first … the desktop
inherits them for free via ADR-0007 sync"* — and ADR-0008's "Who owns what"
repeats it as *"Per ADR-0007 the lap configuration UI belongs in the firmware
editor first."*

**ADR-0007 does not say this.** It is 92 lines about exactly one problem: three
copies of the *dash layout editor* HTML drifting apart. It never mentions
workspaces, device configurators, or where new UI should be authored. The rule
was attributed to it, not derived from it.

The codebase never obeyed it either:

| Workspace | authored in `firmware-base.html` | authored in `tauri-overlay.html` |
|---|---|---|
| Keypad, CAN Analyzer, IO, Lap Timer, RDM GPS | **0** | **5** |

So this plan **retires the placement rule** rather than working around it. Both
citations should be corrected at the source (§9).

**What ADR-0007 actually protects, and which still stands:** the dash layout
editor is one ~22k-line artifact that runs both inside Studio and served off the
device over WiFi. That fork risk is real and mechanical. It is the *only* thing
the sync pipeline is for.

**The replacement policy:**

> **The device serves an API. Clients are separate products.**
>
> - Every configurator, workspace and analysis surface is authored in its
>   client — Studio now, the mobile app later — and built to be the best
>   version of itself. No mirroring obligation between them.
> - The device's job is the **API**, plus running the car. Its embedded HTML is
>   a limp-home page, not a client: explicitly allowed to be plain, frozen from
>   now, reduced at mobile launch (§6.1).
> - Devices still **store and run** their own configuration — the dash works
>   with no PC and no phone attached. That is about where config *lives*, not
>   where the UI that authors it lives.
> - Every client speaks the same contract over any transport. One API, N
>   clients, N transports — never a second model of the data (§6.3).
>
> Consequence: the three-copy problem **dissolves**. The dash editor stops being
> a shared artifact the moment it stops being the phone's only way in.

### 2.1 Three nouns

Studio has exactly three nouns. Name them, separate them, and every future
placement question answers itself.

- **Devices** — things you own and configure. Dash, RDM GPS, Keypad, IO.
- **Capabilities** — what the car *does*. Lap timing, logging, night mode.
  They belong to whichever device *runs* them, and nest under it.
- **Tools & sessions** — desktop work that isn't about one device. CAN
  analyzer, session analysis, fleet update, car projects.

This is a **layout** taxonomy — where a thing appears in the shell — not an
authoring one. Authoring is settled by §2.0: Studio owns all of it. Capabilities
nest under the device that *runs* them, which is a statement about the
architecture, not about which repo the UI lives in.

The one discipline that survives: where Studio and the device both touch a
feature, they share the **API**, never a second model. A big-screen map for
dragging the start/finish line is Studio being better; a second source of truth
for what a track *is* would be a bug. Same endpoints, same JSON, bigger canvas.

---

## 3. Where each piece of lap timing goes

Today's single "GPS Lap Timer" card is three separable things:

| Piece | Home | Status |
|---|---|---|
| Track library, big-map line placement, sectors, capture-from-car | **Studio** — the good one | to build (reuses the existing Leaflet map) |
| Live lap state — fix, current/last/best, delta, sector splits | **Studio** — from `/api/lap/status` | to build (replaces the simulator) |
| Puck health & config — fix, sats, link, WiFi, identify | **RDM GPS** workspace | **built** (2026-07-27) |
| Session analysis — delta-T, track map, two-lap overlay, CSV | **Studio, desktop-only** | **not built — the actual prize** |
| Minimal track-side track/gate edit, no laptop | **Dash** — existing firmware modal | **built in firmware**, becomes the fallback |

Per §2.0, Studio owns the lap timing experience outright and is built to be the
best version of it. The firmware's existing Lap Timing modal is *kept* — it is
the limp-home surface for someone at a track day with no laptop — but it stops
being the canonical UI, and **Studio hides it from its own menus** so the app
never shows two lap UIs. (Precedent: the desktop already omits the firmware's
Custom PIDs editor — ADR-0007 §"three copies", intentional omission.)

Both talk to the same six `/api/lap/*` endpoints. The dash still stores the
track and runs the engine, so the car works with no PC attached.

So "integrating the track timer" is:

1. Sync `e80452d` — for the `/api/lap/*` surface and 1.1.29's editor fixes, not
   to inherit a lap UI as canonical. Hide the inherited modal from Studio's menu.
2. Delete the synthetic engine (~600 lines) → ADR-0011 consistency.
3. Keep the map; point it at real data; build the track editor Studio deserves.
4. Build **Sessions** — §6.2's "anti-i2", and the part that was always
   desktop-only.

**Honest dependency:** Sessions needs session files, and ADR-0008 still lists
*"session logging to SD with a lap/sector index"* under **Still to build**. So
Sessions cannot ship before that firmware work. Don't let the roadmap pretend
otherwise — see §7.

---

## 4. The shell

Replace the flat grid as *primary navigation* with a persistent left sidebar.

```
┌─ THIS CAR ─────────────────┐
│ ● RDM-7 Dash   192.168.1.42│   ● live   ○ known, absent   · never seen
│     Layouts                │
│     Channels               │
│     Lap Timing             │   ← capability, nested under what runs it
│ ● RDM GPS      COM37       │
│ ○ CAN Keypad   not detected│
│ · RDM IO                   │
├─ TOOLS ────────────────────┤
│   CAN Bus Analyzer         │
│   Sessions                 │
│   Fleet Update             │
└────────────────────────────┘
```

Why this shape:

- **Capabilities nest under their device.** "Lap Timing" sitting under "RDM-7
  Dash" states the architecture — the engine runs there — and permanently kills
  the "is Lap Timing a device?" confusion.
- **Presence is visible and honest.** A device that isn't there is shown absent,
  not hidden and not faked. Same principle as ADR-0011.
- **Tools are separate**, because they're always available and aren't about one
  device.
- It scales to the three IO SKUs and to fleet/car-project work without becoming
  a wall of cards.

**Home survives, demoted.** The grid is genuinely good as a first-run and
product-discovery surface — "here's what RDM makes, here's what you could add".
It is bad as everyday navigation. Keep it reachable (⌂ / Esc, as now), stop
making it the only way in.

**Naming changes:**

| Now | Becomes | Why |
|---|---|---|
| GPS Lap Timer | **Lap Timing** | a dash capability; the GPS is the puck, not the feature |
| RDM GPS | RDM GPS | correct already — it's the product |
| — | **Sessions** | the desktop-only analysis workspace |
| Device menu (7 mixed items) | **Devices** / **Tools** | mirrors the sidebar |

---

## 5. The enabling change: more than one device at a time

`RDM._transport` (one slot) → a small registry:

```js
RDM.devices = { dash: <transport|null>, gps: <transport|null>, … }
RDM.active                  // what the editor addresses by default
RDM.get('gps')              // explicit addressing for a device workspace
RDM.transport               // getter → active, so nothing existing breaks
```

- `proxyApiCall` keeps routing `/api/*` to the **dash** slot — the firmware
  editor's contract is untouched, which is what makes this safe.
- The GPS workspace addresses `RDM.get('gps')` explicitly instead of relying on
  a global mode.
- `deviceType` stops being a single global (it already exists per-connection
  from the 2026-07-27 work; this just moves it onto the slot where it belongs).

Contained almost entirely in `transport.js` plus the connect flow. This is the
prerequisite for the sidebar, for lap timing spanning two devices, and for fleet
update later.

**What this is *not*:** it does not replace the dash-as-gateway
(`GET /api/bus/devices`, platform plan §5.1). Keypad and IO are CAN-only — they
cannot be reached from a PC at all without the dash relaying. The registry is
the near-term win for dash + GPS, which are directly reachable *today*. Both are
needed; they solve different halves.

---

## 6. The device becomes an API, not a UI

A second client is the forcing function. Everything below is what "the device
can talk to either one" actually requires.

### 6.1 This dissolves the three-copy problem

The **only** reason the device serves a 24k-line HTML editor is that a phone had
no other way in. A native mobile app removes that reason.

So ADR-0007's problem doesn't shrink — it **dissolves**:

| | Before | After |
|---|---|---|
| Desktop UI | copy of the firmware editor + overlay | Studio owns its frontend |
| Phone UI | the device's embedded editor | the mobile app owns its frontend |
| Device | serves the canonical UI | serves an **API**, plus a small emergency page |

The dash editor stops being a shared artifact. That is the single largest
maintenance burden in this system, and this steer deletes it. **§8.3 is
therefore settled**: the device keeps only a genuine limp-home page, and it is
allowed to be plain.

**Sequencing caveat — do not rip it out now.** Until the mobile app ships, the
embedded editor *is* the phone story. Freeze it (bug fixes only, no new
features) rather than deleting it, and reduce it to emergency-only at mobile
launch. Studio should stop inheriting it before then.

### 6.2 There is no API contract today

Measured on the firmware:

| | Count |
|---|---|
| HTTP endpoints | **131** |
| Serial RPC methods | **53** |
| Documents specifying either | **0** |

The only specification of the device's API is the 24k-line HTML that consumes
it. A second client cannot be built against that.

Worse, the two transports are not two views of one API — they are two
partially-overlapping APIs that grew separately. Whole families are
HTTP-only:

```
obd2 (11)   channels (10)   splash (6)   lap (6)   can (6)   ecu (5)   canraw (5)
```

That's ~53 endpoints a USB-connected client simply cannot reach. And where both
exist, they have drifted in *shape*: the sim toggle takes `{enabled}` over HTTP
and `{enable}` over serial, and replies `{enabled}` vs `{active}` — a real bug
that cost a debugging session and is commented as such in `transport.js`. With
one client that's an irritation. With two it's a permanent tax.

### 6.3 Make serial a tunnel, not a second API

The fix is cheap and removes code rather than adding it:

```
serial frame payload  →  { "method": "GET", "path": "/api/lap/status", "body": … }
                      ←  { "status": 200,  "body": … }
```

One handler table in the firmware, dispatched by path, reachable over HTTP *and*
serial. Consequences:

- The 53 hand-written RPC methods collapse into the existing HTTP handlers —
  and the USB client immediately gains the ~53 endpoints it can't reach today.
- **`_usbRouteApiCall` disappears.** That's ~300 lines in `transport.js` whose
  entire job is hand-translating paths to a divergent RPC vocabulary, including
  the `{enabled}`/`{enable}` fixups. It exists only because the vocabularies
  differ.
- Shape drift becomes structurally impossible: one handler, one contract.
- The GPS puck's RPC stays as it is — it has no HTTP server and only six
  methods. Not everything needs this; the dash does.

### 6.4 Discovery is a mobile problem the desktop doesn't have

There is **no mDNS in the firmware** (removed 2026-04-27). Studio finds a dash by
sweeping every local /24 — fine on a PC with mains power and a real network
stack.

A phone cannot do that. iOS requires the Local Network permission, both platforms
throttle background scanning, and a 254-address sweep on cellular-class radios is
slow and battery-hostile. So the mobile app needs one of:

- **mDNS back in the firmware** (`_rdm._tcp`) — the conventional answer, and it
  also speeds up desktop rediscovery;
- **the dash's own AP** + QR-code pairing — no network required, best for
  first-run and track-side;
- both (AP for setup, mDNS once joined).

This has firmware lead time, so it wants deciding well before the app is built —
but it does not block any desktop work.

### 6.5 Versioning and access, once there are two clients

- **Capability negotiation.** `device.info` returns `schema` today; it needs an
  explicit **API version** and capability list. Two clients on independent
  release cadences make `PLATFORM_PLAN` §4's own "no version lockstep, speak N
  and N−1" guardrail mandatory rather than aspirational.
- **Access control.** The API is currently unauthenticated on the local network.
  One laptop on a workshop LAN is one risk profile; a phone app on a paddock or
  home network is another. Not urgent while desktop-only — but decide it before
  the app ships, not after.

---

## 7. Phasing

> **Built 2026-07-27** (in `rdm7-desktop`, uncommitted): A2, A3, B1, and the
> GPS half of B2. Specifically — the device registry (`RDM.attach` / `RDM.get`)
> so a dash on WiFi and a puck on USB are live at the same time; the RDM GPS and
> GPS Lap Timer cards merged into one **RDM GPS & Lap Timing** workspace; a live
> satellite map showing the puck's position and the dash's track and gates; lap
> state read from the real `/api/lap/status`; capture-from-car and session reset
> wired to the real endpoints; and the 1596-line synthetic lap engine deleted.
> Remaining in A: A0 (correct the documents), A1 (the sync), A4 (widen ADR-0011).
> Remaining in B: the full "This car" sidebar and Home demotion.

Ordered so each phase is shippable and nothing depends on vapour.

**Phase A — truth, and correcting the record** *(small; mostly deletion)*
- **A0.** Correct the misattributed placement rule at both sources —
  `PLATFORM_PLAN` §5.3 and ADR-0008's "Who owns what" — and record the §2.0
  policy. Cheap, and it stops the next person re-deriving the wrong constraint.
- **A1.** `python tools/sync_firmware.py` → pull `e80452d` for the `/api/lap/*`
  surface and 1.1.29's editor fixes. Re-anchor any overlay blocks the merge
  flags; that's the drift detector working. Add an overlay block hiding the
  inherited Lap Timing modal from Studio's Setup menu.
- **A2.** Strip the synthetic lap engine from `ltWorkspace` (~600 of its 1396
  lines). Wire the map and timer bar to `/api/lap/status` + `/api/lap/track`.
  Where there's no dash, say so — don't animate a car.
- **A3.** Rename to **Lap Timing**; split the native menu into Devices / Tools.
- **A4.** Widen ADR-0011 from "the CAN analyzer never fabricates traffic" to
  "no RDM Studio workspace fabricates data" — we've now hit it twice, which is
  the point at which it's a policy rather than an incident.

*Ships:* a lap timer that tells the truth, and a written policy that matches
both the strategy and what the code already does.

**Phase B — the shell** *(medium; the real refactor)*
- **B1.** Transport registry (§5).
- **B2.** "This car" sidebar with live presence; dash + GPS connected at once.
- **B3.** Home demoted to first-run / product picker.

*Ships:* the shell §5.3 specified; puck and dash usable together, which is the
first time lap timing can be set up end-to-end in one view.

**Phase B′ — API contract** *(medium; pays for itself before mobile exists)*

Runs alongside B. Everything here helps the desktop *today*; it just happens to
be what a second client will need.

- **B′1.** Generate an API reference from the firmware's handler registrations —
  extract, don't invent. 131 endpoints, currently undocumented. Also produces the
  HTTP-vs-serial coverage diff mechanically.
- **B′2.** Serial becomes a tunnel carrying `{method, path, body}` (§6.3). Kills
  the 53 shadow RPC methods and ~300 lines of `_usbRouteApiCall`, and gains USB
  clients the ~53 endpoints they can't reach today.
- **B′3.** `device.info` carries an explicit API version + capability list, and
  Studio checks it (§6.5). This is `PLATFORM_PLAN` §4's own N/N−1 guardrail,
  which nothing currently enforces.
- **B′4.** Freeze the device's embedded editor: bug fixes only, no new features.
  Studio stops inheriting it (it already owns every workspace).

*Ships:* a device whose interface is written down and identical over both
transports — and a smaller `transport.js` than we started with.

**Phase B″ — track editing on the map** — **built 2026-07-28**

- **B″1. Done.** A **Tracks** view in the GPS workspace: gates are placed at the
  map centre then edited with two handles — drag the centre to move, drag an end
  to aim and size at once (the line you draw *is* the line the car crosses, so
  there is no heading spinner to reason about). Heading is stored as the
  perpendicular, which is what the dash wants. Send to / read from the dash over
  `/api/lap/track`, with the echoed `sector_count` checked against what was sent
  so a silently-dropped split is reported rather than assumed stored.
- **B″2. Partly done.** A local track library with import/export, seeded with
  five real circuit *locations* (map centres only — no invented start/finish
  coordinates). **Migration from the removed workspace's `rdm7_laptimer_v2` /
  `rdm7_track_libs` is implemented**, converting its full-width gates to the
  dash's half-widths, so the tracks flagged as unreachable in the merge are
  recovered. Still missing: the ~100-circuit seeded DB and proximity
  auto-selection from `PLATFORM_PLAN` §6.2.

**Phase B‴ — sprint / hillclimb needs firmware** *(not started; scope note)*

`lap_track_t` holds **one** `start_finish` line plus splits, and `lap_core.c`
closes a lap only by RE-crossing it. A point-to-point course with a separate
start and finish therefore cannot be timed at all today. The removed workspace
drew a green start and a chequered finish for a `type: "sprint"` track, but that
only ever fed its own simulator — no firmware ever consumed it. Studio now says
so plainly rather than offering a mode the hardware doesn't have. Supporting it
means a second line and a run-based (not lap-based) timing path in `main/lap/`.

**Phase C — Sessions** *(the prize; has a firmware dependency)*
- **C1.** *Firmware:* session files to SD with a lap/sector index
  (ADR-0008 "Still to build"). **Blocks C2 — do not start C2 first.**
- **C2.** *Studio:* Sessions workspace — auto-download over WiFi, delta-T plot vs
  reference, track map with sector colouring, two-lap overlay, CSV export.
  Explicitly out of v1: video overlay.

**Phase D — later, per the platform plan**
- Fleet firmware manager (needs B1's registry).
- `.rdmcar` car projects.
- `bus_manager` gateway → Keypad and IO appear in the sidebar for real.

**Phase M — mobile enablement** *(not now; listed so it isn't a surprise)*
- **M1.** Discovery: mDNS and/or AP + QR pairing (§6.4). **Firmware lead time —
  decide before the app is designed, not during.**
- **M2.** API access control (§6.5).
- **M3.** Embedded editor reduced to a limp-home page.

None of Phase M blocks desktop work. It is here because M1 and M2 are firmware
changes with long lead times, and finding them late is what makes a second
client expensive.

---

## 8. Decisions

**Settled 2026-07-27 (owner):**

- **8.1 — Studio is the star.** The placement rule is retired; Studio owns every
  configurator and is built to win on integration and ease of use. §2.0.
- **8.2 — Studio's lap UI is the full experience**, not a read-only view. The
  firmware modal is demoted to track-side fallback and hidden from Studio's
  menus. §3.
- **8.3 — The device serves an API; clients are separate products.** A mobile
  app is planned before release, so the embedded editor is no longer the phone
  story. It freezes now and reduces to a limp-home page at mobile launch. The
  three-copy problem dissolves rather than shrinks. §6.1.
  *Desktop is the focus until then — none of §6 blocks desktop work.*

**Still open:**

- **8.4 — Does Home survive?** Assumed yes-but-demoted: good for first run and
  for showing the product line. Alternative is booting straight into the shell.
- **8.5 — Does Sessions jump the queue?** The most sellable thing here
  ("free, modern, cross-platform, the anti-i2"), gated behind firmware SD work.
  Worth pulling C1 ahead of Phase B?
- **8.6 — Is "no synthetic data" absolute?** A single loudly-labelled demo mode
  for trade shows, or no, permanently, everywhere?
- **8.7 — Mobile discovery: mDNS, AP + QR, or both?** Firmware lead time, so
  worth deciding early even though it blocks nothing now. §6.4.
- **8.8 — Does the API get authentication before the app ships?** §6.5.

## 9. Documents to correct

Because the rule was cited, not invented, it has to be unwound where it's written (see §9):

| File | Change |
|---|---|
| `PLATFORM_PLAN_2026-07.md` §5.3 | Replace the "Placement rule" paragraph with the §2.0 policy |
| `adr/0008-gps-lap-timing-integration.md` | "Who owns what" — drop *"Per ADR-0007 the lap configuration UI belongs in the firmware editor first"*; lap config UI owner becomes Studio |
| `adr/0007-html-source-of-truth.md` | Add a scope note: this ADR is about the **dash layout editor** only, and has never governed workspace placement |
| `adr/0011-analyzer-no-synthetic-data.md` | Widen to all Studio workspaces (A4) |

---

## 10. What I'd do first, if it were one thing

Delete the lap simulator and point the map at `/api/lap/status`. It removes ~600
lines we'd otherwise keep paying for, it stops the app asserting lap times it
invented, and it's the prerequisite for everything else here.

The sync (A1) rides along because it's one command and it brings the API surface
— but the *reason* has changed since the first draft of this document. It is no
longer "inherit the canonical lap UI". Studio is building the canonical one.
