#!/usr/bin/env node
/*
 * Mobile dev server for the firmware web UI.
 *
 * Serves main/web/index.html and mocks all /api/* endpoints so the editor
 * loads without needing a real ESP32 on the network. Iteration only — does
 * not execute layout logic, only returns plausible stub payloads.
 *
 * Run: node tools/mobile-dev-server.js
 * Open: http://localhost:8180  (use DevTools device emulation for phone/tablet)
 */
const http = require('http');
const fs = require('fs');
const path = require('path');

const _portArg = process.argv.indexOf('--port');
const PORT = _portArg >= 0 ? parseInt(process.argv[_portArg + 1], 10) : 8180;
const ROOT = path.resolve(__dirname, '..');

/* --index <path>: serve a different editor HTML (e.g. the desktop studio's
 * merged src/dist/index.html) with the same API mocks. Static assets
 * (transport.js, build/*.wasm, …) are served from that file's directory. */
const _idxArg = process.argv.indexOf('--index');
const INDEX_HTML = _idxArg >= 0
  ? path.resolve(process.argv[_idxArg + 1])
  : path.join(ROOT, 'main', 'web', 'index.html');
const STATIC_ROOT = path.dirname(INDEX_HTML);

/* Browser-dev layout persistence. POST /api/layout/save writes the firmware
 * payload here (pretty-printed) and GET /api/layout/current serves it back —
 * so layouts authored in the studio survive a page reload / server restart
 * without a device, and you can hand the file to anyone to Import. Delete the
 * file to fall back to SAMPLE_LAYOUT. */
const SAVED_LAYOUT = path.join(ROOT, 'tools', 'ford_cluster.json');

/* --device <ip>: proxy mode — serves the LOCAL main/web/index.html but
 * forwards every /api/* request to the real dash, so the editor under
 * development runs same-origin on localhost against live device data. */
const _devIdx = process.argv.indexOf('--device');
const DEVICE_IP = _devIdx >= 0 ? process.argv[_devIdx + 1] : null;

function proxyToDevice(req, res) {
  const opts = {
    host: DEVICE_IP, port: 80, path: req.url, method: req.method,
    headers: { ...req.headers, host: DEVICE_IP },
  };
  const up = http.request(opts, (ur) => {
    res.writeHead(ur.statusCode, ur.headers);
    ur.pipe(res);
  });
  up.on('error', (e) => {
    res.writeHead(502, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ error: 'device proxy failed: ' + e.message }));
  });
  req.pipe(up);
}

const SAMPLE_LAYOUT = {
  schema_version: 13,
  layout_name: 'default',
  screen: { w: 800, h: 480, bg: '#000000' },
  signals: [
    { name: 'RPM', can_id: 0x316, start_bit: 16, length: 16, scale: 0.25, offset: 0, unit: 'rpm', min: 0, max: 8000 },
    { name: 'SPEED', can_id: 0x153, start_bit: 8, length: 16, scale: 0.01, offset: 0, unit: 'km/h', min: 0, max: 250 },
    { name: 'COOLANT', can_id: 0x329, start_bit: 8, length: 8, scale: 1, offset: -40, unit: 'C', min: -40, max: 150 },
    { name: 'BOOST', can_id: 0x18E, start_bit: 0, length: 8, scale: 0.02, offset: -1, unit: 'bar', min: -1, max: 3 },
    { name: 'OIL_PRES', can_id: 0x19F, start_bit: 0, length: 8, scale: 0.1, offset: 0, unit: 'bar', min: 0, max: 10 }
  ],
  widgets: [
    { type: 'meter', slot: 0, x: -250, y: -80, w: 320, h: 320, signal_name: 'RPM', min: 0, max: 8000 },
    { type: 'panel', slot: 0, x: 200, y: -80, w: 260, h: 140, signal_name: 'SPEED', label: 'SPEED', unit: 'km/h' },
    { type: 'bar', slot: 0, x: 0, y: 180, w: 760, h: 40, signal_name: 'RPM', min: 0, max: 8000 }
  ]
};

const SAMPLE_SIGNALS_VALUES = SAMPLE_LAYOUT.signals.map((s, i) => ({
  name: s.name,
  value: [2340, 67.4, 88, 0.42, 3.8][i] || 0,
  unit: s.unit,
  fresh: true,
  min: [780, 0, 12, -0.3, 0.8][i] || 0,
  max: [6800, 142, 103, 1.8, 7.2][i] || 0
}));

/* Whether the stub dash is "recording" right now. Lives here so /api/log/start
 * and /api/canraw/start actually change what the status endpoints report. */
