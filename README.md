# RDM-7 Dash

Firmware for the **RDM-7 Digital Dashboard** — an ESP32-S3-based 7" automotive display that reads live CAN bus data from your ECU and presents it on a customisable 800 × 480 touchscreen.

- **800 × 480 RGB LCD** with capacitive touch
- **Drag-and-drop layout editor** (RDM Studio) over WiFi or USB
- **8 built-in ECU presets** (MaxxECU, Haltech, Ford, etc.) — no CAN knowledge required
- **Over-the-air firmware updates**
- **SD-card data logging and replay** for offline layout testing
- **Night mode, peak hold, diagnostics** built in

## For Users

📖 **[Setup & User Guide](RDM-7_User_Guide.md)** — start here if you bought a dashboard.

## For Developers

This firmware is built with **ESP-IDF v5.3.1** and **LVGL v8**.

**Start here:**
- [`docs/handover/README.md`](docs/handover/README.md) — developer handover docs. 10 numbered chapters covering architecture, build, widgets, signals, storage, UI, web server, and conventions. Read in order if you're new.
- [`CLAUDE.md`](CLAUDE.md) — terse architectural reference. Best for "I know the codebase, I just need to remember a detail."
- [`docs/adr/README.md`](docs/adr/README.md) — Architecture Decision Records. The "we already tried that" answers live here.
- [`tests/README.md`](tests/README.md) — native-host test suite (122 tests, sub-second feedback loop).
- [`CHANGELOG.md`](CHANGELOG.md) — what landed when, with commit SHAs.
- [`SECURITY.md`](SECURITY.md) — security posture + pre-release hardening checklist.

**Build + flash:**

```bash
# Configure IDF environment (one-time)
C:\Espressif\frameworks\esp-idf-v5.3.1\export.bat

# Build + flash
idf.py build
idf.py -p COMxx flash monitor
```

## The RDM Project

RDM-7 Dash is one of four repos that make up the RDM ecosystem:

| Repo | What it is |
|------|-----------|
| **RDM-7 Dash** *(this repo)* | Dashboard firmware |
| **RDM Desktop Studio** | Cross-platform Tauri app for layout editing over USB |
| **RDM Web Studio** | Browser-based layout editor (identical to the firmware-embedded one) |
| **RDM Marketplace** | Community-shared layouts, signal packs, and DBC files |

They share the same layout JSON format, API contracts, and asset pipelines.

## License

Code in this repository is in the Public Domain (CC0).
