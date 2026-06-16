# Cluster recreations

One toolset for the photoreal RDM-7 dashboard cluster recreations (Toyota
Altezza, KTM "demon", Mercedes-AMG V12 BiTurbo, ...). Replaces the old
per-cluster `tools/{altezza,ktm,mercedes}/gen.py` scripts.

## Technique

Each design **bakes all the static art into one opaque 800×480 background image**
and overlays only the live widgets (needles, digits) on top. Geometry is defined
once and shared between the PIL face renderer and the meter/bar widget config, so
needles stay glued to the baked ticks. Layout colors are raw **RGB565 decimal**
(`cluster_lib.rgb565`); device fonts use already-seeded families ("Fugaz One",
"Manrope Bold", "Montserrat") so no font upload is needed.

## Layout

```
cluster_lib.py        shared toolkit: rgb565, .rdmimg writer, geometry, draw helpers, glow
gen.py                build CLI  -> tools/clusters/dist/<name>/
deploy.py             deploy CLI -> upload + activate + inject + screenshot on a live dash
designs/
  altezza.py  ktm.py  mercedes.py     one module per cluster (uniform interface)
dist/<name>/          build output: <IMAGE_NAME>.rdmimg/.png, <name>_layout.json, meta.json
```

## Build

```sh
python tools/clusters/gen.py mercedes          # build one
python tools/clusters/gen.py mercedes ktm       # several
python tools/clusters/gen.py all                # every design
python tools/clusters/gen.py altezza --preview  # + WASM browser-preview bundle (rdm7-wasm-editor)
python tools/clusters/gen.py mercedes --mock     # + standalone composited preview PNG (if supported)
```

## Deploy + verify (bench dash, default 192.168.4.61)

```sh
python tools/clusters/deploy.py mercedes                       # default state
python tools/clusters/deploy.py mercedes --state idle          # a named STATES key
python tools/clusters/deploy.py ktm --state '{"RPM":7400,"GEAR":3}'   # ad-hoc JSON
python tools/clusters/deploy.py mercedes --skip-upload         # image already on device
```

Pipeline: `POST /api/image/upload?name=<IMAGE_NAME>` → `POST /api/layout/save`
→ `POST /api/signal/inject` → `GET /api/screenshot`, with ~1.4 s settle after
save/inject (async hot-reload + needle smoothing). Bodies go through `urllib`, so
JSON isn't mangled by shell quoting.

**OOM note:** the ~1.15 MB image buffers in PSRAM on upload; re-uploading the bg
that's *already loaded* can OOM (drops the connection). Deploy a different layout
first, or pass `--skip-upload` when the image is unchanged on the device.

## Adding a cluster

1. Add `designs/<name>.py` exposing `NAME`, `IMAGE_NAME`, `build_background()`,
   `build_layout()`, `STATES`, `DEFAULT_STATE` (optionally `render_mock`,
   `emit_preview`). See `designs/mercedes.py` for the simplest example.
2. Register it in `designs/__init__.py` `REGISTRY`.
3. `python tools/clusters/gen.py <name>` then read `dist/<name>/<IMAGE_NAME>.png`
   to eyeball, then `deploy.py <name>` to verify live.
