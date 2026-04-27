# ADR 0003 — Desktop `index.html` Sync Plan

**Status**: Implemented (2026-04-27)
**Context**: `../rdm7-desktop/src/index.html` is ~518 lines smaller than `main/web/index.html` and is missing recent firmware UI work (CONTROL/LIVE mode, mobile widget toolbar, auto-save, layout-too-large pre-validation, ECU selector live-API, alert-test-without-signal, panel9 limiter tint, calculated gear modal, …). Conversely, the desktop file has Tauri-specific code that does not belong in firmware: USB-serial connection, WASM offline preview, native file dialogs, ZIP backup/restore via Rust, auto-updater, device-manager modal.

This ADR documents the merge plan: replace the desktop file with the firmware copy, re-inject the Tauri delta, and (critically) add a `fetch` interceptor in `transport.js` so the firmware's raw `fetch('/api/...')` calls route through the RDM SDK when running under Tauri.

## Why we haven't done it yet

The "load-bearing decision" is the **`fetch` interceptor in `transport.js`** (described below). Without it, ~30 firmware functions that use raw `fetch('/api/...')` will fail when the desktop app is in USB-serial or offline-local mode (WiFi mode keeps working because `fetch` to a real IP works). I cannot validate the interceptor without:

1. A Tauri build environment (`cargo tauri dev`).
2. The ability to exercise each transport mode (local / wifi / hotspot / usb).

Doing the merge as a one-shot text operation without that loop is the kind of move that produces "works on the dev's machine, fails on every USB user's machine" outcomes. So we're saving the plan and deferring execution to a session where the desktop dev is at the keyboard.

## The load-bearing dependency: `transport.js` `fetch` interceptor

Add this near the top of `rdm7-desktop/src/transport.js`, after `RDM` is defined:

```js
/* If running under Tauri AND not in 'local' mode, redirect /api/* fetches
   through the RDM SDK so USB-serial and HTTP transports both work.
   Local mode (no device) returns a clear error so the UI can show
   "you need to connect to a device first". */
if (typeof window.__TAURI_INTERNALS__ !== 'undefined') {
    const _origFetch = window.fetch.bind(window);
    window.fetch = async (input, init) => {
        const url = typeof input === 'string' ? input : input.url;
        if (RDM.mode !== 'local' && url.startsWith('/api/')) {
            return RDM.proxyApiCall(url, init);
        }
        return _origFetch(input, init);
    };
}
```

`RDM.proxyApiCall(url, init)` needs to be implemented in `transport.js`. It should:
- Translate the request to the active transport (HTTP for `wifi`/`hotspot`, JSON-RPC over serial for `usb`).
- Return a `Response`-shaped object the firmware code can consume (`.ok`, `.status`, `.json()`, etc.).
- Be the single conversion layer between firmware's "I'm talking to my own HTTP server" assumption and the desktop's multi-transport reality.

If `RDM.proxyApiCall` already partially exists, audit its coverage against the firmware endpoints listed in [docs/handover/07-web-server-api.md](../handover/07-web-server-api.md) before considering it complete.

## Execution plan

Do these in **separate commits** in the desktop repo, in this order. Test each transport mode (`local` / `wifi` / `hotspot` / `usb`) after each commit.

### Step 1 — Implement the `fetch` interceptor + `proxyApiCall`

Edit `transport.js` only. Don't touch `index.html` yet.

Verify by:
- Open the existing desktop app (still on the old `index.html`).
- Switch transport modes — confirm nothing regresses.
- Add a temporary `console.log` inside the interceptor to confirm `/api/*` routes are intercepted under Tauri.

### Step 2 — Replace `index.html` with firmware copy

```bash
cp ../RDM-7_Dash/main/web/index.html src/index.html
```

This deletes the Tauri-specific code from the file. The result will not function under Tauri — re-injection of the delta is steps 3–6.

Commit with message: `chore(desktop): rebase index.html on firmware-canonical (delta lost — restored next commits)`.

### Step 3 — Add Tauri-specific `<head>` / `<style>` / `<script src=>`

Add (in `<head>` or end of `<head>`):

```html
<script src="build/index.js"></script>
<script src="rdm_logo_data.js"></script>
<script src="transport.js"></script>
```