const recState = { log: false, logSince: 0, raw: false, rawSince: 0, rate: 10 };

const MOCK = {
  'GET  /api/layout/current':   () => SAMPLE_LAYOUT,
  'GET  /api/layout/raw':       () => SAMPLE_LAYOUT,
  'GET  /api/layout/list':      () => ({ layouts: ['default', 'track_day', 'dragstrip'], active: 'default' }),
  'GET  /api/layout/version':   () => ({ version: 42 }),
  'POST /api/layout/save':      () => ({ ok: true }),
  'POST /api/layout/delete':    () => ({ ok: true }),
  'POST /api/layout/rename':    () => ({ ok: true }),
  'POST /api/layout/preview':   () => ({ ok: true }),
  'GET  /api/signals/values':   () => ({ signals: SAMPLE_SIGNALS_VALUES }),
  'POST /api/signal/simulate':  () => ({ ok: true, sim: true }),
  'GET  /api/signal/simulate':  () => ({ sim: false }),
  'POST /api/signal/inject':    () => ({ ok: true }),
  'GET  /api/presets':          () => ([
    { ecu: 'MS3-Pro',    version: '1.5.x',      signals: [] },
    { ecu: 'Haltech',    version: 'Elite 2500', signals: [] },
    { ecu: 'MaxxECU',    version: '1.2',        signals: [] },
    { ecu: 'Ford BA/BF', version: 'stock',      signals: [] },
    { ecu: 'Ford FG',    version: 'stock',      signals: [] }
  ]),
  /* /api/presets/custom GET|save|delete handled explicitly below — they need
   * request body/query and an in-memory store so the editor's custom-preset
   * flow ("+ New ECU", "+ Add Signal", "Create Preset") actually persists
   * across requests during browser dev, matching the device's LittleFS. */
  'GET  /api/ecu/list':         () => ({ ecus: ['MS3-Pro', 'Haltech Elite', 'MaxxECU', 'Ford BA/BF', 'Ford FG'] }),
  'GET  /api/ecu/current':      () => ({ ecu: 'MS3-Pro', version: '1.5.x' }),
  'POST /api/ecu/set':          () => ({ ok: true }),
  'GET  /api/device/info':      () => ({
    /* mirrors web_server_system.c _device_info_handler — the discovery
     * sweep in the desktop app identifies a dash by the "serial" field */
    serial: 'MOCKDASH01', schema: 17,
    display: { width: 800, height: 480, shape: 'rect' },
    hardware: { chip: 'esp32s3', cores: 2, psram_mb: 8, flash_mb: 16 },
    system: { uptime_s: 1234, heap_free: 145000, heap_min_free: 98000, psram_free: 6200000, logger_active: false, replay_active: false },
    can: { state: 'running', rx_pending: 0, tx_errors: 0, rx_errors: 0, bus_errors: 0, rx_missed: 0 },
    wifi: { state: 'connected', ssid: 'MockNet', sta_ip: '127.0.0.1', ap_enabled: false, ap_ssid: '', ap_ip: '' },
    sd: { mounted: false },
    signals: { total: 5, fresh: 5, stale: 0 }
  }),
  'GET  /api/storage/info':     () => ({ total: 8800000, used: 420000, free: 8380000 }),
  'GET  /api/image/list':       () => ([{ name: 'warning.rdmimg', size: 4200, width: 64, height: 64 }]),  /* firmware returns a BARE array of {name,width,height,size} */
  /* image/data + font/data handled explicitly below: they must 404 (not
   * empty-200) or exportRdm silently embeds zero-byte assets. */
  'POST /api/image/delete':     () => ({ ok: true }),
  'POST /api/image/upload':     () => ({ ok: true }),
  'GET  /api/font/list':        () => (['Fugaz', 'Orbitron']),  /* firmware returns a bare array of family-name strings */
  'POST /api/font/upload':      () => ({ ok: true }),
  'GET  /api/sd/files':         () => ({ files: [] }),
  'POST /api/sd/copy':          () => ({ ok: true }),
  'POST /api/sd/delete':        () => ({ ok: true }),
  /* Recording state is held, not hardcoded: start/stop used to answer
     {ok:true} while status stayed active:false forever, so the Record button
     could be clicked but never showed a running state — the one thing worth
     checking about it. recState below is the store. */
  'GET  /api/log/status':       () => ({
    active: recState.log, file: recState.log ? 'log_dev_live.csv' : '',
    samples: recState.log ? Math.floor((Date.now() - recState.logSince) / 100) : 0,
    rate_hz: recState.rate, storage: 'lfs', sd_mounted: false,
    lfs_max_bytes: 1024*1024 }),
  'GET  /api/log/list':         () => ([
    { name: 'log_demo_lfs.csv', size: 44000, storage: 'lfs' }
  ]),
  'GET  /api/log/config':       () => ({ rate_hz: recState.rate }),
  'POST /api/log/config':       () => ({ ok: true }),
  'POST /api/log/start':        () => { recState.log = true; recState.logSince = Date.now(); return { ok: true }; },
  'POST /api/log/stop':         () => { recState.log = false; return { ok: true }; },
  'POST /api/log/delete':       () => ({ ok: true }),
  'POST /api/log/upload':       () => ({ status: 'ok', name: 'uploaded.csv', size: 12345, storage: 'lfs' }),
  'GET  /api/canraw/status':    () => ({
    active: recState.raw, file: recState.raw ? 'canraw_dev_live.csv' : '',
    frames: recState.raw ? Math.floor((Date.now() - recState.rawSince) / 2) : 0,
    elapsed_ms: recState.raw ? Date.now() - recState.rawSince : 0,
    storage: 'lfs', lfs_max_bytes: 1024*1024, sd_mounted: false }),
  'POST /api/canraw/start':     () => { recState.raw = true; recState.rawSince = Date.now(); return { status: 'started' }; },
  'POST /api/canraw/stop':      () => { recState.raw = false; return { status: 'stopped' }; },
  'GET  /api/replay/status':    () => ({ replaying: false, file: '', speed: 1, progress: 0 }),
  'POST /api/replay/start':     () => ({ ok: true }),
  'POST /api/replay/stop':      () => ({ ok: true }),
  'GET  /api/dimmer/config':    () => ({ auto: false, level: 80 }),
  'POST /api/dimmer/config':    () => ({ ok: true }),
  'GET  /api/splash/list':      () => ({ splashes: ['RDM Logo.png'], active: 'RDM Logo.png' }),
  'POST /api/splash/set':       () => ({ ok: true }),
  'POST /api/splash/fade':      () => ({ ok: true }),
  'POST /api/splash/delete':    () => ({ ok: true }),
  'POST /api/screen/switch':    () => ({ ok: true }),
  'GET  /api/fuel/status':      () => ({ calibrated: false, voltage: 1.75 + Math.random() * 0.05, empty_raw: 0, full_raw: 4095, level_pct: 42 }),
  'POST /api/fuel/set-empty':   () => ({ ok: true, voltage: 0.5 }),
  'POST /api/fuel/set-full':    () => ({ ok: true, voltage: 3.0 })
};

