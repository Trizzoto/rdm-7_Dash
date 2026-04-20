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

This firmware is built with **ESP-IDF v5.3.1** and **LVGL v8**. See [`CLAUDE.md`](CLAUDE.md) for the architectural overview.

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
