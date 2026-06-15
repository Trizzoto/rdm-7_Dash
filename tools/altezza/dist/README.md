# Toyota Altezza cluster — RDM-7 Dash layout

A recreation of the Toyota Altezza RS200 (Lexus IS200) **optitron** gauge cluster:
big centre tachometer (0–9 ×1000 r/min, graduated redline at 8), left speedometer
(0–180 km/h), right fuel gauge, three inner sub-dials (oil L–H, volt "18V", coolant
C–H), the "ALTEZZA" wordmark, and the orange LCD (ODO + live digital speed) — amber
electroluminescent numerals on near-black with chrome bezels.

Verified rendering through the actual firmware widget engine (WASM) **and on real
ESP32-S3 hardware** (see `device_idle.jpg` / `device_driving.jpg`).

## How it's built

All static artwork (faces, ticks, amber-glow numerals, redline, icons, ALTEZZA,
LCD frame + labels) is **baked into a single 800×480 background image**
(`cluster_bg.rdmimg`). The layout overlays only the live elements:

- 6 `meter` widgets = transparent (no ticks/face), needle-only, positioned over the
  baked dials — tach, speedo, fuel, oil, volt, temp.
- 1 `text` widget = the live digital speed on the LCD (font `Fugaz One:60`, which is
  already bundled on every device).
- The image widget is index 0 so it renders behind everything.

Source generator: `tools/altezza/gen.py` (+ `altezza_lib.py`). Re-run with
`python tools/altezza/gen.py` — it regenerates the image, `layout.json`,
`manifest.json`, and the standalone `preview_*.png` mockups.

## Files

| file | purpose |
|---|---|
| `altezza_layout.json` | the layout (firmware schema v14) |
| `cluster_bg.rdmimg`   | the baked 800×480 cluster background (RGB565+alpha) |
| `device_idle.jpg`, `device_driving.jpg` | real-hardware screenshots |
| `preview_idle.png`    | offline mockup (PIL-rendered needles) |

## Install on a device (verified on 192.168.4.61)

```powershell
$ip = "192.168.4.61"   # your dash IP

# 1) upload the background texture
Invoke-RestMethod "http://$ip/api/image/upload?name=cluster_bg" -Method Post `
  -InFile .\cluster_bg.rdmimg -ContentType 'application/octet-stream'

# 2) save + activate the layout
$body = [System.IO.File]::ReadAllBytes(".\altezza_layout.json")
Invoke-RestMethod "http://$ip/api/layout/save" -Method Post `
  -Body $body -ContentType 'application/json'
```

No font upload is needed — the layout uses `Fugaz One`, which the firmware seeds on
every device. The needles bind to CAN signals `RPM`, `VEHICLE_SPEED`, `FUEL_LEVEL`,
`BATT_VOLT`, `OIL_PRESS`, `COOLANT_TEMP` (rebind to your ECU's channels as needed).

### See it without live CAN (test values)

```powershell
$b = '{"values":[{"signal":"RPM","value":850},{"signal":"VEHICLE_SPEED","value":0},{"signal":"FUEL_LEVEL","value":86},{"signal":"BATT_VOLT","value":14.2},{"signal":"OIL_PRESS","value":4.5},{"signal":"COOLANT_TEMP","value":88}]}'
Invoke-RestMethod "http://$ip/api/signal/inject" -Method Post -Body $b -ContentType 'application/json'
```

(`POST /api/signal/clear` with `{"all":true}` releases the test locks.)

### Restore the previous layout

The device's prior active layout was backed up to
`tools/altezza/device_backup_layout.json` before deployment — POST it back to
`/api/layout/save` to restore.