/* ── Channel store (dev, DISK-PERSISTED) ───────────────────────────────────
 * Enough of /api/channels to exercise the Channels modal in the browser,
 * including the display-unit conversion UI (units_native vs units_display).
 * Values are NATIVE — the client converts for display, same as against the
 * device. /api/channels/update merges fields and echoes {channel} back like
 * the firmware does.
 *
 * Persistence: tools/dev_channels.json (same idiom as ford_cluster.json).
 * That makes the offline workflow real: at the car hit "Backup Channels"
 * (or Export .rdm — it embeds channels.json), at home POST it to this
 * server's /api/channels/import (the Restore Channels card), edit in the
 * real Channels modal, then re-export for the dash. Delete the file to
 * fall back to the built-in mock set. */
const SAVED_CHANNELS = path.join(ROOT, 'tools', 'dev_channels.json');

/* Stand-in ECU catalog for /api/channels/source-options. Real makes with real
 * frames, trimmed to a few signals each — enough to drive the ECU column, the
 * version column, the tick list and the frame/bit line in each row. Only
 * kind:"ecu" rows carry a decode, which is what the picker filters on. */
const _ecuSig = (label, signal_name, can_id, bit_start, bit_length, scale, unit, decimals) =>
  ({ kind: 'ecu', label, signal_name, can_id, bit_start, bit_length,
     scale, offset: 0, is_signed: false, endian: 0, decimals, unit });
