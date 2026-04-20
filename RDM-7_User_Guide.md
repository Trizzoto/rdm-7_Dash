# RDM-7 Dashboard — Setup & User Guide

**Firmware:** v1.1.1 · **Display:** 800 × 480 touchscreen · **Studio:** RDM Studio (web + desktop)

Welcome. This guide takes you from "just unboxed" to "live engine data in under 5 minutes," then covers everything the dashboard can do.

---

## Table of Contents

1. [Quickstart (5 minutes)](#quickstart-5-minutes)
2. [What's in the Box](#1-whats-in-the-box)
3. [Wiring & Install](#2-wiring--install)
4. [First Power-On — The 3-Step Wizard](#3-first-power-on--the-3-step-wizard)
5. [Connecting RDM Studio](#4-connecting-rdm-studio)
6. [Using RDM Studio (Web Editor)](#5-using-rdm-studio-web-editor)
7. [On-Device Touchscreen Controls](#6-on-device-touchscreen-controls)
8. [ECU Presets](#7-ecu-presets)
9. [Feature Tour](#8-feature-tour)
10. [Device Settings — Full Reference](#9-device-settings--full-reference)
11. [WiFi & Hotspot — Full Reference](#10-wifi--hotspot--full-reference)
12. [Firmware Updates (OTA)](#11-firmware-updates-ota)
13. [Troubleshooting](#12-troubleshooting)
14. [Advanced — Custom CAN Signals](#13-advanced--custom-can-signals)
15. [Specifications](#14-specifications)

---

## Quickstart (5 minutes)

| Step | What to do |
|------|-----------|
| **1** | Connect **4 wires**: Red → +12V switched, Black → ground, Green → CAN High, Yellow → CAN Low |
| **2** | Turn on the ignition. The dashboard boots and the **First-Run Wizard** launches automatically |
| **3** | Wizard **Step 1** auto-scans your CAN bus to find the right bitrate — just wait a few seconds |
| **4** | Wizard **Step 2** lets you pick your ECU (MaxxECU, Haltech, Ford, etc.) — tap the matching brand and version |
| **5** | Wizard **Step 3** offers three ways to customise the dash — see below |
| **6** | Tap **Finish**. Data should be live. Done. |

**How do you want to customise the dashboard?**

| Option | Best for | Trade-off |
|--------|----------|-----------|
| **Home WiFi** | You have WiFi in the garage | Your phone stays on the internet |
| **USB cable** | You want the Desktop Studio app | Needs a laptop + the cable |
| **Dashboard Hotspot** | No WiFi in the garage | Your phone leaves its normal WiFi while connected — no internet until you disconnect |

Any of the three gets you into **RDM Studio**, the drag-and-drop layout editor. Skip straight to [§5 Using RDM Studio](#5-using-rdm-studio-web-editor) if the wizard got you online.

---

## 1. What's in the Box

- **RDM-7 display unit** — 7" touchscreen, rear-mount connector
- **CAN + power harness** — 4-wire pigtail (red / black / green / yellow)
- **Mounting hardware** — brackets and screws for dash-panel install
- **USB-C cable** *(if ordered with Desktop Studio option)*

---

## 2. Wiring & Install

### 2.1 Wire Colours

| Wire | Function | Connects to |
|------|----------|-------------|
| 🔴 **Red** | +12V power | **Switched** ignition source (not constant 12V) |
| ⚫ **Black** | Ground | Clean chassis ground or battery negative |
| 🟢 **Green** | CAN High | ECU CAN-H (sometimes labelled `CANH`) |
| 🟡 **Yellow** | CAN Low | ECU CAN-L (sometimes labelled `CANL`) |

> **Tip:** CAN signal integrity is best with **twisted-pair** cable for the green/yellow run. Most ECUs publish their CAN wire colours and pinout in the wiring diagram — check "CAN H" / "CAN L" there.

### 2.2 CAN Termination

CAN networks need a **120 Ω terminator** at each end of the bus. Most aftermarket ECUs already have one built in at the ECU end, so you usually only need one at the dashboard end.

The RDM-7 has a built-in terminator you can enable via a jumper:

1. Remove the rear cover.
2. Find the **yellow terminator block** in the bottom-right corner.
3. Bridge the two pins to **enable** the resistor.
4. Leave them open to **disable** it (for daisy-chained installs where something else is terminating).

### 2.3 Mounting

Any flat panel works. Use the supplied brackets. The display reads best with a slight tilt back — anywhere between 0–20° off vertical is fine.

---

## 3. First Power-On — The 3-Step Wizard

The very first time the RDM-7 powers up, it launches a **First-Run Wizard** so you're not poking around menus trying to find things.

### Step 1 — CAN Auto-Scan

The dash tries every common CAN bitrate (125 / 250 / 500 / 1000 kbps) and locks onto whichever one sees traffic. If your ECU is powered and sending messages, this usually succeeds in 2–4 seconds.

If the scan times out (nothing detected on any bitrate), re-check your wiring — most commonly CAN-H and CAN-L are swapped, or the ECU isn't powered up yet. You can also **skip** and set the bitrate manually later in Device Settings.

### Step 2 — Choose Your ECU

A picker appears listing every supported ECU. Pick your brand, then your firmware version. The dashboard loads all the right CAN message IDs, bit positions, scale factors, and units for that ECU — no manual CAN setup needed.

See [§7 ECU Presets](#7-ecu-presets) for the full list. Don't see yours? Pick **Custom** and configure signals manually — see [§13 Advanced](#13-advanced--custom-can-signals).

### Step 3 — Connect

The final screen offers three ways to customise the layout. Pick whichever matches your situation:

#### Option A — Home WiFi  *(recommended if you have WiFi in the garage)*

1. Tap **WiFi Join**.
2. Pick your network from the scan list.
3. Type the password using the on-screen keyboard.
4. The dash shows its IP address once connected (e.g. `192.168.1.45`).
5. On your phone or laptop, open a browser and visit `http://<that IP>`.

Your phone stays on your home WiFi. You get internet as normal. This is the easiest path if signal is strong.

#### Option B — USB Cable  *(needs a laptop + Desktop Studio app)*

1. Plug the supplied USB-C cable from the RDM-7 to your laptop.
2. Open **RDM Desktop Studio** on the laptop. The app detects the device automatically.
3. No network setup required.

Good for at-home tuning. The Desktop Studio app is a free download — see [getstudio.rdm7.com](https://getstudio.rdm7.com).

#### Option C — Dashboard Hotspot  *(no WiFi needed)*

The dashboard broadcasts its own WiFi network you can connect to directly:

- **Network name:** `RDM7-XXXX` *(the last 4 digits match your device's MAC address — it's printed under the rear cover too)*
- **Password:** `rdm7dash`
- **Address:** `http://192.168.4.1`

1. On your phone's WiFi settings, connect to `RDM7-XXXX`.
2. Open a browser and go to `http://192.168.4.1`. *(On iPhone, the "Sign in to network" sheet usually pops up automatically — tap it.)*
3. RDM Studio loads.

> ⚠️ **Important trade-off:** While your phone is on the dashboard's hotspot, it **cannot reach the internet**. Messages, email, maps — all offline. Disconnect from `RDM7-XXXX` when you're done editing. For faster reconnects, there's a **"Scan QR"** button in Device Settings that shows the full URL as a QR code.

### Finishing the Wizard

Tap **Finish Setup**. The wizard never shows again (unless you factory-reset). You'll land on your live dashboard with real data flowing.

You can re-enter the wizard any time from **Device Settings → Re-run First-Run Wizard** if you want to redo CAN setup or change ECUs.

---

## 4. Connecting RDM Studio

If you skipped the wizard or need to reconnect later:

### Find the dashboard's address

1. **Long-press the RDM logo** on the dashboard home screen to open Device Settings.
2. Look at the **Network** section — it shows the current WiFi IP (if connected to home WiFi) and the hotspot IP (`192.168.4.1`).
3. Tap **Show QR** to display a scannable QR code — open your phone's camera, aim it at the screen, and tap the link that pops up.

### Open Studio

| Path | URL |
|------|-----|
| Home WiFi (preferred) | `http://<dash IP shown in Settings>` |
| Hotspot (phone on `RDM7-XXXX`) | `http://192.168.4.1` |
| Desktop Studio (USB) | Launch the app — it finds the dash automatically |

Any modern browser works (Chrome, Safari, Firefox, Edge). Desktop / laptop is easier for drag-and-drop; the mobile UI auto-activates on phones and tablets.

---

## 5. Using RDM Studio (Web Editor)

Studio is built into the firmware — there's nothing to install on the browser side.

### 5.1 Desktop UI — Quick Tour

```
┌─ Header bar ────────────────────────────────────────────────┐
│  [☰ menu]  Layout ▾   ECU ▾   [Screen mode]  [Sim]  [Save] │
├─ Left sidebar ─────┬─ Canvas (800×480) ─┬─ Inspector ──────┤
│                    │                     │                  │
│   Live signal      │   Your dashboard    │   Widget props   │
│   cards            │   preview           │   (when a widget │
│   (value, fresh/   │   (drag, resize,    │    is selected)  │
│    stale, CAN      │    nudge widgets    │                  │
│    decode info)    │    here)            │                  │
│                    │                     │                  │
├────────────────────┴─────────────────────┴──────────────────┤
│ Bottom: widget palette · zoom · coordinates · status        │
└─────────────────────────────────────────────────────────────┘
```

**The hamburger menu (☰)** in the header has Device Info, Signal Dashboard, Data Logger, Fuel Calibration, Marketplace, DBC import, and more.

### 5.2 Mobile UI

When you open Studio on a phone, the layout adapts:

- **Top:** hamburger menu + layout selector
- **Canvas:** pinch to zoom, drag to pan, tap a widget to select
- **Bottom nav:** Properties · Signals · Sim · Save
- **Floating zoom bar** auto-fades after 1.5 s so it doesn't block the view
- **Selection toolbar** (appears when a widget is selected): nudge arrows, 1 px / 10 px step toggle, copy / paste / lock / delete, width / height steppers, and a test-value slider for signal-driven widgets

Tablet layouts get a persistent side drawer; phones get a bottom sheet.

### 5.3 Adding Widgets

1. Open the widget palette (bottom bar on desktop, "+" button on mobile).
2. Tap the widget type you want.
3. A new widget appears on the canvas — drag it to where you want, drag the corners to resize.

### 5.4 Widget Types at a Glance

| Widget | What it shows | Typical use |
|--------|---------------|-------------|
| **Panel** | A number + label in a coloured box | Oil pressure, coolant temp, boost, voltage |
| **RPM Bar** | Horizontal fill-bar with redline zone + tick marks | Engine RPM, always across the top |
| **Bar** | Horizontal or vertical progress bar | Fuel level, throttle %, any ranged value |
| **Meter** | Circular needle gauge | Classic analogue-style reads (pressure, temp) |
| **Arc** | Partial-arc gauge | Slimmer alternative to Meter |
| **Indicator** | Turn-signal arrows | Left / right blinkers |
| **Warning** | Named warning lamp | Oil pressure low, check engine, etc. |
| **Shift Light** | RPM-triggered LED row | Upshift alert |
| **Text / Value** | Plain numeric readout, any font | Large standalone digits (speed, gear) |
| **Image** | Static image (logo, badge) | Branding, skin |
| **Button** | Touch-triggered action | Toggle layouts, reset trip |
| **Toggle** | Two-state button | Lights, fan override |
| **Shape Panel** | Background rectangle / container | Visual grouping, backdrops |

**Slot limits per layout:** 16 panels · 2 bars · 2 indicators (L/R) · 8 warnings · 1 RPM bar · unlimited of everything else within the overall 32-widget cap.

### 5.5 Assigning a Signal

Every data-showing widget needs a **signal** (a piece of CAN data). Easiest way:

1. Select the widget.
2. In the Inspector, click **Assign Signal**.
3. The picker opens — choose **ECU → version → channel** (e.g. MaxxECU → v1.3 → `OIL PRESSURE`).
4. Click **Apply**. The widget is now bound to that signal.

If your ECU isn't in the presets, see [§13 Advanced](#13-advanced--custom-can-signals) to enter CAN parameters manually.

### 5.6 Saving and Switching Layouts

- **Auto-save** is always on — your edits are persisted to the device every ~1 second. You should **never** lose work.
- Hit **Save** in the header to force-save immediately and apply to the running dashboard.
- The **Layout dropdown** switches between saved layouts. The hamburger menu → Layouts has rename / duplicate / delete / import / export JSON.
- Up to **16 layouts** can be stored on the device. The last-saved one loads automatically on power-up.

### 5.7 Live Preview

Click the **Live Preview** toggle in the header. The canvas backdrop becomes a live screenshot from the device, updated every few seconds, with your widget overlays on top. Great for verifying signals, colours, and thresholds without having to look over at the real dashboard.

### 5.8 Undo / Redo

- **Ctrl + Z** — undo (100-step history)
- **Ctrl + Y** — redo
- Toolbar arrows (↶ ↷) do the same

### 5.9 Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| `?` | Show all shortcuts |
| `Delete` | Remove selected widget |
| `Ctrl + D` | Duplicate widget |
| `Ctrl + C / V` | Copy / paste |
| `Ctrl + Z / Y` | Undo / redo |
| Arrow keys | Nudge 1 px |
| `Shift + Arrow` | Nudge 10 px |
| `Space + Drag` | Pan canvas |
| `Ctrl + Scroll` | Zoom |

---

## 6. On-Device Touchscreen Controls

You don't *have* to use Studio — most things can be done with just the touchscreen.

### 6.1 Configure a Widget

**Long-press** (hold ~0.5 s) any widget to open its config popup. You get 3 tabs:

- **CAN Signal** — ECU preset picker, or manual CAN ID / bit-start / bit-length / scale / offset
- **Display** — label, font, decimals, unit text, colours, widget-specific options
- **Alerts** — warning thresholds + alert colours

Changes save automatically when you close the popup.

### 6.2 Device Settings

**Long-press the RDM logo** on the home screen to open Device Settings. See [§9](#9-device-settings--full-reference) for the full reference.

### 6.3 Quick Actions

- **Two-finger tap** — toggles night mode manually
- **Swipe from left edge** — opens the peaks list (if enabled)
- **Long-press logo** — Device Settings

---

## 7. ECU Presets

Built-in presets ship with full signal maps so you don't have to enter CAN IDs by hand.

### 7.1 Supported ECUs

| ECU | Versions | Signal count |
|-----|----------|--------------|
| **MaxxECU** | v1.2, v1.3 | 46 / 100+ |
| **Haltech** | Nexus, Elite | full stream |
| **Ford** | BA, BF, FG | core powertrain |
| **Link** | G4+, G4X | *(coming)* |
| **GM GEN IV** | LS2 / LS3 / LT1 | core powertrain |

The picker shows brand, then version — pick whichever matches your ECU firmware. If in doubt, pick the closest — most aftermarket ECUs share CAN message structure across minor versions.

### 7.2 When Your ECU Isn't Listed

- **Custom** preset lets you enter CAN IDs manually — see [§13](#13-advanced--custom-can-signals)
- **DBC import** (Studio → hamburger → DBC Import) can translate a CAN database (.dbc) file from your ECU vendor into RDM signal definitions
- **Marketplace** (Studio → hamburger → Marketplace) hosts community-contributed layouts + signal packs — someone else may have already done the work for your car

---

## 8. Feature Tour

### 8.1 Night Mode

The dashboard can switch to a night-friendly colour scheme — dimmer, warmer, usually with different widget colours so nothing is glaring at 2am.

**How to trigger it:**
- **Manual:** two-finger tap the screen, or toggle in Device Settings → Display
- **Automatic via CAN:** bind night mode to a headlight-on signal in the layout config. Lights on → night mode on. Simple.

**Per-widget overrides:** Any widget can have a night-mode colour override. In the widget's Inspector panel, expand the **Night Mode** section and set whichever colours you want different. Leave them blank to use day colours unchanged.

*(Time-of-day triggering is on the roadmap — needs an RTC we haven't fitted yet.)*

### 8.2 Data Logger

Record every CAN signal to a CSV file on SD card. Great for diagnosing intermittent issues, analysing a session, or replaying data into Studio for layout testing later.

**How to use it:**
1. Insert an SD card (FAT32, any size up to 32 GB).
2. Device Settings → **Data Logging** → **Start**.
3. A red dot appears in the status area while logging.
4. Tap **Stop** when done. File is saved to `/sdcard/logs/` with a timestamped name.

**Rate selection:** 1, 2, 5, 10, 20, 50, 100, 200 Hz — or **Max** (as fast as possible, ~70–200 Hz depending on load). The rate is persisted — survives reboots.

**Auto-start on boot:** optional toggle in Data Logging settings — starts a new log every time the car powers up.

### 8.3 Signal Replay *(offline testing)*

Play a recorded CSV back through the signal system as if it were live CAN data. Perfect for verifying layout changes without being in the car.

- Device Settings → **Data Logging** → **Replay a file**
- Pick a log from SD
- Pick playback speed (0.1× – 100×) and loop on/off
- The dashboard behaves exactly as if you were driving — widgets update, warnings trigger, everything

Stop replay any time — real CAN immediately takes over again.

### 8.4 Peak Hold

Every signal silently tracks its all-time peak and minimum. You don't have to enable anything.

- **Peak Hold screen** (Device Settings → **View Peaks**) — scrollable table of every signal with Current / Min / Max columns, refreshes twice a second
- **"Reset All"** button — wipes all peak/min values
- **Per-widget peak display** — each Panel widget has a `Show Peak` option (Off / Max / Min / Both). Renders a small secondary label under the main value so you can watch, say, peak boost while driving

### 8.5 System Diagnostics

Device Settings → **System Diagnostics** opens a 5-card dashboard refreshing every second:

- **CAN Bus** — TX / RX frame counts, error counters, bus state, current bitrate
- **Wi-Fi** — SSID, signal strength, IP addresses (STA + AP)
- **SD Card** — mount status, free space, filesystem type
- **Signals** — total registered, stale count, highest-rate signal
- **System** — uptime, free heap, free PSRAM, logger + replay status, ESP-IDF version

First place to look if anything's acting weird.

### 8.6 Fuel Calibration

Studio → hamburger → **Fuel Calibration** opens a wizard for tuning analog fuel senders (most common on older cars).

1. Run the tank empty, note the sender's raw ADC value.
2. Fill the tank, note the full value.
3. Enter both — the dashboard maps sender voltage linearly between them and shows fuel % on any bar widget bound to `FUEL_LEVEL`.

For senders that aren't linear (common on Ford tanks), the wizard also supports a multi-point curve.

### 8.7 Marketplace

Studio → hamburger → **Marketplace** opens a catalogue of community-shared layouts and signal packs. Download directly to the device over WiFi. You can also upload your own creations.

---

## 9. Device Settings — Full Reference

Long-press the RDM logo on the home screen. Settings is organised into sections:

### Display
- **Brightness** slider (5–100%) with live preview
- **CAN Dimmer** — optional auto-brightness from a CAN signal (e.g. headlight bus)
- **Night Mode** manual toggle + CAN-trigger signal binding

### CAN Bus
- **Bitrate** dropdown (125 / 250 / 500 / 1000 kbps) — must match your ECU
- **Re-run Auto-Scan** — redoes the Step-1 wizard scan
- **TX / RX stats** mini-display

### Network
- **WiFi Settings** — join / forget / prioritise up to 5 saved networks
- **Hotspot Settings** — SSID shown (read-only), change password, "Hotspot on Boot" toggle
- **Show QR** button — scannable QR of the current dashboard URL (see §10.3)
- **Check Updates** — OTA firmware check

### Data Logging
- **Start / Stop** log button
- **Rate** dropdown (1 / 2 / 5 / 10 / 20 / 50 / 100 / 200 Hz / Max)
- **Auto-start on boot** toggle
- **Replay a file** — picker for recorded logs

### Peak Hold
- **View Peaks** — open the peaks screen
- **Reset Peaks** — clear all peak / min values

### Tools
- **Simulator** toggle — green when on. Generates fake CAN frames so you can test layouts on the bench without the car running.
  - *Long-press* the Sim button to open Sim Settings (rate, signal selection, waveform)
- **System Diagnostics** — health dashboard
- **Signal Dashboard** — raw list of every live signal with decoded values
- **Re-run First-Run Wizard** — full reset of CAN + ECU + connection

### About
- Firmware version, serial number, MAC address
- Free heap / PSRAM

---

## 10. WiFi & Hotspot — Full Reference

### 10.1 Joining a WiFi Network

1. Device Settings → **WiFi Settings**
2. Wait for the scan to populate (2–3 s)
3. Tap a network
4. Enter password on the on-screen keyboard (or skip if it's an open network)
5. Tap **Connect**
6. Status flips to **Connected** and shows the dash's IP

**Saved networks** — up to **5**. Any of them that appear in range auto-connect on boot, priority by save order. Networks currently out of range still show in the list under **SAVED (out of range)** so you know what the dash remembers.

**Forgetting a network** — each saved row has a **Forget** button.

**Re-connecting** — tap a saved network to re-connect without typing the password again.

### 10.2 Hotspot Mode

The dashboard always runs a built-in hotspot (`RDM7-XXXX`, password `rdm7dash`) alongside WiFi station mode. You can always connect directly to it, even when the dash is also online on your home WiFi.

**Hotspot on Boot** toggle — in WiFi Settings. If on, the AP starts at boot time. If off, the AP only starts after you manually enable it (useful if your phone auto-joins `RDM7-XXXX` and you don't want that).

**Changing the hotspot password** — Device Settings → Hotspot → set a new password → reboot.

### 10.3 The QR Code

`rdm7.local` mDNS resolution is flaky on some consumer routers, so the QR code is the reliable "scan and open" fallback:

1. Device Settings → Network → **Show QR**
2. A 280 × 280 QR appears overlaid on screen
3. Open your phone's camera, point it at the dash, tap the URL banner that pops up
4. Browser opens directly to Studio

The URL inside the QR is picked intelligently: prefers the home-WiFi IP if connected, falls back to the hotspot IP (`192.168.4.1`) otherwise.

### 10.4 Captive Portal Behaviour

When your phone joins the `RDM7-XXXX` hotspot, iOS and Android will try to verify internet access. The dashboard responds with a captive-portal redirect that usually pops up a **"Sign in to network"** sheet — tapping it opens Studio directly.

If the sheet doesn't appear automatically (varies by phone), just open any browser and go to `http://192.168.4.1`.

---

## 11. Firmware Updates (OTA)

Updates happen over WiFi — no cables, no flashing software.

### 11.1 Manual Check

1. Make sure the dash is on your home WiFi with internet access
2. Device Settings → **Check Updates**
3. If an update is available you'll see:
   - The new version number
   - What changed (release notes)
4. Tap **Download & Install**
5. Progress bar shows download
6. Dashboard reboots into the new firmware once done

### 11.2 Automatic Check Banner

When the dash is idle on your home WiFi, it checks for updates every few hours in the background. A small banner appears on the main screen if one is available — tap it to jump straight to the update prompt.

### 11.3 If It Fails

- **Dual-OTA safety:** the dash keeps two firmware slots. If the new version fails to boot cleanly, it rolls back automatically to the last known good version. You won't brick the device.
- **Retry:** downloads that fail mid-way retry up to 3 times automatically
- **WiFi strength matters:** updates are ~2 MB — weak signal can cause repeated failures. Move to a closer AP or use a USB flash as a fallback

---

## 12. Troubleshooting

### Widgets all show "---"

No CAN data. Work through:
- **Wiring:** CAN-H to green, CAN-L to yellow (not swapped)
- **Bitrate:** check Device Settings → CAN Bus — it must match your ECU
- **ECU preset:** wrong version selected means CAN IDs don't line up
- **Engine state:** some ECUs only broadcast when the engine is running (not just ignition on)
- **Check Diagnostics:** Device Settings → System Diagnostics → CAN card. RX frame count should be increasing

### Can't connect to the hotspot (phone)

- Make sure you picked `RDM7-XXXX` (your specific one, not another dashboard's)
- Password is `rdm7dash` by default — **case-sensitive**
- **Forget the network** on your phone, then rejoin — iOS especially gets sticky if it tried once and failed
- Move your phone within ~5 m of the dash — ESP32 WiFi isn't the strongest

### Connected to hotspot but browser won't load `192.168.4.1`

- Disable **Private Relay** (iOS) or **VPN** — both can intercept local IP traffic
- Turn off mobile data temporarily — some phones try to route local IPs through cell
- Open a fresh tab — browser may have cached an old error

### Home WiFi drops every few minutes

- The dash's WiFi is 2.4 GHz only. 5-GHz-only networks won't work; a dual-band network with `11b/g/n` on 2.4 GHz will
- If your router splits SSIDs into 2.4-GHz and 5-GHz bands, connect the dash to the 2.4-GHz one explicitly
- Weak signal — move the dash closer, or install a WiFi repeater

### OTA update fails

- Diagnostics → check free heap and free PSRAM. If free heap is < 30 KB something's leaking — reboot and retry
- WiFi signal strength — update needs a steady connection for ~30 s
- Can't reach the update server? Check the dash has internet: Diagnostics → Wi-Fi → "Internet: OK"

### Screen is blank / frozen

- **Hard reboot** — cycle the ignition
- If it stays blank, the layout file might be corrupt. The dashboard falls back to a default layout automatically after a second failed load

### Layout edit didn't save

- Auto-save uses a 1 s debounce — hit **Save** in Studio to force an immediate flush
- SD-card full? Diagnostics → SD card should show free space > 1 MB
- Layout count hit 16? Delete an old one — that's the hard cap

### Value looks wrong (e.g. showing 16384 instead of 64)

Classic endianness mismatch. In the widget's CAN Signal tab, flip **Endianness** (Intel ⇄ Motorola) and the value usually snaps right.

### "Signal stale" flag on a widget

The dashboard hasn't received the bound signal in the last 2 seconds. Either the ECU stopped sending it, or you've got the wrong CAN ID.

---

## 13. Advanced — Custom CAN Signals

*For users whose ECU isn't in the presets and who know their ECU's CAN protocol.*

### 13.1 Anatomy of a Signal

A CAN message is just 8 bytes of data at a given message ID. A **signal** is a specific field packed somewhere inside those 8 bytes. To read it you need:

| Parameter | Example | What it is |
|-----------|---------|-----------|
| **CAN ID** | `0x218` (hex) or `536` (dec) | Which message to listen on |
| **Bit Start** | `32` | Which bit the value starts at (0–63) |
| **Bit Length** | `16` | How many bits wide |
| **Endianness** | `Intel` | Byte order (Intel = little, Motorola = big) |
| **Signed?** | `No` | Can it be negative? |
| **Scale** | `0.1` | Multiply raw value by this |
| **Offset** | `0` | Add this after scaling |

**Formula:** `displayed = raw × scale + offset`

### 13.2 Creating a Signal in Studio

1. Right sidebar → **Signals** tab
2. Click **+ New Signal**
3. Name it (`OIL_PRESSURE`), fill in the CAN params
4. Click **Create**
5. Now any widget can pick it via Assign Signal

### 13.3 Creating One On-Device

1. Long-press a widget
2. Go to the **CAN Signal** tab
3. Manually enter CAN ID, bit start, bit length, scale, offset, endianness
4. Close the popup — the signal is saved with the layout

### 13.4 DBC Import

Got a `.dbc` file from your ECU vendor? Studio → hamburger → **DBC Import** converts the whole file into RDM signals in one shot.

### 13.5 Finding CAN Protocol Info

- **ECU manual** — most aftermarket ECUs publish a CAN protocol sheet
- **Similar ECUs** — MaxxECU and Link share bits of their protocol; same with Ford and Mazda older models
- **CAN sniffer** — a PCAN / USB-CAN adapter + SavvyCAN lets you log raw traffic and identify message IDs

### 13.6 Endianness Quick Check

If the value looks completely wild (16384 instead of 64, negative when it shouldn't be), flip endianness. 9 times out of 10 that's the fix.

---

## 14. Specifications

| | |
|---|---|
| **Display** | 800 × 480, RGB LCD, 16-bit colour |
| **Refresh** | 70 Hz |
| **Touch** | Capacitive, GT911 controller |
| **Processor** | ESP32-S3 dual-core, 240 MHz |
| **Memory** | 8 MB PSRAM + 16 MB flash |
| **Storage** | LittleFS (~8.8 MB for layouts, images, fonts) + SD card |
| **CAN Bus** | ISO 11898, 125 / 250 / 500 / 1000 kbps |
| **CAN Pins** | TX = GPIO 20, RX = GPIO 19 |
| **WiFi** | 2.4 GHz 802.11 b/g/n, station + AP concurrent |
| **Web Server** | HTTP port 80 |
| **Layouts** | Up to 16 stored |
| **Widgets** | 32 per layout |
| **Signals** | 128 per layout |
| **Signal Timeout** | 2 s (widget shows "---" after this) |
| **Power** | 12 V DC switched |
| **Firmware Updates** | OTA over WiFi, dual-slot rollback safety |
| **Supported ECUs** | 8 built-in presets + custom + DBC import |

---

*RDM-7 Dashboard — Setup & User Guide*
*Document for firmware v1.1.1 · © RDM*
