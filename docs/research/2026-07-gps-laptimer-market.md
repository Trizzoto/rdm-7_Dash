# GPS Lap Timer Market Research — July 2026

> Research input for the RDM platform plan (`docs/PLATFORM_PLAN_2026-07.md`).
> Compiled 2026-07-10 from vendor documentation, distributor pricing, and forum threads.

## 1. Standalone lap timer hardware

| Product | GNSS | Price | Notes |
|---|---|---|---|
| **AiM Solo 2** | 25 Hz, 4-constellation (GPS/GLONASS/Galileo/BeiDou), 100 Hz 6-axis IMU | ~US$400–470 / £365 (~A$700) ([aim-sportline](https://www.aim-sportline.com/en/products/solo2-solo2dl/index.htm), [aimshop](https://www.aimshop.com/products/aim-solo-2-gps-track-day-racing-lap-timer-and-data-logger)) | Predictive lap + sector times to 1/100s, >4,000-track DB with auto track recognition, WiFi download, 4 GB, shift-light LEDs |
| **AiM Solo 2 DL** | same | ~US$700–760 ([winecountry](https://winecountrymotorsports.com/products/aim-solo-2-dl-gps-lap-timer)) | Adds ECU connection (CAN/OBD2/RS232) so GPS laps are logged alongside engine data — the "DL" is the bridge product between lap timer and data logger |
| **RaceBox (orig.)** | 10 Hz | $399 ([store](https://www.racebox.pro/store/)) | Standalone with display; drag + lap modes |
| **RaceBox Mini / Mini S / Micro** | 25 Hz u-blox-class, 4 constellations | $219 / $289 / $129 ([store](https://www.racebox.pro/store/)) | BLE pucks for phone apps (RaceChrono/Harry's); Mini S/Micro add standalone storage; claims "10 cm" relative precision, no display, no CAN |
| **Garmin Catalyst 2** (Feb 2026) | 25 Hz multi-GNSS | US$1,199 ([Garmin PR](https://www.garmin.com/en-US/newsroom/press-release/automotive/optimize-time-on-the-track-with-the-cutting-edge-garmin-catalyst-2/)) | Camera + audible AI coaching, "True Optimal Lap" composite video, cloud Vault; gen-1 was 10 Hz, £899, 15.4 oz ([CAR review](https://products.carmagazine.co.uk/car-accessories/technology/garmin-catalyst-review/)) |
| **VBOX LapTimer** | 25 Hz GNSS | US$1,085 ([store](https://www.vboxmotorsport.store/index.php?route=product/product&product_id=36)) | OLED display, position-based (not distance) predictive delta, 6 RGB delta LEDs, SD logging → Circuit Tools |
| **VBOX Sport** | 20 Hz | ~$580, discontinued ([EDO](https://www.edoperformance.com/products/racelogic-vbox-sport-performance-lap-time-data-logger)) | Later units quietly dropped to 10 Hz — community noticed and complained ([Harry's forum](http://forum.gps-laptimer.de/viewtopic.php?t=5452)) |
| **Starlane Corsaro-II R** | 10 Hz GPS+GLONASS+Galileo | ~$429 ([MOTO-D](https://www.motodracing.com/starlane-corsaro-ii-r-race-yamaha-r6-2017-gps-lap-timer-wireless-data-logger)) | Bike/kart touch-screen; predictive, 3 splits, auto track recognition, wireless RPM/suspension modules, MAAT software |
| **Alfano 6/7** | 10 Hz | ~$560 (Alfano 7 1T) ([Accelerationkarting](https://www.accelerationkarting.com/shop/alfano-7-2t-gps-lap-timer-data-logger-4654)) | Kart standard: RPM/temp inputs, 6 RGB LEDs, Bluetooth download |

Takeaway: 25 Hz + 4 constellations + IMU is now table stakes at $219 (RaceBox Mini); AiM's moat is track DB + ecosystem; VBOX/Garmin sell at 2–3× on brand, video and coaching.

## 2. Dash-integrated lap timing (RDM's direct competition)

- **AiM MXS 1.2/1.3 (Strada)** — lap timing via the **GPS09 module** (bus-connected roof puck, ~sub-0.5 m position, auto start/finish recognition from AiM's track DB) or optical beacon ([GPS09](https://www.aimshop.com/products/aim-gps09-gps-module), [MXS Strada](https://www.aimtechnologies.com/mx-strada-series/)). Strada (display-only) vs full MXS (logger) split; dash ~US$1.3–2.2k. Config in Race Studio 3; Link ECU resells the same hardware through its dealer network ([dealers.linkecu.com/GPS09](https://dealers.linkecu.com/GPS09)).
- **MoTeC C125/C1xx** — 10 Hz GPS ships in race kits; GPS lap timing "never again put out an infrared beacon"; track maps + driven-line comparison in i2 ([motec.com.au](https://www.motec.com.au/c125/c125overview/)). Kits ~$3.2k+, typical spend $4.6–5.4k with options unlocked — logging and **i2 Pro are paid unlocks** ([Rennlist](https://rennlist.com/forums/racing-and-drivers-education-forum/778797-aim-vs-motec-2.html)).
- **ECUMaster ADU5/ADU7 + GPS-to-CAN V2** — closest analogue to RDM's plan: **$313–479** CAN GPS module, 25 Hz, 4 constellations, integrated accel+gyro broadcast over CAN; predictive lap timing via a "Qualification Mode" page using an auto-updated best reference lap; no beacon needed; track-map visualization in ADU software ([ecumaster.com](https://www.ecumaster.com/products/gps-to-can-v2/), [ECUMaster USA](https://ecumasterusa.com/products/gps-to-can-with-imu-v2)). ADU5 ~$1,429–1,599. Friction point: the GPS module itself **must be programmed via a separate USB-CAN interface** (ECUMaster USB2CAN/Kvaser/Peak).
- **Haltech iC-7** — no internal GPS; plug-and-play **HT-011310 GPS Speed Input module** ("calibrates with a simple check box"), and the NSP software update added lap times, track position and predictive lap timing to the dash ([haltech.com](https://www.haltech.com/product/ht-011310-gps-speed-input-module/), [iC-7](https://www.haltech.com/product-category/digital-displays/ic-7-digital-dash/)). The newer **uC-10 shipped without lap timing** ([support.haltech.com](https://support.haltech.com/portal/en/kb/articles/haltech-uc-10-display-dash)) — a gap users notice.
- **Plex SDM-550** — the premium "built-in" reference: 25 Hz multi-GNSS **and** 100 Hz IMU inside the dash, predictive delta vs best lap, 256 log channels, IP67, Dakar-proven ([plex-tuning.com](https://www.plex-tuning.com/product/plex-sdm-550/)).

Feature baseline every competitor dash offers: auto track detection against a bundled DB, best/last/predictive lap, sector/split times, beacon-free GPS start/finish, delta shown via LEDs or a dedicated race page.

## 3. Software / analysis

- **AiM Race Studio 3 + RS3 Analysis** — Windows-only; deep (math channels, GPS track maps, video sync) but long-time users call it "clunky" vs RS2 ([Rennlist thread](https://rennlist.com/forums/data-acquisition-and-analysis-for-racing-and-de/1290804-race-studio-3-analysis-released-to-production.html)); no macOS is a recurring complaint.
- **MoTeC i2** — the pro benchmark (delta-T, driven-line overlay on Google Earth, track maps); i2 **Pro is licensed per device/PC** ([motec.com.au/i2](https://www.motec.com.au/i2/i2overview/)).
- **VBOX Circuit Tools** — deliberately simple, "by racing drivers for racing drivers", auto video sync, **Windows + macOS + iOS** ([vboxmotorsport](https://www.vboxmotorsport.co.uk/en/vbox-laptimer)) — the usability counter-example to RS3/i2.
- **RaceChrono Pro** — ~$25 app, 100k+ active users, >2,600-track library, supports RaceBox/Dragy/Qstarz/VBOX BLE GPS + OBD-II sync, video overlay ([racechrono.com](https://racechrono.com/article/faq/which-external-gps-should-i-buy), [Play Store](https://play.google.com/store/apps/details?id=com.racechrono.pro&hl=en)). Documented DIY/BLE ecosystem (ESP32 projects like [bonogps](https://github.com/renatobo/bonogps) target it).
- **TrackAddict (HP Tuners)** — free-ish, video overlay via RaceRender; complaints: crashes mid-log, picky OBD adapters, phone GPS inadequate ([reviews](https://appgrooves.com/app/trackaddict-by-racerender-llc/negative)).
- **Garmin Catalyst** — anti-analysis positioning: no channel plots, just opinionated coaching ("brake earlier into the next right") and session summaries; gen 2 adds export/social ([Hagerty](https://www.hagerty.com/media/motorsports/review-garmin-catalyst/)).

What club racers actually use: delta-T vs reference lap, GPS track map with sector coloring, two-lap overlay, video with data overlay, CSV export. What they hate: Windows-only, per-seat licenses, ugly 2000s UIs.

## 4. Track databases

- AiM Track Manager: ~**4,160 tracks worldwide / 1,680 NA** (2020 count), proprietary, all surface types; start/finish + splits stored per track file, user-editable, custom tracks supported ([AiM webinar PDF](https://www.aimsports.com/webinars/Documents/May2020TrackMapsAiMWebinar.pdf), [RS3 docs](https://www.aim-sportline.com/docs/racestudio3/manual/html/tracks.html)).
- RaceChrono: community-submitted library (>2,600 tracks) + user-defined start/finish and splits in-app.
- Open data: [TUMFTM racetrack-database](https://github.com/TUMFTM/racetrack-database) (centerlines for ~20 F1/DTM tracks), OpenStreetMap raceway ways (ODbL — attribution + share-alike if you derive your DB from it). Practical pattern: a start/finish line is just two coordinate pairs; every vendor curates its own list, and community submission (RaceChrono model) scales it cheaply. Don't scrape AiM's DB — proprietary.

## 5. GNSS module reality check

- **u-blox M8 = 10 Hz max; M9/M10 = 25 Hz** ([RaceChrono FAQ](https://racechrono.com/article/faq/which-external-gps-should-i-buy)). NEO-M9N/MAX-M10S modules are single-digit-to-$30 parts; this is exactly what RaceBox ships at $129–219 retail.
- Dual-band RTK (ZED-F9P) does 25 Hz raw and cm-level with corrections, but boards run ~$180–260 ([SparkFun](https://www.sparkfun.com/sparkfun-gnss-combo-breakout-zed-f9p-neo-d9s-qwiic.html), [Ardusimple](https://www.ardusimple.com/product/simplertk2b/)) — overkill for lap timing v1.
- Credibility floor: **10 Hz is the accepted minimum** (Starlane/Alfano/MoTeC kits still sell 10 Hz); **25 Hz is the current marketing standard** (Solo 2, RaceBox, VBOX, ECUMaster V2, Plex, Catalyst 2). Forum consensus: constellation count/accuracy matters more than raw Hz for lap-time precision; 25 Hz mainly buys smoother traces and better line analysis ([Harry's forum](http://forum.gps-laptimer.de/viewtopic.php?f=9&p=2213)). Typical claims: ~0.5 m position (Solo 2/GPS09), lap times ±0.01–0.02 s. An IMU (100 Hz) is bundled by AiM/ECUMaster/Plex for g-plots and dead-reckoning between fixes.

## 6. User pain points (forums/reviews)

1. **AiM WiFi/firmware fragility** — Solo 2 freezing on WiFi screen, "bricked" mid-update ([miata.net](https://forum.miata.net/vb/archive/index.php/t-736176.html), [bimmerpost](https://f80.bimmerpost.com/forums/showthread.php?t=1817763)).
2. **False/missed laps** — Solo 2 recording 86 phantom laps on a bike, AiM support acknowledging GPS issues ([r1-forum](https://www.r1-forum.com/threads/my-mistake-or-malfunctioning-aim-solo2-timer.639338/)).
3. **Windows-only, dated analysis software** — RS3 "clunky" ([Rennlist](https://rennlist.com/forums/data-acquisition-and-analysis-for-racing-and-de/1290804-race-studio-3-analysis-released-to-production.html)); no macOS support for AiM.
4. **Paid analysis unlocks** — MoTeC i2 Pro/logging licenses push a C125 install to $4.6k+ ([Rennlist](https://rennlist.com/forums/racing-and-drivers-education-forum/778797-aim-vs-motec-2.html)).
5. **Config requires extra hardware** — ECUMaster GPS-to-CAN needs a USB-CAN dongle just to program it ([ecumaster.com](https://www.ecumaster.com/products/gps-to-can-v2/)).
6. **Phone-app fragility** — TrackAddict crashes, OBD-adapter incompatibility, phone GPS not good enough without a $150 external puck ([appgrooves](https://appgrooves.com/app/trackaddict-by-racerender-llc/negative)).
7. **Silent spec downgrades** — VBOX Sport quietly cut from 20 Hz to 10 Hz ([Harry's forum](http://forum.gps-laptimer.de/viewtopic.php?t=5452)).
8. **Coaching-vs-data skepticism** — Garmin Catalyst reviewers couldn't verify its advice made verifiable sector gains; closed ecosystem, $1,200 ([Winding Road](https://windingroad.com/articles/reviews/gear-review-the-garmin-catalyst-driving-performance-optimizer-the-best-lap-timer/), [Lemons forum](https://forums.24hoursoflemons.com/viewtopic.php?id=41633)).

## 7. Implications for RDM

- **The market gap is exactly where RDM sits**: RaceBox proves 25 Hz hardware costs ~$130–290 but has no dash; AiM/ECUMaster charge $1.4–2.2k for the dash before adding a $300–480 GPS module. A **~A$150–250 25 Hz CAN GPS+IMU module** (u-blox M9/M10 + cheap IMU, broadcasting an open CAN protocol into the existing RDM channel system) undercuts the ECUMaster GPS-to-CAN V2 while the RDM-7 at $599 undercuts every logging dash by 3–5×. Total lap-timing dash < A$900 vs ~A$3k (ADU5+GPS) — that's the headline.
- **Match the table-stakes feature set on the dash**: auto track detection, GPS start/finish (no beacon), best/last/predictive delta, sectors, a dedicated "race page" widget with delta LEDs/bar. ECUMaster's "Qualification Mode" auto-best-reference is the pattern to copy; Plex's built-in 25 Hz+IMU is the spec to quote against.
- **Config UX is the differentiator**: competitors need Windows apps and CAN dongles. RDM already has a web editor + desktop studio — set start/finish by tapping a point on a recorded trace or map, zero extra hardware. Nobody else can configure lap timing from a phone browser.
- **Track DB**: seed with the top ~100 AU/NZ + popular US/EU circuits (self-curated coordinates), then take the RaceChrono route — community submissions synced via the marketplace. Avoid deriving from AiM (proprietary) or wholesale OSM import (ODbL share-alike).
- **Analysis story**: don't build i2. Build the VBOX Circuit Tools of this tier — cross-platform, auto-synced laps, delta-T plot, track map, two-lap overlay, CSV export — inside the existing desktop studio. Windows-only/clunky/licensed software is the most repeated complaint in this market; "free, modern, cross-platform analysis included" is a marketing line none of AiM/MoTeC can say.
- **Credibility details**: publish real numbers (25 Hz, constellations, ±0.02 s lap precision), never downgrade specs silently, and make firmware updates un-brickable — AiM's WiFi-update horror stories and VBOX's silent 10 Hz cut are reputational openings for a brand positioning itself as a peer to Haltech/AiM/MoTeC.

## 8. Feature deep-dive (2026-07-11 refresh) — what the software actually does

> **Status correction — 2026-07-28.** The ✅ marks in the two tables below were
> earned by RDM Studio's *Simulate/Analyse* workspace, which computed laps in
> the browser from a synthetic speed model — it never called `/api/lap/*` and
> never saw a real lap. That workspace has been removed (ADR-0011 widened: no
> Studio workspace fabricates data), so **every analysis row marked shipped is
> now "not built"**, not "built and working". The on-device rows are real: the
> lap engine, sectors, theoretical best and predictive delta all exist in
> `main/lap/lap_core.c`, are covered by 33 host tests, and now run on the puck
> itself. Treat this section as a feature *map*, not a shipping claim, until the
> analysis surface is rebuilt against recorded laps.


Three parallel research passes on AiM Solo 2/DL + Garmin Catalyst, RaceBox +
Harry's LapTimer/RaceChrono/TrackAddict, and the pro-analysis tier
(VBOX Circuit Tools, MoTeC i2, RaceRender). Feature-level findings, mapped to
what RDM Studio now ships vs. what's still open.

**On-device / real-time (dash + node) — the table stakes:**

| Feature | Who | RDM status |
|---|---|---|
| Predictive live delta vs best | everyone | ✅ shipped (Simulate) |
| Sector/split times | everyone (2–3 splits) | ✅ shipped |
| **Theoretical best** (quickest sector spliced from any lap) | AiM, VBOX, TrackAddict | ✅ **added v0.4.3** (OPT in timer bar + Analyse) |
| **Optimal lap** (best *driven* segments, not just sectors) | Garmin "True Optimal Lap" (patented) | open — richer than theoretical best |
| **Real-time coaching cues** (voice: "brake earlier", braking point in ft) | Garmin Catalyst — its whole moat | open — we have the data; text cues on the dash are feasible |
| Lap-consistency score | Garmin | open |
| Shift LEDs / race-page widget | AiM, Alfano, MoTeC | partial — dash widgets exist, no dedicated race page yet |

**Desktop analysis — where incumbents are weakest and RDM can win:**

| Feature | Benchmark | RDM status |
|---|---|---|
| **Delta-T trace** (cumulative time lost/gained, two laps) | Circuit Tools opens on it; i2 "time-variance" | ✅ **added v0.4.3** |
| **Racing line coloured by channel** (speed/gear) | all | ✅ speed colouring shipped |
| Two-lap line overlay (reference ghost) | Circuit Tools "Overhead Car View" | ✅ reference ghost shipped |
| **Channel graphs vs distance, overlaid on reference** | i2 benchmark (RPM/throttle/brake/steer) | ✅ speed/throttle-brake/RPM shipped |
| **g-g / friction circle** (Lat-G × Long-G) | i2 idiom | ✅ shipped |
| Scrub cursor linking graph ↔ map ↔ readout | i2, Circuit Tools | ✅ shipped |
| Sector deltas + coaching call-out ("−1.0s in S3") | Garmin auto "top-3 opportunities" | ✅ shipped (auto worst-sector call-out) |
| CSV / data export | all | ✅ shipped |
| **Video overlay + data gauges** (auto-synced to GPS timestamp) | RaceRender, Harry's, RaceChrono | open — heaviest lift; GPS-timestamp auto-sync is the differentiator (all incumbents do *manual* offset alignment) |
| Math channels | i2 Pro | open (channel-math exists on the dash already — reuse) |
| Cloud session sharing / social leaderboards | RaceBox has leaderboards; nobody has slick sharing | open (marketplace is the natural home) |

**The two structural moats (unchanged, now demonstrated):**

1. **GPS + CAN fusion, free and pre-labelled.** VBOX HD2 is the only incumbent
   fusing GPS with vehicle CAN (up to 80 ch), but it needs hand-mapped CAN
   IDs/DBC and is a bolt-on box. The RDM-7 dash is *already* the CAN gateway
   with decoded, named channels — so throttle/RPM/brake overlay against track
   position with zero config. The Analyse workbench shows exactly this.
2. **Free, modern, cross-platform analysis included.** The single most-repeated
   complaint across AiM Race Studio 3, MoTeC i2 and TrackAddict is the
   software: Windows-only, node-locked licences (i2 Pro reissue ≈ $100 on a
   hardware change), codec breakage, "sold my PC over it" learning curves.
   RDM Studio's Analyse mode is the "VBOX Circuit Tools of this tier" — a line
   none of AiM/MoTeC/Garmin can print.

**Pricing context (fresh):** RaceBox Micro/Mini/Mini-S **$129/$219/$289**;
apps one-time — TrackAddict Pro **$8.99**, RaceChrono Pro **$19.99**, Harry's
Petrolhead **$19.99**/Grand-Prix **$27.99**; AiM Solo 2 **$399–499**, Solo 2 DL
**$649**; Garmin Catalyst 2 **$1,199 / A$1,999**. Analysis software is either a
$9–28 app or bundled-but-Windows-locked — "free + cross-platform + GPS-CAN
fusion" splits that field.

**Priority order for the node era** (once hardware ships): (1) real recorded
sessions auto-downloaded off the node over WiFi replacing the demo data; (2)
Garmin-style real-time coaching cues on the dash (text, then audio); (3) video
overlay with GPS-timestamp auto-sync; (4) marketplace session sharing +
community leaderboards. Delta-T, theoretical best, speed-map, channel graphs,
g-g and coaching call-outs are **done in the desktop workbench today**.