const DEV_ECU_MAKES = [
  { make: 'MaxxECU', is_active: true, versions: [
    { version: '1.3', display: 'MaxxECU 1.3', is_active: true, signals: [
      _ecuSig('RPM', 'RPM', 0x520, 0, 16, 1, 'rpm', 0),
      _ecuSig('Coolant Temp', 'COOLANT_TEMP', 0x521, 0, 16, 0.1, 'C', 1),
      _ecuSig('Manifold Pressure', 'MAP', 0x522, 16, 16, 0.1, 'kPa', 1),
      _ecuSig('Lambda', 'LAMBDA', 0x523, 0, 16, 0.001, '', 3),
      _ecuSig('Battery Voltage', 'BATTERY_VOLTAGE', 0x524, 0, 16, 0.01, 'V', 2),
      _ecuSig('Total Fuel Trim', 'TOTAL_FUEL_TRIM', 0x525, 0, 16, 0.1, '%', 1),
    ] },
    { version: '1.2', display: 'MaxxECU 1.2', is_active: false, signals: [
      _ecuSig('RPM', 'RPM', 0x520, 0, 16, 1, 'rpm', 0),
      _ecuSig('Coolant Temp', 'COOLANT_TEMP', 0x521, 0, 16, 0.1, 'C', 1),
    ] },
  ] },
  { make: 'Haltech', is_active: false, versions: [
    { version: 'Nexus', display: 'Haltech Nexus', is_active: false, signals: [
      _ecuSig('Engine RPM', 'ENGINE_RPM', 0x360, 0, 16, 1, 'rpm', 0),
      _ecuSig('Manifold Pressure', 'MANIFOLD_PRESSURE', 0x360, 16, 16, 0.1, 'kPa', 1),
      _ecuSig('Throttle Position', 'THROTTLE_POSITION', 0x360, 32, 16, 0.1, '%', 1),
      _ecuSig('Oil Pressure', 'OIL_PRESSURE', 0x372, 0, 16, 0.1, 'kPa', 1),
      _ecuSig('Battery Volt', 'BATTERY_VOLT', 0x372, 16, 16, 0.1, 'V', 1),
      _ecuSig('Ambient Air Temp', 'AMBIENT_AIR_TEMP', 0x3E0, 48, 16, 0.1, 'C', 1),
    ] },
  ] },
  { make: 'Link', is_active: false, versions: [
    { version: 'G4X', display: 'Link G4X', is_active: false, signals: [
      _ecuSig('Engine Speed', 'ENGINE_SPEED', 0x3E8, 0, 16, 1, 'rpm', 0),
      _ecuSig('MGP', 'MGP', 0x3E8, 16, 16, 0.1, 'kPa', 1),
      _ecuSig('TPS', 'TPS', 0x3E9, 0, 16, 0.1, '%', 1),
      _ecuSig('IAT', 'IAT', 0x3E9, 16, 16, 0.1, 'C', 1),
      _ecuSig('Gear Position', 'GEAR_POSITION', 0x3EA, 0, 8, 1, '', 0),
    ] },
  ] },
  /* Virtual buckets the real endpoint also returns. The picker must filter
   * these OUT — they carry no importable decode. */
  { make: 'OBD2', is_active: false, versions: [
    { version: 'Standard', display: 'OBD2 Standard (any 2008+ car)', is_active: false,
      signals: [{ kind: 'obd2', label: 'Engine RPM', signal_name: 'RPM', service: 1, pid: 0x0C, unit: 'rpm' }] },
  ] },
  { make: 'Custom', is_active: false, versions: [
    { version: 'User-defined', display: 'Custom CAN + DBC imports', is_active: false, signals: [] },
  ] },
];

const DEFAULT_CHANNELS = [
  { id: 'oil_pressure', label: 'Oil Pressure', group: 0, tier: 0, is_canonical: true,
    signal: 'OIL_PRES', source: 'can', units_native: 'kPa', units_display: 'bar', decimals: 2,
    min: 0, max: 1000, low_warn: 80, high_warn: 650, current_value: 203.9, is_stale: false },
  { id: 'coolant_temp', label: 'Coolant Temp', group: 0, tier: 0, is_canonical: true,
    signal: 'COOLANT', source: 'can', units_native: '°C', units_display: '°C', decimals: 0,
    min: -40, max: 150, low_warn: null, high_warn: 105, current_value: 88, is_stale: false },
  { id: 'vehicle_speed', label: 'Vehicle Speed', group: 2, tier: 0, is_canonical: true,
    signal: 'SPEED', source: 'can', units_native: 'km/h', units_display: 'km/h', decimals: 0,
    min: 0, max: 300, low_warn: null, high_warn: null, current_value: 67.4, is_stale: false },
  { id: 'custom_lambda', label: 'Lambda', group: 99, tier: 1, is_canonical: false,
    signal: 'LAMBDA', source: 'can', units_native: 'λ', units_display: '', decimals: 2,
    min: 0.6, max: 1.4, low_warn: 0.75, high_warn: 1.1, current_value: 0.98, is_stale: false }
];

/* Fuel-over-CAN forward config (dev, in-memory). Mirrors the firmware
 * defaults in config_store_load_fuel_forward(). */
