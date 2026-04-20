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
  'GET  /api/presets/custom':   () => ([]),
  'POST /api/presets/custom/save':   () => ({ ok: true }),
  'POST /api/presets/custom/delete': () => ({ ok: true }),
  'GET  /api/ecu/list':         () => ({ ecus: ['MS3-Pro', 'Haltech Elite', 'MaxxECU', 'Ford BA/BF', 'Ford FG'] }),
  'GET  /api/ecu/current':      () => ({ ecu: 'MS3-Pro', version: '1.5.x' }),
  'POST /api/ecu/set':          () => ({ ok: true }),
  'GET  /api/device/info':      () => ({
    model: 'RDM-7 Dash', version: '1.4.0-dev', ip: '192.168.4.1', hostname: 'rdm7.local',
    mac: 'AA:BB:CC:DD:EE:FF', uptime_s: 1234, free_heap: 145000, free_psram: 6200000, chip: 'ESP32-S3'
  }),
  'GET  /api/storage/info':     () => ({ total: 8800000, used: 420000, free: 8380000 }),
  'GET  /api/image/list':       () => ({ images: [{ name: 'warning.rdmimg', size: 4200, w: 64, h: 64 }] }),
  'GET  /api/image/data':       () => '',
  'POST /api/image/delete':     () => ({ ok: true }),
  'POST /api/image/upload':     () => ({ ok: true }),
  'GET  /api/font/list':        () => ({ fonts: [{ family: 'Fugaz', sizes: [16, 24, 32] }, { family: 'Orbitron', sizes: [18, 28] }] }),
  'GET  /api/font/data':        () => '',
  'POST /api/font/upload':      () => ({ ok: true }),
  'GET  /api/sd/files':         () => ({ files: [] }),
  'POST /api/sd/copy':          () => ({ ok: true }),
  'POST /api/sd/delete':        () => ({ ok: true }),
  'GET  /api/log/status':       () => ({ logging: false, file: '', samples: 0 }),
  'GET  /api/log/list':         () => ({ logs: [{ name: '2026-04-19_12-00.csv', size: 44000 }] }),
  'GET  /api/log/config':       () => ({ rate_hz: 50 }),
  'POST /api/log/config':       () => ({ ok: true }),
  'POST /api/log/start':        () => ({ ok: true }),
  'POST /api/log/stop':         () => ({ ok: true }),
  'POST /api/log/delete':       () => ({ ok: true }),
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
  'GET  /api/fuel/status':      () => ({ calibrated: false, empty_raw: 0, full_raw: 4095, level_pct: 42 }),
  'POST /api/fuel/set-empty':   () => ({ ok: true }),
  'POST /api/fuel/set-full':    () => ({ ok: true })
};

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
    const ct = { '.js': 'application/javascript', '.css': 'text/css', '.png': 'image/png', '.svg': 'image/svg+xml', '.html': 'text/html' }[ext] || 'application/octet-stream';
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
