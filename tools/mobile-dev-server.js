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

const PORT = 8180;
const ROOT = path.resolve(__dirname, '..');
const INDEX_HTML = path.join(ROOT, 'main', 'web', 'index.html');

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
    model: 'RDM-7 Dash', version: '1.4.0-dev', ip: '192.168.4.1', hostname: '',
    mac: 'AA:BB:CC:DD:EE:FF', uptime_s: 1234, free_heap: 145000, free_psram: 6200000, chip: 'ESP32-S3'
  }),
  'GET  /api/storage/info':     () => ({ total: 8800000, used: 420000, free: 8380000 }),
  'GET  /api/image/list':       () => ({ images: [{ name: 'warning.rdmimg', size: 4200, w: 64, h: 64 }] }),
  'GET  /api/image/data':       () => '',
  'POST /api/image/delete':     () => ({ ok: true }),
  'POST /api/image/upload':     () => ({ ok: true }),
  'GET  /api/font/list':        () => (['Fugaz', 'Orbitron']),  /* firmware returns a bare array of family-name strings */
  'GET  /api/font/data':        () => '',
  'POST /api/font/upload':      () => ({ ok: true }),
  'GET  /api/sd/files':         () => ({ files: [] }),
  'POST /api/sd/copy':          () => ({ ok: true }),
  'POST /api/sd/delete':        () => ({ ok: true }),
  'GET  /api/log/status':       () => ({ active: false, file: '', samples: 0, rate_hz: 10, storage: 'lfs', sd_mounted: false, lfs_max_bytes: 1024*1024 }),
  'GET  /api/log/list':         () => ([
    { name: 'log_demo_lfs.csv', size: 44000, storage: 'lfs' }
  ]),
  'GET  /api/log/config':       () => ({ rate_hz: 50 }),
  'POST /api/log/config':       () => ({ ok: true }),
  'POST /api/log/start':        () => ({ ok: true }),
  'POST /api/log/stop':         () => ({ ok: true }),
  'POST /api/log/delete':       () => ({ ok: true }),
  'POST /api/log/upload':       () => ({ status: 'ok', name: 'uploaded.csv', size: 12345, storage: 'lfs' }),
  'GET  /api/canraw/status':    () => ({ active: false, file: '', frames: 0, elapsed_ms: 0, storage: 'lfs', lfs_max_bytes: 1024*1024, sd_mounted: false }),
  'POST /api/canraw/start':     () => ({ status: 'started' }),
  'POST /api/canraw/stop':      () => ({ status: 'stopped' }),
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

/* ── In-memory channel store (dev only) ────────────────────────────────────
 * Enough of /api/channels to exercise the Channels modal in the browser,
 * including the display-unit conversion UI (units_native vs units_display).
 * Values are NATIVE — the client converts for display, same as against the
 * device. /api/channels/update merges fields and echoes {channel} back like
 * the firmware does. */
const channelStore = [
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

  if (req.url.startsWith('/api/')) {
    /* Channels — explicit handlers (need body + the in-memory store). */
    if ((url === '/api/channels' || url === '/api/channels/active') && req.method === 'GET') {
      return sendJson(res, { channels: channelStore });
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
          Object.assign(c, data.fields || {});
          sendJson(res, { ok: true, channel: c });
        } catch (e) {
          sendJson(res, { ok: false, error: e.message }, 400);
        }
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

  const fp = path.join(ROOT, 'main', 'web', url);
  if (fp.startsWith(path.join(ROOT, 'main', 'web')) && fs.existsSync(fp) && fs.statSync(fp).isFile()) {
    const ext = path.extname(fp);
    const ct = { '.js': 'application/javascript', '.css': 'text/css', '.png': 'image/png', '.svg': 'image/svg+xml', '.html': 'text/html', '.ico': 'image/x-icon' }[ext] || 'application/octet-stream';
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