let fuelForwardCfg = {
  enabled: false, can_id: 0x6F0, extd: false, endian: 1,
  bit_start: 0, bit_length: 16, rate_hz: 10, mode: 0, scale: 1, offset: 0,
};

function loadChannelStore() {
  try {
    if (fs.existsSync(SAVED_CHANNELS)) {
      const j = JSON.parse(fs.readFileSync(SAVED_CHANNELS, 'utf8'));
      if (Array.isArray(j.channels)) return j.channels;
      if (Array.isArray(j)) return j;
    }
  } catch (e) { console.log('[channels] failed to load dev_channels.json:', e.message); }
  return DEFAULT_CHANNELS;
}
let channelStore = loadChannelStore();
function persistChannelStore() {
  try {
    fs.writeFileSync(SAVED_CHANNELS,
      JSON.stringify({ schema_version: 3, channels: channelStore }, null, 2));
  } catch (e) { console.log('[channels] persist failed:', e.message); }
}
const CANONICAL_DEFS = [
  { id: 'oil_pressure', label: 'Oil Pressure', group: 0, tier: 0, units_native: 'kPa',
    units_display_default: 'bar', decimals: 2, min_default: 0, max_default: 1000,
    low_warn: 80, high_warn: 650, notes: 'Mock canonical def.' },
  { id: 'coolant_temp', label: 'Coolant Temp', group: 0, tier: 0, units_native: '°C',
    units_display_default: '°C', decimals: 0, min_default: -40, max_default: 150,
    low_warn: null, high_warn: 105, notes: '' },
  { id: 'vehicle_speed', label: 'Vehicle Speed', group: 2, tier: 0, units_native: 'km/h',
    units_display_default: 'km/h', decimals: 0, min_default: 0, max_default: 300,
    low_warn: null, high_warn: null, notes: '' }
];

/* ── In-memory custom-preset store (dev only) ──────────────────────────────
 * Mirrors the device's LittleFS custom presets. Each entry:
 *   { ecu, version, signals: [ { label, can_id, bit_start, bit_length,
 *                                scale, offset, endianess, is_signed, decimals } ] }
 * GET /api/presets/custom returns the FLAT array the firmware emits — one row
 * per signal (carrying ecu/version/label/decode), or a single { _empty:true }
 * placeholder row so an ECU with no signals still appears in the picker. */
const customPresetStore = [];

function flattenCustomPresets() {
  const out = [];
  for (const p of customPresetStore) {
    if (!p.signals || p.signals.length === 0) {
      out.push({ ecu: p.ecu, version: p.version, label: '', _empty: true });
      continue;
    }
    for (const s of p.signals) {
      out.push({
        ecu: p.ecu, version: p.version,
        label: s.label || '',
        can_id: (s.can_id != null ? String(s.can_id) : '0'),
        endianess: (s.endianess != null ? s.endianess : 1),
        bit_start: s.bit_start || 0,
        bit_length: (s.bit_length != null ? s.bit_length : 16),
        scale: (s.scale != null ? s.scale : 1),
        offset: s.offset || 0,
        decimals: s.decimals || 0,
        is_signed: !!s.is_signed
      });
    }
  }
  return out;
}

function readBody(req, cb) {
  let body = '';
  req.on('data', (chunk) => { body += chunk; });
  req.on('end', () => cb(body));
}

function sendJson(res, body, code = 200) {
  const json = typeof body === 'string' ? body : JSON.stringify(body);
  res.writeHead(code, {
    'Content-Type': 'application/json; charset=utf-8',
    'Access-Control-Allow-Origin': '*'
  });
  res.end(json);
}

function sendHtml(res, body, code = 200) {
  res.writeHead(code, {
    'Content-Type': 'text/html; charset=utf-8',
    'Cache-Control': 'no-store'
  });
  res.end(body);
}