Add to the project `<style>` block:

**Download progress overlay** (just before the `/* Toast Notifications */` comment):

```css
.dl-progress-overlay {
    position: fixed; bottom: 80px; left: 50%; transform: translateX(-50%);
    z-index: 3500; background: hsl(220, 15%, 16%);
    border: 1px solid hsla(0, 0%, 100%, 0.12); border-radius: var(--radius-md);
    padding: 12px 20px; min-width: 300px; box-shadow: var(--shadow-md);
    display: none;
}
.dl-progress-overlay.visible { display: block; }
.dl-progress-label { font-size: 12px; color: hsl(0, 0%, 75%); margin-bottom: 8px;
    white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.dl-progress-track { height: 6px; background: hsl(220, 10%, 25%);
    border-radius: 3px; overflow: hidden; }
.dl-progress-fill { height: 100%; background: #2d8ceb; border-radius: 3px;
    transition: width 0.15s ease-out; width: 0%; }
```

**Device Manager modal** (just before `</style>`):

```css
.dm-section { background: var(--bg); border-radius: var(--radius-md);
    border: 1px solid var(--border); padding: 12px; }
.dm-section-title { font-size: 10px; color: var(--text-muted);
    text-transform: uppercase; letter-spacing: 0.08em; margin-bottom: 8px; }
.dm-row { display: flex; justify-content: space-between; align-items: center;
    font-size: 12px; padding: 3px 0; }
.dm-label { color: var(--text-muted); }
.dm-value { font-family: 'JetBrains Mono', monospace; }
.dm-bar { height: 6px; background: var(--border); border-radius: 3px;
    overflow: hidden; margin-top: 4px; }
.dm-bar-fill { height: 100%; background: var(--accent); border-radius: 3px;
    transition: width 0.3s; }
```

### Step 4 — Add Tauri-specific globals + connection management

Inside the main `<script>` block, just before `function _checkLayoutSize(payload) {`:

```js
/* ── Tauri / Desktop runtime state ──────────────────────────────── */
let _lastUsbPort = null;
let _connectionOk = false;
let _healthInterval = null;

/* WASM preview engine state */
let wasmModule = null;
let _wasmReady = false;
let _wasmPreviewTimer = null;
let _wasmCrashed = false;
const _wasmRegisteredImages = new Set();
const _wasmFailedImages = new Set();

/* Auto-updater state */
const _UPDATE_DESKTOP_REPO = 'Trizzoto/rdm7-desktop';
const _DESKTOP_VERSION = '0.1.0';
let _pendingUpdate = null;

/* Device Manager refresh timer */
let _dmRefreshTimer = null;
```

Then re-inject the connection management block. Source: pre-merge `rdm7-desktop/src/index.html` lines 2766–3061 (≈296 lines: `_onConnectionModeChange`, `_connectUsb`, `_testConnection`, `_startHealthCheck`, `_stopHealthCheck`, `_updateConnectionUI`, `_scanForDevices`, `_connectToDevice`, scan-button visibility check, file-open listener, `_restoreConnection`).

Get those lines from git:

```bash
cd ../rdm7-desktop && git show HEAD~1:src/index.html | sed -n '2766,3061p' > /tmp/tauri_connection.js
```

