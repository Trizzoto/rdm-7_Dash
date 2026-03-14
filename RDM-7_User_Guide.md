# RDM-7 Dashboard — User Guide

**Firmware Version:** v1.x
**Display:** 800 x 480 Touchscreen
**Web Editor:** RDM Studio

---

## Table of Contents

1. [What is the RDM-7?](#1-what-is-the-rdm-7)
2. [Getting Started](#2-getting-started)
3. [Connecting to RDM Studio (Web Editor)](#3-connecting-to-rdm-studio-web-editor)
4. [RDM Studio Overview](#4-rdm-studio-overview)
5. [Working with Layouts](#5-working-with-layouts)
6. [Adding Widgets](#6-adding-widgets)
7. [Widget Types](#7-widget-types)
8. [Assigning Signals from Presets](#8-assigning-signals-from-presets)
9. [Editing Widget Properties](#9-editing-widget-properties)
10. [Moving, Resizing and Aligning Widgets](#10-moving-resizing-and-aligning-widgets)
11. [Live Preview](#11-live-preview)
12. [On-Device Touchscreen Configuration](#12-on-device-touchscreen-configuration)
13. [Device Settings](#13-device-settings)
14. [Firmware Updates (OTA)](#14-firmware-updates-ota)
15. [Keyboard Shortcuts](#15-keyboard-shortcuts)
16. [Troubleshooting](#16-troubleshooting)
17. [Advanced: Custom CAN Bus Signals](#17-advanced-custom-can-bus-signals)
18. [Technical Specifications](#18-technical-specifications)

---

## 1. What is the RDM-7?

The RDM-7 is a programmable automotive dashboard display. It connects to your vehicle's CAN bus and shows live engine data — things like RPM, speed, oil pressure, boost, temperatures, and more — on a full-colour 800 x 480 touchscreen.

You design your dashboard layout using a drag-and-drop web editor called **RDM Studio**. Pick what data you want to see, choose where it goes on screen, and save. The device updates instantly.

**Key features:**

- 800 x 480 pixel colour touchscreen
- Reads live CAN bus data from your ECU
- Drag-and-drop layout editor via WiFi (RDM Studio)
- Built-in ECU presets (MaxxECU and more) — no CAN knowledge needed
- Multiple saved layouts — switch between them anytime
- Warning lights, RPM bars, gauges, and more
- Over-the-air firmware updates

---

## 2. Getting Started

### What's in the box

- RDM-7 display unit
- CAN bus wiring harness (CAN High, CAN Low, Power, Ground)
- Mounting hardware

### Wiring

1. **Power:** Connect the red wire to 12V switched power and the black wire to ground.
2. **CAN Bus:** Connect the CAN High and CAN Low wires to your vehicle's CAN bus. These are usually available at your ECU or an OBD-II port.
3. **Termination:** If the RDM-7 is at the end of the CAN bus chain, you may need a 120-ohm termination resistor between CAN High and CAN Low. There is one built into the RDM-7. Remove the back cover of the RDM-7 to access it. The yellow termination connector in the bottom right, change the joined pins of the connector to enable the termination resistor.

### First power on

When you turn on the RDM-7 for the first time:

1. A splash screen appears briefly.
2. The device loads a default dashboard layout with common widgets.
3. Widgets will show "---" until they receive CAN data.

That's it — you're up and running. The next step is connecting via WiFi to customise your layout.

---

## 3. Connecting to RDM Studio (Web Editor)

RDM Studio is the web-based layout editor built into the RDM-7. You access it from any phone, tablet, or computer on the same WiFi network.

### Step 1: Set up WiFi on the device

1. On the RDM-7 touchscreen, open the **Settings** menu.
2. Go to the **WiFi** section.
3. Enter your WiFi network name (SSID) and password.
4. Tap **Connect**.
5. The device will show its IP address once connected (e.g., `192.168.1.45`).

### Step 2: Open RDM Studio

1. On your phone or computer, make sure you're connected to the **same WiFi network**.
2. Open a web browser (Chrome, Firefox, Safari, Edge — any will work).
3. Type the IP address shown on the device into the address bar, for example: `http://192.168.1.45`
4. RDM Studio will load in your browser.

> **Tip:** RDM Studio works best on a laptop or desktop with a larger screen. It works on phones and tablets too, but the drag-and-drop experience is easier with a mouse.

---

## 4. RDM Studio Overview

When RDM Studio loads, you'll see a layout editor that looks similar to a design tool. Here's what each part does:

### Header Bar (top)

The black bar across the top contains:

- **RDM-7 logo** — top left
- **Bezel** button — toggles a realistic device frame around the display preview
- **Widgets** button — shows or hides widget outlines on the display
- **Layout dropdown** — select which layout to edit
- **Layout menu** — save, rename, duplicate, delete, import, and export layouts
- **Live Preview toggle** — shows a live screenshot from the device
- **Save** and **Apply** buttons — save your changes to the device

### Widget Palette (left sidebar)

A list of all available widget types you can add to your layout. Click one to add it to the canvas.

### Display Canvas (centre)

The large area in the middle shows your 800 x 480 display. This is where you drag, drop, move, and resize widgets. If you have Live Preview turned on, you'll see a real-time screenshot from the device behind your widget boxes.

### Inspector Panel (right sidebar)

When you select a widget, this panel shows its properties — position, size, and type-specific settings like labels, fonts, and signal assignments.

### Signals Panel (right sidebar, second tab)

Shows all the CAN signals in your layout. You can search, edit, and create signals here.

### Status Bar (bottom)

Shows the connection status, widget/signal count, cursor coordinates, and zoom controls. You can type a custom zoom percentage directly into the zoom field.

---

## 5. Working with Layouts

A **layout** is a saved dashboard design. It contains all your widgets (their positions, sizes, and settings) and all the CAN signals they use. You can have multiple layouts saved on the device and switch between them.

### Creating a new layout

1. Click the **layout menu** button (hamburger icon) in the header.
2. Select **New Layout**.
3. Give it a name.
4. You'll start with a blank canvas — add widgets from the left palette.

### Saving your layout

- Click the **Save** button in the header, or use the layout menu and select **Save AS NEW**.
- Your layout is saved to the device immediately.
- The device will reload and display your updated layout.
- Last layout saved will be loaded on boot up.
### Switching between layouts

- Use the **layout dropdown** in the header to pick a different layout.
- The selected layout becomes the active one on the device.

### Renaming a layout

1. Open the **layout menu**.
2. Select **Rename**.
3. Type a new name and confirm.

### Duplicating a layout

- Open the **layout menu** and select **Duplicate**. This creates a copy you can modify without affecting the original.

### Deleting a layout

- Open the **layout menu** and select **Delete**. You'll be asked to confirm.

### Importing and exporting

- **Export JSON:** Downloads the layout as a `.json` file to your computer. Useful for backups or sharing.
- **Import JSON:** Loads a `.json` layout file from your computer.

---

## 6. Adding Widgets

Widgets are the building blocks of your dashboard. Each widget displays a piece of information — a number, a bar, a gauge, or a warning light.

### How to add a widget

1. Look at the **Widget Palette** on the right sidebar.
2. Click on the widget type you want (e.g., "Panel", "RPM Bar", "Meter").
3. A new widget appears on the canvas at a default position.
4. Drag it to where you want it.
5. Use the handles on the edges and corners to resize it.

### How to delete a widget

- Select the widget, then press the **Delete** key on your keyboard.
- Or right-click the widget and select **Delete** from the context menu.

### How to duplicate a widget

- Right-click the widget and select **Duplicate**, or press **Ctrl+D**.

---

## 7. Widget Types

Here's what each widget type does and when to use it.

### Panel

A data display box that shows a single value with a label.

**Use it for:** Oil pressure, coolant temperature, boost, battery voltage, or any numeric reading.

**What you can set:**
- **Label** — the title shown above the value (e.g., "OIL", "COOLANT")
- **Signal** — which CAN signal to display
- **Decimals** — how many decimal places to show (0, 1, 2, etc.)
- **Unit text** — optional text below the value (e.g., "PSI", "C")
- **Warning thresholds** — high/low values that change the colour to alert you

---

### RPM Bar

A horizontal bar that fills up as RPM increases, with a redline zone.

**Use it for:** Engine RPM display, typically placed along the top or bottom of the screen.

**What you can set:**
- **Signal** — the RPM signal from your ECU
- **Gauge Max** — the maximum RPM shown (e.g., 8000, 10000)
- **Redline** — the RPM value where the bar turns red
- **Bar Colour** — choose from green, blue, yellow, orange, red, purple, or custom
- **Limiter Effect** — visual effects at rev limit (flashing, warning circles, or both)
- **Background Enhancement** — optional background colour at a certain RPM

**Limits:** One RPM bar per layout.

---

### Bar

A horizontal progress bar that fills between a minimum and maximum value.

**Use it for:** Fuel level, throttle position, or any value with a defined range.

**What you can set:**
- **Signal** — which CAN signal to display
- **Label** — text beside the bar (e.g., "FUEL")
- **Min / Max** — the range of values the bar represents
- **Low / High thresholds** — colour changes when the value is too low or too high
- **Colours** — separate colours for low, normal, and high zones
- **Fuel Sender Mode** — special mode for analogue fuel level senders (set empty and full voltages)

---

### Indicator

A turn signal indicator light (left or right arrow).

**Use it for:** Turn signal / blinker indicators.

**What you can set:**
- **Signal** — the CAN signal or wire input for the indicator
- **Slot** — left (0) or right (1)

**Limits:** Up to 2 indicators (one left, one right).

---

### Warning

A small warning light that turns on when a condition is active.

**Use it for:** Check engine, low oil pressure, high temperature, or any on/off alert.

**What you can set:**
- **Signal** — the CAN signal that triggers the warning
- **Label** — name of the warning (e.g., "OIL", "TEMP")
- **Active Colour** — the colour when the warning is on (red, yellow, etc.)
- **Invert** — swaps the on/off logic
- **Momentary** — for pulse-type signals vs. steady-state

**Limits:** Up to 8 warning lights per layout.

---

### Text / Value

A simple text display that shows a signal value with optional formatting.

**Use it for:** Any numeric value you want to display without the box styling of a panel. Good for large standalone readouts.

**What you can set:**
- **Signal** — which CAN signal to display
- **Static Text** — optional fixed text to show
- **Font** — choose size and style
- **Decimals** — how many decimal places

---

### Meter

An analogue dial gauge with a needle that sweeps across an arc.

**Use it for:** Oil pressure, coolant temp, boost, or any value that looks good as a round gauge.

**What you can set:**
- **Signal** — which CAN signal drives the needle
- **Min / Max** — the range of the gauge scale
- **Start / End Angle** — where the sweep begins and ends (in degrees)
- **Label Font** — font for the scale numbers

**Note:** The meter is always square — changing the width automatically changes the height to match (and vice versa). Maximum size is 800 x 800 pixels.

---

## 8. Assigning Signals from Presets

This is the easiest way to get your dashboard working. If you have a supported ECU (like MaxxECU), you can load pre-configured signal definitions with one click — no need to know CAN IDs or bit positions.

### What is a signal?

A **signal** is a piece of data inside a CAN bus message. For example, your ECU sends a CAN message that contains your engine RPM. A signal definition tells the RDM-7 exactly where to find that RPM value inside the message — which message ID, which bits, and how to convert the raw number into a meaningful value.

### Using ECU Presets

**From RDM Studio (web editor):**

1. Select a widget on the canvas (click on it).
2. In the right panel, look for the **Signal** field.
3. Click the **Assign Signal** button.
4. A popup appears with three columns:
   - **ECU** — pick your ECU brand (e.g., MaxxECU)
   - **Version** — pick the ECU software version (e.g., v1.3)
   - **Channel** — browse the list of available signals
5. Click the signal you want (e.g., "OIL PRESSURE").
6. All the CAN parameters are filled in automatically.
7. Click **Apply**.

**From the device touchscreen:**

1. Long-press on a widget to open the config menu.
2. Go to the **CAN Signal** tab.
3. Tap **Load Preset**.
4. Select your ECU, version, and channel from the picker.
5. The CAN ID, bit positions, scale, and offset are filled in for you.
6. Tap **Confirm**.

### Available presets

**MaxxECU v1.2** — 46 signals including:
- Throttle %, MAP, Lambda, Ignition Angle
- Vehicle Speed, Battery Voltage, Coolant Temp
- EGT 1-8, Boost Solenoid Duty
- User Analog Inputs 1-4

**MaxxECU v1.3** — 100+ signals including everything in v1.2 plus:
- Oil Pressure, Oil Temp, Fuel Pressure
- Wastegate Pressure, Boost Target
- User Channels 1-12, Acceleration X/Y/Z
- VVT Cam Position, Knock Detection
- Boolean flags: Shiftcut, Rev Limit, Anti-Lag, Launch Control, Traction

> **Tip:** If your ECU isn't listed, you can still use the RDM-7 by entering signal parameters manually. See the [Advanced: Custom CAN Bus Signals](#17-advanced-custom-can-bus-signals) section.

---

## 9. Editing Widget Properties

Once you've placed a widget on the canvas, you can fine-tune its settings.

### Using RDM Studio (web editor)

1. Click on a widget to select it (its border will highlight).
2. The **Inspector panel** on the right shows all its properties.
3. Edit values directly — changes appear on the canvas immediately.

**Common properties:**

| Property | What it does |
|----------|-------------|
| **X, Y** | Position on screen (0,0 is the centre) |
| **W, H** | Width and height in pixels |
| **ID** | Unique name (e.g., "panel_0") — usually left as-is |
| **Signal** | Which CAN signal this widget displays |
| **Label** | The text label shown on the widget |
| **Decimals** | Number of decimal places (0 = whole numbers) |

Each widget type has additional properties specific to it (colours, thresholds, fonts, etc.) — see the widget descriptions in Section 7.

### Using the device touchscreen

1. Long-press on any widget for about half a second.
2. A 3-tab configuration menu opens:
   - **CAN Signal** — set the CAN bus parameters or load a preset
   - **Display** — set labels, fonts, decimal places, colours
   - **Alerts** — set warning thresholds and alert colours
3. Make your changes and close the menu.
4. Changes are saved to the layout automatically.

---

## 10. Moving, Resizing and Aligning Widgets

### Moving widgets

- **Drag:** Click and hold on a widget, then drag it to a new position.
- **Nudge:** Select a widget and use the **arrow keys** to move it 1 pixel at a time. Hold **Shift** for 10 pixels at a time.
- **Inspector:** Type exact X and Y values in the right panel.

### Resizing widgets

- **Handles:** Select a widget to see 8 small squares on its edges and corners. Drag any handle to resize.
- **Inspector:** Type exact W and H values in the right panel.
- **Meter note:** Meters are always square. Changing width changes height automatically.

### Aligning widgets

The **Align Bar** sits just above the canvas. Select a widget, then click an align button:

| Button | What it does |
|--------|-------------|
| Align Left | Moves widget to the left edge of the screen |
| Align Left Quarter | Moves to the 25% mark |
| Align Centre X | Centres the widget horizontally |
| Align Right Quarter | Moves to the 75% mark |
| Align Right | Moves to the right edge |
| Align Top | Moves to the top edge |
| Align Top Quarter | Moves to the 25% mark vertically |
| Align Centre Y | Centres vertically |
| Align Bottom Quarter | Moves to the 75% mark vertically |
| Align Bottom | Moves to the bottom edge |

### Smart Guides

As you drag widgets, thin red lines appear when edges line up with other widgets. This helps you align things neatly without counting pixels.

### Multi-select

Hold **Ctrl** (or **Cmd** on Mac) and click multiple widgets to select them together. You can then drag them as a group or nudge them with arrow keys.

---

## 11. Live Preview

Live Preview shows you exactly what the device screen looks like in real time, directly inside RDM Studio.

### Turning it on

- Click the **Live Preview** toggle in the header bar.
- The canvas background changes from blank to a live screenshot of the device display.
- The screenshot updates automatically every few seconds.

### How it works

- RDM Studio fetches a screenshot from the device over WiFi.
- Your widget boxes are overlaid on top so you can still select and edit them.
- When you save a layout, the device reloads and the preview updates.

### When to use it

- **Testing signals:** See if your CAN data is coming through correctly.
- **Tweaking positions:** Fine-tune widget placement while seeing real data.
- **Verifying colours:** Check that warning thresholds are triggering correctly.

---

## 12. On-Device Touchscreen Configuration

You don't always need the web editor. Many settings can be changed directly on the RDM-7 touchscreen.

### Opening the config menu

Long-press (hold your finger) on any widget for about half a second. A 3-tab configuration popup will appear.

### Tab 1: CAN Signal

This is where you set which CAN data the widget reads.

- **CAN ID** — the message ID in hexadecimal (e.g., 0x218)
- **Endianness** — Motorola (big-endian) or Intel (little-endian)
- **Bit Start** — which bit the value starts at (0-63)
- **Bit Length** — how many bits the value uses (1-64)
- **Scale** — multiply the raw value by this number
- **Offset** — add this number after scaling
- **Load Preset** button — opens the ECU preset picker (see Section 8)

### Tab 2: Display

Customise how the widget looks.

- **Label** — the header text
- **Decimals** — number of decimal places
- **Font** — choose from available fonts and sizes
- **Custom Text** — unit label or description
- Widget-specific options (colours, bar ranges, etc.)

### Tab 3: Alerts

Set up visual warnings.

- **Warning thresholds** — high and low values
- **Colours** — what colour to use when a threshold is exceeded
- **Enable/Disable** — turn individual alerts on or off

### Saving changes

Changes made on the touchscreen are saved automatically to the current layout. The web editor will pick up the changes within a few seconds if it's open.

---

## 13. Device Settings

Access device settings from the settings menu on the touchscreen.

### Brightness

- **Slider:** Adjust screen brightness from 5% to 100%.
- **Preview:** Test the brightness before committing.
- **CAN Dimmer:** Optionally control brightness via a CAN signal (e.g., for automatic day/night dimming).

### WiFi

- **SSID:** Your WiFi network name.
- **Password:** Your WiFi password.
- **Connect/Disconnect:** Toggle the WiFi connection.
- **Status:** Shows connected network and the device's IP address.

### CAN Bus

- **Bitrate:** Choose 125, 250, 500, or 1000 kbps.
- **Default:** 500 kbps (most common for aftermarket ECUs).
- The CAN bus restarts when you change the bitrate.

> **Important:** The bitrate must match your ECU's CAN bus speed. If you're not sure, 500 kbps is the most common setting for aftermarket ECUs like MaxxECU, Haltech, and Link.

---

## 14. Firmware Updates (OTA)

The RDM-7 can update its firmware over WiFi — no cables or special software needed.

### How to update

1. Make sure the device is connected to WiFi (and the WiFi network has internet access).
2. Open **Device Settings** on the touchscreen.
3. Go to the **OTA / Updates** section.
4. Tap **Check for Updates**.
5. If a new version is available, you'll see:
   - The new version number
   - What changed (release notes)
6. Tap **Download and Install**.
7. A progress bar shows the download status.
8. Once complete, the device reboots into the new firmware.

### What if it fails?

- The device has two firmware slots. If an update fails or causes problems, it can boot back into the previous working version.
- If the download fails, try again — the device will retry automatically up to 3 times.
- Make sure your WiFi signal is strong and stable during updates.

---

## 15. Keyboard Shortcuts

These work when RDM Studio is open in your browser. Press **?** to see this list in the editor.

| Shortcut | Action |
|----------|--------|
| **?** | Show keyboard shortcuts |
| **Delete** | Delete selected widget |
| **Ctrl + D** | Duplicate selected widget |
| **Ctrl + Z** | Undo |
| **Ctrl + Y** | Redo |
| **Ctrl + C** | Copy widget |
| **Ctrl + V** | Paste widget |
| **Arrow keys** | Move widget by 1 pixel |
| **Shift + Arrow keys** | Move widget by 10 pixels |
| **Scroll wheel** | Pan the canvas up/down |
| **Ctrl + Scroll** | Zoom in/out |
| **Space + Drag** | Pan the canvas freely |
| **Middle mouse + Drag** | Pan the canvas freely |
| **Right-click** | Context menu (copy, paste, delete, duplicate) |

---

## 16. Troubleshooting

### Widgets show "---"

The widget isn't receiving CAN data. Check:

- Is the CAN bus wired correctly? (CAN High and CAN Low connected)
- Is the CAN bitrate set correctly? (Must match your ECU — usually 500 kbps)
- Is the correct signal assigned? (Check the CAN ID, bit start, and bit length)
- Is the ECU actually sending data? (Engine may need to be running or ignition on)

### Can't connect to RDM Studio

- Make sure your phone/computer is on the **same WiFi network** as the RDM-7.
- Check the IP address shown on the device settings screen.
- Try typing `http://` before the IP address (e.g., `http://192.168.1.45`).
- Try a different browser if the page won't load.

### Layout won't save

- Check that the layout name doesn't contain special characters.
- The device can store up to 16 layouts. Delete old ones if you've reached the limit.

### Widget position looks wrong

- The RDM-7 uses a centre-origin coordinate system. (0, 0) is the middle of the screen, not the top-left corner.
- X ranges from -400 (left) to +400 (right).
- Y ranges from -240 (top) to +240 (bottom).

### Screen is blank or white

- Wait a few seconds — the display initialises during boot.
- If it stays blank, the layout file may be corrupted. The device will fall back to a default layout automatically.

### Web editor shows "Disconnected"

- The device may have lost WiFi. Check the WiFi settings on the device.
- Refresh the browser page.
- Make sure you haven't changed WiFi networks.

### Data values are wrong

- Double-check the signal parameters: CAN ID, bit start, bit length, scale, offset, and endianness.
- If using presets, make sure you selected the correct ECU version (e.g., MaxxECU v1.2 vs v1.3 — they use different CAN IDs).
- Check if the value is signed or unsigned — getting this wrong can cause negative numbers or wildly large values.

---

## 17. Advanced: Custom CAN Bus Signals

> **Note:** This section is for users who want to read CAN signals that aren't covered by the built-in ECU presets. You'll need to know your ECU's CAN protocol documentation (often called a DBC file or CAN map).

### What you need to know

Every CAN message has:

- **CAN ID** — a unique number identifying the message (11-bit, 0x000 to 0x7FF)
- **Data** — up to 8 bytes (64 bits) of payload

A **signal** is a specific value packed somewhere inside those 8 bytes. To read it, you need to know:

| Parameter | What it means | Example |
|-----------|-------------|---------|
| **CAN ID** | Which message contains this value | 0x218 (decimal 536) |
| **Bit Start** | Which bit the value starts at | 32 |
| **Bit Length** | How many bits wide the value is | 16 |
| **Endianness** | Byte order: Intel (little-endian) or Motorola (big-endian) | Intel |
| **Signed** | Whether the value can be negative | No |
| **Scale** | Multiply the raw value by this | 0.1 |
| **Offset** | Add this after multiplying | 0 |

**The formula:** `displayed_value = (raw_extracted_value * scale) + offset`

For example, if your ECU sends oil pressure as a 16-bit unsigned value starting at bit 32 in CAN message 0x218, with a scale of 0.1 and offset of 0:
- Raw value of 1250 becomes: 1250 x 0.1 + 0 = **125.0** (displayed as 125.0 PSI)

### Creating a custom signal in RDM Studio

1. In the right panel, switch to the **Signals** tab.
2. Click **+ New Signal**.
3. Enter the signal name (e.g., "OIL_PRESSURE").
4. Fill in the CAN parameters from your ECU documentation.
5. Click **Create**.
6. Now assign this signal to a widget using the signal picker.

### Creating a custom signal on the device

1. Long-press on a widget.
2. Go to the **CAN Signal** tab.
3. Manually enter the CAN ID, bit start, bit length, scale, offset, and endianness.
4. Close the config menu — the signal is saved with the layout.

### Endianness explained

- **Intel (little-endian):** The least significant byte comes first. Most aftermarket ECUs (MaxxECU, Haltech, Link) use this.
- **Motorola (big-endian):** The most significant byte comes first. Some OEM ECUs and older systems use this.

If your values look completely wrong (e.g., showing 16384 instead of 64), you probably have the endianness set incorrectly. Try switching it.

### Tips for finding CAN signals

- **Check your ECU manual** — most aftermarket ECUs publish their CAN protocol.
- **Use a CAN bus sniffer** — tools like a PCAN adapter or CANalyzer can log raw CAN traffic so you can identify message IDs and data patterns.
- **Start with presets** — even if your exact ECU isn't listed, a similar one might use the same CAN protocol.

---

## 18. Technical Specifications

| Specification | Value |
|--------------|-------|
| **Display** | 800 x 480 pixels, RGB LCD, 16-bit colour |
| **Refresh Rate** | 70 Hz |
| **Touch** | Capacitive (GT911 controller) |
| **Processor** | ESP32-S3 dual-core, 240 MHz |
| **Memory** | 8 MB PSRAM, 8 MB Flash |
| **CAN Bus** | Standard 11-bit (ISO 11898) |
| **CAN Bitrates** | 125 / 250 / 500 / 1000 kbps |
| **CAN Pins** | TX: GPIO 20, RX: GPIO 19 |
| **WiFi** | 2.4 GHz 802.11 b/g/n |
| **Web Server** | HTTP port 80 |
| **Max Layouts** | 16 |
| **Max Widgets** | 32 per layout |
| **Max Signals** | 128 per layout |
| **Signal Timeout** | 2 seconds (shows "---" if no data received) |
| **Power** | 12V DC |
| **Firmware Updates** | Over-the-air (OTA) via WiFi |

---

*RDM-7 Dashboard — User Guide*
*Document version: 1.0*