const server = http.createServer((req, res) => {
  const url = req.url.split('?')[0];
  const key = `${req.method.padEnd(4)} ${url}`;
  const handler = MOCK[key];

  /* Device-proxy dev mode: serve the local editor, forward API to the dash. */
  if (DEVICE_IP) {
    if (url.startsWith('/api/')) {
      return proxyToDevice(req, res);
    }
  }

  if (req.url.startsWith('/api/')) {
    /* Layout persistence — serve the saved layout file if present so the
     * studio opens straight to the authored layout (no device needed). */
    if ((url === '/api/layout/current' || url === '/api/layout/raw') && req.method === 'GET') {
      try {
        if (fs.existsSync(SAVED_LAYOUT)) return sendJson(res, fs.readFileSync(SAVED_LAYOUT, 'utf8'));
      } catch (e) { /* fall through to sample */ }
      return sendJson(res, SAMPLE_LAYOUT);
    }
    if (url === '/api/layout/save' && req.method === 'POST') {
      return readBody(req, (body) => {
        try {
          const pretty = JSON.stringify(JSON.parse(body), null, 2);
          fs.writeFileSync(SAVED_LAYOUT, pretty);
        } catch (e) { /* ignore malformed save */ }
        sendJson(res, { ok: true });
      });
    }
    /* Dev-only: dump the studio preview SVG to disk so it can be shared. */
    if (url === '/api/preview/svg' && req.method === 'POST') {
      return readBody(req, (body) => {
        try { fs.writeFileSync(path.join(ROOT, 'tools', 'ford_cluster_preview.svg'), body); } catch (e) {}
        sendJson(res, { ok: true });
      });
    }
    /* Dev-only: decode a {name, dataurl} data: URL (png/jpeg) to tools/_icons/
     * so studio-rasterized images can be pulled to disk for device upload /
     * side-by-side diffing. */
    if (url === '/api/_save' && req.method === 'POST') {
      return readBody(req, (body) => {
        try {
          const d = JSON.parse(body || '{}');
          const b64 = String(d.dataurl || '').replace(/^data:[^,]+,/, '');
          const dir = path.join(ROOT, 'tools', '_icons');
          if (!fs.existsSync(dir)) fs.mkdirSync(dir, { recursive: true });
          fs.writeFileSync(path.join(dir, d.name), Buffer.from(b64, 'base64'));
          sendJson(res, { ok: true, bytes: Buffer.from(b64, 'base64').length });
        } catch (e) { sendJson(res, { ok: false, error: e.message }, 400); }
      });
    }
    /* Channels — explicit handlers (need body + the persisted store). */
    if ((url === '/api/channels' || url === '/api/channels/active') && req.method === 'GET') {
      /* ?stub=1 mimics RDM Studio's local-mode answer: a truthful HTTP 200
         carrying offline:true. Lets the editor's "a 200 is not proof of a
         dash" branch be exercised in the browser. */
      if (/[?&]stub=1/.test(req.url))
        return sendJson(res, { channels: [], capacity: 128, offline: true });
      return sendJson(res, { count: channelStore.length, capacity: 128, channels: channelStore });
    }
    if (url === '/api/channels/canonical' && req.method === 'GET') {
      return sendJson(res, { channels: CANONICAL_DEFS });
    }
    if (url === '/api/channels/update' && req.method === 'POST') {
      return readBody(req, (body) => {
        try {
          const data = JSON.parse(body || '{}');
          const c = channelStore.find(x => x.id === data.id);
          if (!c) return sendJson(res, { ok: false, error: 'unknown channel' }, 404);
          const fields = data.fields || {};
          /* Mirror the firmware's math handling so the Calculated-channel
           * form is exercisable in browser dev. */
          if ('math' in fields) {
            if (fields.math == null) {
              delete c.math; c.source = 'unbound'; c.signal = '';
            } else {
              c.math = fields.math; c.source = 'math';
              c.signal = 'MATH_' + c.id.toUpperCase();
            }
            delete fields.math;
          }
          /* Decode arrives as a partial patch — merge like the firmware. */
          if ('decode' in fields) {
            c.decode = Object.assign({}, c.decode || {}, fields.decode || {});
            delete fields.decode;
          }
          Object.assign(c, fields);
          persistChannelStore();
          sendJson(res, { ok: true, channel: c });
        } catch (e) {
          sendJson(res, { ok: false, error: e.message }, 400);
        }
      });
    }
    if (url === '/api/channels/activate' && req.method === 'POST') {
      return readBody(req, (body) => {
        try {
          const data = JSON.parse(body || '{}');
          const def = CANONICAL_DEFS.find(d => d.id === data.id);
          if (!def) return sendJson(res, { ok: false, error: 'Not a canonical channel id' }, 404);
          let c = channelStore.find(x => x.id === data.id);
          if (!c) {
            c = { id: def.id, label: def.label, group: def.group, tier: def.tier,
                  is_canonical: true, signal: '', source: 'unbound',
                  units_native: def.units_native, units_display: def.units_display_default,
                  decimals: def.decimals, min: def.min_default, max: def.max_default,
                  low_warn: def.low_warn, high_warn: def.high_warn,
                  current_value: null, is_stale: true };
            channelStore.push(c);
            persistChannelStore();
          }
          sendJson(res, { ok: true, channel: c });
        } catch (e) { sendJson(res, { ok: false, error: e.message }, 400); }
      });
    }
    if (url === '/api/channels/create' && req.method === 'POST') {
      return readBody(req, (body) => {
        try {
          const d = JSON.parse(body || '{}');
          if (!d.label) return sendJson(res, { ok: false, error: 'label required' }, 400);
          const id = 'custom_' + String(d.label).toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '');
          if (channelStore.find(x => x.id === id))
            return sendJson(res, { ok: false, error: 'already exists' }, 400);
          const c = { id, label: d.label, group: d.group ?? 11, tier: 1, is_canonical: false,
                      signal: String(d.label).toUpperCase().replace(/[^A-Z0-9]+/g, '_'),
                      source: 'can', units_native: d.units || '', units_display: d.units || '',
                      decimals: d.decimals ?? 0, min: d.min ?? 0, max: d.max ?? 100,
                      low_warn: null, high_warn: null, decode: d.decode || null,
                      current_value: null, is_stale: true };
          channelStore.push(c);
          persistChannelStore();
          sendJson(res, { ok: true, channel: c });
        } catch (e) { sendJson(res, { ok: false, error: e.message }, 400); }
      });
    }
    if (url === '/api/channels/delete' && req.method === 'POST') {
      return readBody(req, (body) => {
        try {
          const d = JSON.parse(body || '{}');
          if (Array.isArray(d.ids)) {
            let deleted = 0, skipped = 0;
            d.ids.forEach(id => {
              const i = channelStore.findIndex(x => x.id === id && !x.is_canonical);
              if (i >= 0) { channelStore.splice(i, 1); deleted++; } else skipped++;
            });
            persistChannelStore();
            return sendJson(res, { ok: true, deleted, skipped });
          }
          const i = channelStore.findIndex(x => x.id === d.id);
          if (i < 0) return sendJson(res, { ok: false, error: 'Channel not found' }, 404);
          if (channelStore[i].is_canonical)
            return sendJson(res, { ok: false, error: 'Canonical channels cannot be deleted' }, 403);
          channelStore.splice(i, 1);
          persistChannelStore();
          sendJson(res, { ok: true });
        } catch (e) { sendJson(res, { ok: false, error: e.message }, 400); }
      });
    }
    /* Channel backup/restore — the firmware streams raw channels.json;
     * here the store IS the file. Restore skips the firmware's reboot. */
    if (url === '/api/channels/export' && req.method === 'GET') {
      return sendJson(res, JSON.stringify({ schema_version: 3, channels: channelStore }, null, 2));
    }
    if (url === '/api/channels/import' && req.method === 'POST') {
      return readBody(req, (body) => {
        try {
          const j = JSON.parse(body || '{}');
          if (!Array.isArray(j.channels)) return sendJson(res, { ok: false, error: 'no channels[]' }, 400);
          channelStore = j.channels;
          persistChannelStore();
          console.log(`[channels] imported ${channelStore.length} channels -> ${SAVED_CHANNELS}`);
          sendJson(res, { ok: true, count: channelStore.length });
        } catch (e) { sendJson(res, { ok: false, error: e.message }, 400); }
      });
    }
    /* Source options + bulk preset import — enough of the real shapes to
     * exercise "Add channels > From a pre-defined ECU" in the browser. The
     * firmware answers from its preconfig catalog and resolves each label
     * through the ECU alias table; here a handful of real MaxxECU/Haltech/
     * Link rows stand in, and import-preset just mints a channel per label
     * so the picker's counts and refresh path are honest. */
    if (url.startsWith('/api/channels/source-options') && req.method === 'GET') {
      return sendJson(res, { makes: DEV_ECU_MAKES, current_signal: '' });
    }
    if (url === '/api/channels/import-preset' && req.method === 'POST') {
      return readBody(req, (body) => {
        try {
          const d = JSON.parse(body || '{}');
          const mk = DEV_ECU_MAKES.find(m => m.make === d.make);
          const vr = mk && (mk.versions || []).find(v => v.version === d.version);
          if (!vr) return sendJson(res, { error: 'no such make/version' }, 404);
          const want = Array.isArray(d.labels) && d.labels.length
            ? vr.signals.filter(s => d.labels.includes(s.label))
            : vr.signals;
          let applied = 0;
          want.forEach(s => {
            const id = 'custom_' + s.signal_name.toLowerCase();
            if (channelStore.find(c => c.id === id)) return;
            channelStore.push({
              id, label: s.label, group: 11, units_native: s.unit || '',
              units_display: s.unit || '', decimals: s.decimals | 0,
              signal: s.signal_name, min: 0, max: 100, is_stale: true,
              decode: {
                can_id: s.can_id, bit_start: s.bit_start,
                bit_length: s.bit_length, scale: s.scale,
                offset: s.offset, is_signed: !!s.is_signed, endian: s.endian,
              },
            });
            applied++;
          });
          persistChannelStore();
          console.log(`[channels] import-preset ${d.make}/${d.version}: +${applied}`);
          sendJson(res, { status: 'ok', applied, make: d.make, version: d.version });
        } catch (e) { sendJson(res, { error: e.message }, 400); }
      });
    }
    /* Asset data must 404 when absent — an empty 200 makes exportRdm embed
     * zero-byte images/fonts with no warning. */
    if ((url.startsWith('/api/image/data') || url.startsWith('/api/font/data')) && req.method === 'GET') {
      res.writeHead(404);
      return res.end('no asset data in browser dev');
    }
    /* Fuel-over-CAN forward — modelled properly (not via the {ok:true}
     * catch-all) so the editor's availability probe, field round-trip and
     * range validation are all exercised in browser dev. */
    if (url === '/api/fuel/forward' && req.method === 'GET') {
      return sendJson(res, { ...fuelForwardCfg, tx_ok: 0, tx_fail: 0 });
    }
    if (url === '/api/fuel/forward' && req.method === 'POST') {
      return readBody(req, (body) => {
        try {
          const d = JSON.parse(body || '{}');
          const merged = { ...fuelForwardCfg, ...d };
          const idMax = merged.extd ? 0x1FFFFFFF : 0x7FF;
          if (!merged.can_id || merged.can_id > idMax)
            return sendJson(res, { ok: false, error: 'CAN id out of range' }, 400);
          if (merged.bit_start > 63 || merged.bit_length < 1 ||
              merged.bit_length > 32 || merged.bit_start + merged.bit_length > 64)
            return sendJson(res, { ok: false, error: 'Bit field out of range' }, 400);
          fuelForwardCfg = merged;
          sendJson(res, { ok: true });
        } catch (e) { sendJson(res, { ok: false, error: e.message }, 400); }
      });
    }
    /* Custom presets — explicit handlers (need body/query + persistence). */
    if (url === '/api/presets/custom' && req.method === 'GET') {
      return sendJson(res, flattenCustomPresets());
    }
    if (url === '/api/presets/custom/save' && req.method === 'POST') {
      return readBody(req, (body) => {
        try {
          const data = JSON.parse(body || '{}');
          const ecu = data.ecu, version = data.version;
          if (!ecu || !version) return sendJson(res, { ok: false, error: 'ecu and version required' }, 400);
          const signals = Array.isArray(data.signals) ? data.signals : [];
          const existing = customPresetStore.find(p => p.ecu === ecu && p.version === version);
          if (existing) existing.signals = signals;
          else customPresetStore.push({ ecu, version, signals });
          sendJson(res, { ok: true });
        } catch (e) {
          sendJson(res, { ok: false, error: e.message }, 400);
        }
      });
    }
    if (url === '/api/presets/custom/delete' && req.method === 'POST') {
      const q = new URLSearchParams(req.url.split('?')[1] || '');
      const ecu = q.get('ecu'), version = q.get('version');
      const idx = customPresetStore.findIndex(p => p.ecu === ecu && p.version === version);
      if (idx >= 0) customPresetStore.splice(idx, 1);
      return sendJson(res, { ok: true });
    }
    if (handler) return sendJson(res, handler());
    console.log(`[mock] no handler for ${key} — returning {ok:true}`);
    return sendJson(res, { ok: true });
  }

  if (url === '/' || url === '/index.html') {
    try {
      const html = fs.readFileSync(INDEX_HTML, 'utf8');
      return sendHtml(res, html);
    } catch (e) {
      return sendHtml(res, `<h1>Cannot read ${INDEX_HTML}</h1><pre>${e.message}</pre>`, 500);
    }
  }

  const fp = path.join(STATIC_ROOT, url);
  if (fp.startsWith(STATIC_ROOT) && fs.existsSync(fp) && fs.statSync(fp).isFile()) {
    const ext = path.extname(fp);
    const ct = { '.js': 'application/javascript', '.css': 'text/css', '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg', '.svg': 'image/svg+xml', '.html': 'text/html', '.ico': 'image/x-icon', '.wasm': 'application/wasm' }[ext] || 'application/octet-stream';
    res.writeHead(200, { 'Content-Type': ct });
    return res.end(fs.readFileSync(fp));
  }

  res.writeHead(404);
  res.end('Not found');
});

server.listen(PORT, () => {
  console.log(`RDM-7 mobile dev server: http://localhost:${PORT}`);
  console.log(`Serving ${INDEX_HTML}`);
  console.log(`Open DevTools > Toggle device toolbar for mobile viewport.`);
});