(`HEAD~1` because step 2's commit is the one that lost them. Adjust as needed.)

**One integration point**: the `file-opened` event handler calls `_processRdmBytes`. In the pre-merge desktop file this was a closure inside `importRdm` (line 10199, scoped local). After merge, `importRdm` is firmware-canonical and won't have it. Either:
- (a) Extract `_processRdmBytes` from the pre-merge `importRdm` and add it as a top-level function before the connection block.
- (b) At the top of the (firmware) `importRdm` body, add `window._processRdmBytes = _processRdmBytes;` so the listener finds it.

Recommend (a).

### Step 5 — Re-inject WASM preview + IndexedDB image cache

Same script block, immediately after the connection management block.

Source: pre-merge desktop lines 3063–3141 (IndexedDB image storage, 79 lines) and 3143–3276 (WASM preview engine + `_dlProgressShow`/`_dlProgressHide` + `_syncWasmImages` + `updateWasmPreview` + `scheduleWasmPreview` + `injectWasmSignal` + `buildLayoutJson`, 134 lines).

**Integration**:
- Hook `scheduleWasmPreview()` into firmware's `triggerPreview()` (line ~6411). Add `if (typeof scheduleWasmPreview === 'function') scheduleWasmPreview();` at the bottom of `triggerPreview`'s body.
- Hook the screen-dimension change. Inside firmware's `applyScreenDimensions` (line ~8886), add at the end:

```js
if (typeof wasmModule !== 'undefined' && wasmModule && !_wasmCrashed) {
    try { wasmModule.ccall('set_display_size', null, ['number', 'number'], [w, h]); }
    catch(e) { console.warn('[WASM] set_display_size failed:', e); }
}
```

### Step 6 — Re-inject self-update banner + Device Manager + ZIP backup

**Auto-updater** (Tauri self-update only — drop the desktop's firmware-update branch; firmware now has its own `_otaShowInstall` flow). Add near the end of the script block, just before the firmware's `/* Start */`:

```js
async function _checkDesktopUpdate() {
    if (!RDM.isTauri()) return;
    try {
        const info = await _tauriCall('check_desktop_update', {
            repo: _UPDATE_DESKTOP_REPO, currentVersion: _DESKTOP_VERSION
        });
        if (info && info.available) _showUpdateBanner('desktop', info);
    } catch (e) { console.log('Desktop update check skipped:', e); }
}

/* Copy verbatim from pre-merge desktop:
 *   _showUpdateBanner       lines 11053-11071
 *   _installUpdate          lines 11073-11117 (DESKTOP branch only — strip the firmware else-branch 11118-11145)
 *   _skipUpdate             lines 11148-11154
 */

function _tauriCall(cmd, args) {
    const t = window.__TAURI_INTERNALS__ || (window.__TAURI__ && window.__TAURI__.core);
    if (!t || !t.invoke) return Promise.reject('Not a Tauri app');
    return t.invoke(cmd, args || {});
}

setTimeout(_checkDesktopUpdate, 3000);
```

**Critical**: `_showUpdateBanner` and `_installUpdate` must use `getElementById('desktopUpdateBanner')` (not `'updateBanner'`) to avoid colliding with firmware's existing OTA banner (which has the same id). Update those references.

**Device Manager** — copy verbatim from pre-merge desktop lines 11182–11317 (≈136 lines): `openDeviceManager`, `closeDeviceManager`, `_dmRefresh`, `_dmRefreshHealth`, `_dmReboot`.

**ZIP backup/restore** — copy verbatim from pre-merge desktop lines 9733–9807: `backupAllDeviceLayouts`, `restoreAllDeviceLayouts`. Insert in the script block, ideally near firmware's `exportRdm`/`importRdm` definitions.

### Step 7 — Re-inject HTML elements

Inside `<body>`:

**Desktop update banner** — different id from firmware's, place after firmware's existing `updateBanner` (which stays for firmware OTA):

```html
<div id="desktopUpdateBanner" style="display:none;position:fixed;top:0;left:0;right:0;z-index:10000;background:linear-gradient(135deg,#1a3a2a,#1a2a3a);border-bottom:1px solid #2e5e3e;padding:10px 20px;align-items:center;gap:12px;font-size:12px;color:#ccc;">
    <span id="desktopUpdateIcon" style="font-size:16px;"></span>
    <span id="desktopUpdateMessage" style="flex:1;"></span>
    <button id="desktopUpdateInstallBtn" onclick="_installUpdate()" style="background:#4ade80;border:none;border-radius:6px;padding:5px 14px;color:#111;font-size:11px;font-weight:600;cursor:pointer;">Install</button>
    <button onclick="_skipUpdate()" style="background:transparent;border:1px solid #555;border-radius:6px;padding:5px 14px;color:#aaa;font-size:11px;cursor:pointer;">Skip</button>
</div>
```

Update `_showUpdateBanner` / `_installUpdate` to reference these new ids consistently.

**Download progress overlay** — just after the firmware's `<div id="toastContainer">`:

```html
<div id="dlProgress" class="dl-progress-overlay">
    <div class="dl-progress-label" id="dlProgressLabel">Downloading...</div>
    <div class="dl-progress-track"><div class="dl-progress-fill" id="dlProgressFill"></div></div>
</div>
```

**Connection bar in `<header>`** — placed before `<input type="file" id="importRdmInput">`:

```html
<div id="connectionBar" style="display:flex; align-items:center; gap:6px;">
    <select id="connectionMode" onchange="_onConnectionModeChange(this.value)"
        style="background:var(--bg); color:var(--text); border:1px solid var(--border); padding:4px 6px; border-radius:var(--radius-sm); font-size:10px; font-family:'JetBrains Mono',monospace;">
        <option value="local">Offline (Local)</option>
        <option value="wifi">WiFi</option>
        <option value="hotspot">ESP32 Hotspot</option>
        <option value="usb">USB Serial</option>
    </select>
    <span id="connModeLabel" style="font-size:10px; color:var(--text-muted);">Local</span>
    <button id="scanDevicesBtn" class="btn btn-secondary" onclick="_scanForDevices()"
        style="font-size:9px; padding:2px 6px; display:none;" title="Scan for RDM-7 devices on network">Scan</button>
</div>
<div class="header-divider"></div>
```

**Device Manager modal** — at the same level as other modals:

```html
<div id="deviceManagerModal" style="display:none; position:fixed; inset:0; z-index:9999; background:rgba(10,10,10,0.85); backdrop-filter:blur(8px); align-items:center; justify-content:center;" onclick="closeDeviceManager()">
    <div style="background:var(--panel-elevated); border:1px solid var(--border); border-radius:var(--radius-lg); padding:20px; width:480px; max-width:95vw; max-height:90vh; overflow-y:auto; box-shadow:var(--shadow-md);" onclick="event.stopPropagation()">
        <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:14px;">
            <span style="font-weight:600; font-size:14px;">Device Manager</span>
            <button class="close-btn" onclick="closeDeviceManager()" style="font-size:18px;">&times;</button>
        </div>
        <div id="dmContent" style="display:flex; flex-direction:column; gap:14px;">
            <div style="text-align:center; color:var(--text-muted); font-size:12px; padding:20px;">Loading...</div>
        </div>
    </div>
</div>
```

**Layout-menu items for Tauri-only features** — inside firmware's `<div id="layoutMenu">`:

```html
<div class="layout-menu-separator" id="backupRestoreSep" style="display:none;"></div>
<div class="layout-menu-item" onclick="backupAllDeviceLayouts(); closeLayoutMenu()" id="backupAllItem" style="display:none;">Backup all layouts to file...</div>
<div class="layout-menu-item" onclick="restoreAllDeviceLayouts(); closeLayoutMenu()" id="restoreAllItem" style="display:none;">Restore layouts from file...</div>
<div class="layout-menu-separator" id="importToDeviceSep" style="display:none;"></div>
<div class="layout-menu-item" onclick="openDeviceManager(); closeLayoutMenu()" id="deviceManagerItem">Device Manager...</div>
```

`_updateConnectionUI()` (in the connection block) toggles `display` on these items based on transport state.

### Step 8 — Boot wiring

After firmware's `/* Start */` block (the bit with `window.addEventListener('resize', () => zoomToFit());`), append:

```js
/* Tauri boot extras */
if (typeof initWasm === 'function') initWasm();

if (typeof _RDM_LOGO_RDMIMG_B64 !== 'undefined' && typeof RDM !== 'undefined') {
    RDM.getImageData('RDM').then(existing => {
        if (!existing) RDM.setImageData('RDM', _RDM_LOGO_RDMIMG_B64);
    }).catch(() => {});
}

setTimeout(_checkDesktopUpdate, 3000);
```

### Step 9 — Patch firmware-side functions for Tauri awareness

These functions exist in the firmware copy but need a Tauri branch added. Don't move them; patch in place.

| Function | Add Tauri branch |
|---|---|
| `downloadJson` | Use `RDM.saveFileDialog()` + `RDM.writeFile()` |
| `exportRdm` | Use `RDM.saveFileDialog()` + `RDM.writeFile()` |
| `importRdm` | Use `RDM.openFileDialog()` + `RDM.readFile()` |
| `openMarketplace` | Use `window.__TAURI_INTERNALS__.invoke('plugin:shell|open', ...)` to open in system browser |

Pattern:

```js
async function downloadJson(...) {
    if (typeof RDM !== 'undefined' && RDM.isTauri()) {
        /* Tauri path */
        const path = await RDM.saveFileDialog({ defaultName: '...', filters: [...] });
        if (!path) return;
        await RDM.writeFile(path, /* bytes */);
        return;
    }
    /* …firmware's existing blob+anchor logic… */
}
```

### Step 10 — Discard pre-merge desktop code

The following blocks from the pre-merge desktop file should NOT be re-injected. They're older versions of features the firmware now does better, or were intentionally retired:

| Pre-merge desktop lines | What | Why discard |
|---|---|---|
| 7271–7556 | Older client-side simulator (`_sim*` family + `openSimSettings`) | Firmware now simulates via fake CAN frames through `signal_dispatch_frame` |
| 11321–11758 | Older `openDeviceSettings`/`openSignalDashboard`/`openDataLogger`/`openFuelCalibration`/`openDimmerSettings` | Firmware has rewritten these against `/api/*` endpoints |
| 11770–11926 | Onboarding tour (`_TOUR_STEPS`, `startTour`, `_tourShow`, `_tourEnd`) | Replaced by `@rdm7/sandbox` Guided Tours |
| 56–86 | Boot splash (`#bootSplash`, `.splash-logo`) | Cosmetic only; firmware boots on real hardware where this isn't needed |
| 8–10 | Google Fonts preconnect `<link>` tags | Firmware uses `@import` inline; same effect |
| 3329–3334 | `_helpTab` | Firmware's help modal doesn't use it |
| 10306–10358 | Older `otaFirmwareUpdate` (RDM SDK chunked upload) | Firmware has its own `_otaShowInstall` flow |

## Test plan

After all 10 steps, before merging to desktop's main:

1. **Local mode**: launch desktop, switch to "Offline (Local)". Open a layout from disk. Edit. Save. Confirm WASM preview renders. No console errors about `RDM is not defined`.
2. **WiFi mode**: connect to a real device on the WiFi. Confirm `/api/layout/*` round-trip works. Edit a widget, save, confirm device updates.
3. **Hotspot mode**: same as WiFi but with the device's AP.
4. **USB mode**: connect device via USB. Confirm RDM SDK serial transport works. The `fetch` interceptor (step 1) is the load-bearing piece here.
5. **File-open**: drag a `.rdm` file onto the Tauri app icon (or double-click it on macOS). Confirm `_processRdmBytes` fires and the layout loads.
6. **Auto-updater**: cause a fake update to be available (modify `_DESKTOP_VERSION` lower than the released tag). Confirm banner appears with `id="desktopUpdateBanner"`, NOT firmware's `id="updateBanner"`.
7. **Device Manager**: open via layout-menu → Device Manager. Confirm it populates with device info.
8. **ZIP backup**: Backup all layouts to file. Restore from file. Verify layouts come back.

## Why this can't be a one-shot from the firmware repo

I (Claude in the firmware repo) extracted the delta and wrote this plan in a session without:

- A Tauri build environment (`cargo tauri dev`).
- Visibility into `transport.js` (which lives in the desktop repo).
- The ability to test each transport mode.

Steps 1 (the `transport.js` interceptor + `RDM.proxyApiCall`) and 9 (firmware-side function patches) require runtime verification. Doing them blind risks shipping a desktop app that opens fine in WiFi mode (because raw `fetch` to a real IP just works) but silently breaks in USB-serial or local mode.

## Files referenced

- `RDM-7_Dash/main/web/index.html` — firmware-canonical (the merge source).
- `rdm7-desktop/src/index.html` — desktop (the merge target; pre-merge content is the source of the Tauri delta).
- `rdm7-desktop/src/transport.js` — RDM SDK; the load-bearing dependency. The `fetch` interceptor must land here.
- `rdm7-desktop/src/build/index.js` — Emscripten LVGL WASM (referenced from step 3's `<script src=>`).
- `rdm7-desktop/src/rdm_logo_data.js` — bundled splash logo data (referenced from step 8).
